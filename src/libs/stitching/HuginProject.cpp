#include "hstream/src/libs/stitching/HuginProject.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "hstream/src/libs/common/Process.h"

extern "C" char** environ;

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMinimumUsableMatches = 16;

const std::array<const char*, 8> kRequiredArtifacts = {
    "hm_project.pto",
    "autooptimiser_out.pto",
    "mapping_0000.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
};

const std::array<const char*, 2> kOptionalArtifacts = {"seam_file.png", "panorama.tif"};

absl::StatusOr<std::string> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return absl::NotFoundError("Unable to read Hugin file: " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof())
    return absl::InternalError("Failed reading Hugin file: " + path.string());
  return contents.str();
}

absl::Status write_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to write Hugin file: " + path.string());
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output)
    return absl::InternalError("Failed writing Hugin file: " + path.string());
  return absl::OkStatus();
}

std::map<std::string, std::string> environment() {
  std::map<std::string, std::string> values;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string item(*entry);
    const size_t separator = item.find('=');
    if (separator != std::string::npos)
      values[item.substr(0, separator)] = item.substr(separator + 1);
  }
  return values;
}

absl::StatusOr<std::string> executable(const char* override_name, const char* name) {
  if (const char* override_path = std::getenv(override_name); override_path != nullptr && *override_path != '\0') {
    if (::access(override_path, X_OK) == 0)
      return std::string(override_path);
    return absl::NotFoundError(std::string(override_name) + " is not executable: " + override_path);
  }
  const fs::path system_path = fs::path("/usr/bin") / name;
  if (::access(system_path.c_str(), X_OK) == 0)
    return system_path.string();
  auto found = hm::findExecutable(name, {"PATH"});
  if (found.has_value() && ::access(found->c_str(), X_OK) == 0)
    return *found;
  return absl::NotFoundError(std::string("Required Hugin executable not found: ") + name);
}

absl::Status run_checked(const std::vector<std::string>& command, const fs::path& working_dir) {
  const int exit_code = hm::run_command(
      command, working_dir.string(), environment(), [](const std::string& error, const std::string& output) {
        if (!error.empty())
          std::cerr << error << '\n';
        if (!output.empty())
          std::cout << output << '\n';
      });
  if (exit_code != 0) {
    std::ostringstream message;
    message << "Command failed with exit code " << exit_code << ':';
    for (const std::string& argument : command)
      message << ' ' << argument;
    return absl::InternalError(message.str());
  }
  return absl::OkStatus();
}

absl::Status validate_nonempty_file(const fs::path& path) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error) {
    return absl::NotFoundError("Expected Hugin artifact is missing: " + path.string());
  }
  if (fs::file_size(path, error) == 0 || error) {
    return absl::FailedPreconditionError("Expected Hugin artifact is empty: " + path.string());
  }
  return absl::OkStatus();
}

absl::StatusOr<fs::path> make_staging_directory(const fs::path& game_dir) {
  std::string pattern = (game_dir / ".hmstream-stitch-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    return absl::InternalError(
        "Unable to create stitch staging directory under " + game_dir.string() + ": " + std::strerror(errno));
  }
  if (::chmod(created, 0700) != 0) {
    std::error_code ignored;
    fs::remove_all(created, ignored);
    return absl::InternalError("Unable to make stitch staging directory private");
  }
  return fs::path(created);
}

void remove_mapping_outputs(const fs::path& directory) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("mapping_", 0) == 0 && (entry.path().extension() == ".tif" || entry.path().extension() == ".tiff")) {
      fs::remove(entry.path(), error);
      error.clear();
    }
  }
}

absl::Status run_autooptimiser(const std::string& autooptimiser, const fs::path& directory) {
  std::vector<std::string> command = {autooptimiser, "-a", "-l", "-s", "-o", "autooptimiser_out.pto", "hm_project.pto"};
  return run_checked(command, directory);
}

absl::Status scale_canvas(const std::string& pano_modify, const fs::path& directory, size_t width, size_t height) {
  if (width == 0 || height == 0)
    return absl::InvalidArgumentError("Scaled Hugin canvas dimensions must be positive");
  const std::string temporary = "autooptimiser_scaled.pto";
  auto status = run_checked(
      {pano_modify,
       "--canvas=" + std::to_string(width) + "x" + std::to_string(height),
       "-o",
       temporary,
       "autooptimiser_out.pto"},
      directory);
  if (!status.ok())
    return status;
  status = validate_nonempty_file(directory / temporary);
  if (!status.ok())
    return status;
  std::error_code error;
  fs::rename(directory / temporary, directory / "autooptimiser_out.pto", error);
  if (error)
    return absl::InternalError("Unable to publish scaled Hugin project: " + error.message());
  return absl::OkStatus();
}

absl::Status run_nona(const std::string& nona, const fs::path& directory) {
  remove_mapping_outputs(directory);
  return run_checked(
      {nona, "-m", "TIFF_m", "-z", "NONE", "--bigtiff", "-c", "-o", "mapping_", "autooptimiser_out.pto"}, directory);
}

absl::Status publish_artifacts(const fs::path& staging, const fs::path& game_dir) {
  std::vector<std::string> names(kRequiredArtifacts.begin(), kRequiredArtifacts.end());
  for (const char* optional : kOptionalArtifacts)
    names.emplace_back(optional);

  const fs::path backups = staging / "previous";
  std::error_code error;
  fs::create_directory(backups, error);
  if (error)
    return absl::InternalError("Unable to prepare stitch artifact rollback directory: " + error.message());

  std::vector<std::string> moved_old;
  std::vector<std::string> published;
  auto rollback = [&]() {
    std::error_code ignored;
    for (auto it = published.rbegin(); it != published.rend(); ++it) {
      if (fs::exists(game_dir / *it, ignored))
        fs::rename(game_dir / *it, staging / *it, ignored);
      ignored.clear();
    }
    for (auto it = moved_old.rbegin(); it != moved_old.rend(); ++it) {
      if (fs::exists(backups / *it, ignored))
        fs::rename(backups / *it, game_dir / *it, ignored);
      ignored.clear();
    }
  };

  for (const std::string& name : names) {
    if (!fs::exists(game_dir / name, error)) {
      error.clear();
      continue;
    }
    fs::rename(game_dir / name, backups / name, error);
    if (error) {
      rollback();
      return absl::InternalError("Unable to preserve previous stitch artifact " + name + ": " + error.message());
    }
    moved_old.push_back(name);
  }
  for (const std::string& name : names) {
    if (!fs::exists(staging / name, error)) {
      error.clear();
      continue;
    }
    fs::rename(staging / name, game_dir / name, error);
    if (error) {
      rollback();
      return absl::InternalError("Unable to publish stitch artifact " + name + ": " + error.message());
    }
    published.push_back(name);
  }
  return absl::OkStatus();
}

} // namespace

absl::StatusOr<std::string> HuginProject::InsertControlPoints(
    const std::string& pto,
    const std::vector<FeatureMatch>& matches) {
  if (matches.size() < kMinimumUsableMatches) {
    return absl::FailedPreconditionError(
        "Feature matcher produced " + std::to_string(matches.size()) + ", fewer than the required " +
        std::to_string(kMinimumUsableMatches) + " control points");
  }
  std::ostringstream points;
  points.imbue(std::locale::classic());
  points << std::setprecision(std::numeric_limits<float>::max_digits10);
  for (const FeatureMatch& match : matches) {
    if (!std::isfinite(match.left.x) || !std::isfinite(match.left.y) || !std::isfinite(match.right.x) ||
        !std::isfinite(match.right.y) || match.left.x < 0.0f || match.left.y < 0.0f || match.right.x < 0.0f ||
        match.right.y < 0.0f) {
      return absl::InvalidArgumentError("Control points must contain finite non-negative coordinates");
    }
    points << "c n0 N1 x" << match.left.x << " y" << match.left.y << " X" << match.right.x << " Y" << match.right.y
           << " t0\n";
  }

  std::istringstream input(pto);
  std::ostringstream output;
  std::string line;
  bool marker_seen = false;
  while (std::getline(input, line)) {
    if (line.rfind("c ", 0) == 0)
      continue;
    output << line << '\n';
    if (!marker_seen && line == "# control points") {
      output << points.str();
      marker_seen = true;
    }
  }
  if (!marker_seen)
    return absl::InvalidArgumentError("Hugin PTO has no control-point marker");
  return output.str();
}

absl::StatusOr<std::pair<size_t, size_t>> HuginProject::ParseCanvasSize(const std::string& pto) {
  static const std::regex width_pattern(R"((?:^|[[:space:]])w([0-9]+)(?:[[:space:]]|$))");
  static const std::regex height_pattern(R"((?:^|[[:space:]])h([0-9]+)(?:[[:space:]]|$))");
  std::istringstream input(pto);
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("p ", 0) != 0)
      continue;
    std::smatch width_match;
    std::smatch height_match;
    if (!std::regex_search(line, width_match, width_pattern) ||
        !std::regex_search(line, height_match, height_pattern)) {
      return absl::InvalidArgumentError("Hugin panorama line has no valid canvas dimensions");
    }
    try {
      const size_t width = std::stoull(width_match[1].str());
      const size_t height = std::stoull(height_match[1].str());
      if (width == 0 || height == 0)
        throw std::out_of_range("zero canvas");
      return std::make_pair(width, height);
    } catch (const std::exception&) {
      return absl::InvalidArgumentError("Hugin panorama canvas dimensions are invalid");
    }
  }
  return absl::InvalidArgumentError("Hugin PTO has no panorama line");
}

absl::Status HuginProject::Configure(
    const fs::path& game_dir,
    const std::vector<FeatureMatch>& matches,
    const Options& options) {
  if (!std::isfinite(options.horizontal_fov) || options.horizontal_fov <= 0.0 || options.horizontal_fov >= 360.0) {
    return absl::InvalidArgumentError("Hugin horizontal field of view must be between 0 and 360 degrees");
  }
  if (matches.size() < kMinimumUsableMatches) {
    return absl::FailedPreconditionError("Insufficient control points for Hugin optimization");
  }
  for (const char* image : {"left.png", "right.png"}) {
    auto status = validate_nonempty_file(game_dir / image);
    if (!status.ok())
      return status;
  }

  fs::path staging;
  auto staging_result = make_staging_directory(game_dir);
  if (!staging_result.ok())
    return staging_result.status();
  staging = *staging_result;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};

  std::error_code error;
  fs::copy_file(game_dir / "left.png", staging / "left.png", fs::copy_options::overwrite_existing, error);
  if (error)
    return absl::InternalError("Unable to stage left image: " + error.message());
  fs::copy_file(game_dir / "right.png", staging / "right.png", fs::copy_options::overwrite_existing, error);
  if (error)
    return absl::InternalError("Unable to stage right image: " + error.message());

  auto pto_gen = executable("HM_PTO_GEN", "pto_gen");
  if (!pto_gen.ok())
    return pto_gen.status();
  auto autooptimiser = executable("HM_AUTOOPTIMISER", "autooptimiser");
  if (!autooptimiser.ok())
    return autooptimiser.status();
  auto nona = executable("HM_NONA", "nona");
  if (!nona.ok())
    return nona.status();

  std::ostringstream fov;
  fov.imbue(std::locale::classic());
  fov << std::setprecision(12) << options.horizontal_fov;
  auto status =
      run_checked({*pto_gen, "-p", "0", "-o", "hm_project.pto", "-f", fov.str(), "left.png", "right.png"}, staging);
  if (!status.ok())
    return status;
  auto project = read_file(staging / "hm_project.pto");
  if (!project.ok())
    return project.status();
  auto with_points = InsertControlPoints(*project, matches);
  if (!with_points.ok())
    return with_points.status();
  status = write_file(staging / "hm_project.pto", *with_points);
  if (!status.ok())
    return status;

  status = run_autooptimiser(*autooptimiser, staging);
  if (!status.ok())
    return status;
  std::optional<std::string> pano_modify;
  auto fit_canvas = [&](size_t width, size_t height, double rounding_guard) -> absl::Status {
    if (!pano_modify.has_value()) {
      auto resolved = executable("HM_PANO_MODIFY", "pano_modify");
      if (!resolved.ok())
        return resolved.status();
      pano_modify = std::move(*resolved);
    }
    const size_t longest = std::max(width, height);
    const double factor =
        static_cast<double>(*options.max_canvas_dimension) / static_cast<double>(longest) * rounding_guard;
    const size_t scaled_width = std::max<size_t>(1, static_cast<size_t>(std::floor(width * factor)));
    const size_t scaled_height = std::max<size_t>(1, static_cast<size_t>(std::floor(height * factor)));
    return scale_canvas(*pano_modify, staging, scaled_width, scaled_height);
  };
  if (options.max_canvas_dimension.has_value()) {
    auto optimized = read_file(staging / "autooptimiser_out.pto");
    if (!optimized.ok())
      return optimized.status();
    auto dimensions = ParseCanvasSize(*optimized);
    if (!dimensions.ok())
      return dimensions.status();
    const size_t longest = std::max(dimensions->first, dimensions->second);
    if (longest > *options.max_canvas_dimension) {
      status = fit_canvas(dimensions->first, dimensions->second, 1.0);
      if (!status.ok())
        return status;
    }
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    status = run_nona(*nona, staging);
    if (!status.ok())
      return status;
    bool mappings_valid = true;
    for (size_t index = 2; index < kRequiredArtifacts.size(); ++index) {
      status = validate_nonempty_file(staging / kRequiredArtifacts[index]);
      if (!status.ok()) {
        mappings_valid = false;
        break;
      }
    }
    if (!mappings_valid)
      return status;
    if (!options.max_canvas_dimension.has_value())
      break;

    // Nona derives its mapping canvas from the optimized PTO. Validate that
    // exact final contract and retry with a small rounding guard if necessary.
    auto optimized = read_file(staging / "autooptimiser_out.pto");
    if (!optimized.ok())
      return optimized.status();
    auto dimensions = ParseCanvasSize(*optimized);
    if (!dimensions.ok())
      return dimensions.status();
    const size_t longest = std::max(dimensions->first, dimensions->second);
    if (longest <= *options.max_canvas_dimension)
      break;
    if (attempt == 2) {
      return absl::FailedPreconditionError("Hugin mapping canvas still exceeds maximum dimension after three attempts");
    }
    status = fit_canvas(dimensions->first, dimensions->second, 0.999);
    if (!status.ok())
      return status;
  }

  // A preview/seam is useful but not required for publication: the caller
  // creates a validated hard-seam fallback from the published mapping TIFFs.
  auto enblend = executable("HM_ENBLEND", "enblend");
  if (enblend.ok()) {
    status = run_checked(
        {*enblend, "--save-masks=seam_file.png", "-o", "panorama.tif", "mapping_0000.tif", "mapping_0001.tif"},
        staging);
    if (!status.ok()) {
      std::cerr << "Warning: native enblend preview failed; a hard seam will be generated: " << status << '\n';
      fs::remove(staging / "seam_file.png", error);
      error.clear();
      fs::remove(staging / "panorama.tif", error);
      error.clear();
    }
  }

  for (const char* artifact : kRequiredArtifacts) {
    status = validate_nonempty_file(staging / artifact);
    if (!status.ok())
      return status;
  }
  return publish_artifacts(staging, game_dir);
}

} // namespace hm::stitching
