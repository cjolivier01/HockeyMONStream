#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include "absl/strings/numbers.h"
#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace {
constexpr double kPi = 3.14159265358979323846;
bool finite(cv::Vec3d ray) {
  return std::isfinite(ray[0]) && std::isfinite(ray[1]) && std::isfinite(ray[2]) && cv::norm(ray) > 0.5;
}
cv::Vec3d sphereRay(double x, double y) {
  const double longitude = (x - 1799.5) * kPi / 1800, latitude = (899.5 - y) * kPi / 1800;
  return {std::cos(latitude) * std::cos(longitude), -std::cos(latitude) * std::sin(longitude), std::sin(latitude)};
}
struct PrivateDirectory {
  std::filesystem::path path;
  PrivateDirectory() {
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error)
      return;
    std::string name = (root / "hstream-rink-XXXXXX").string();
    if (mkdtemp(name.data()))
      path = name;
  }
  ~PrivateDirectory() {
    std::error_code error;
    if (!path.empty())
      std::filesystem::remove_all(path, error);
  }
};
absl::StatusOr<std::vector<cv::Vec3d>> transformRays(
    const std::string& pto,
    const std::string& input,
    size_t count,
    bool reverse,
    bool allow_outside,
    const std::function<bool()>& cancelled) {
  if (count > 65536 || input.size() > 4 * 1024 * 1024 || pto.size() > 4 * 1024 * 1024)
    return absl::ResourceExhaustedError("Rink projection exceeds its startup budget");
  auto executable = HuginProject::ResolveExecutable("HM_PANO_TRAFO", "pano_trafo");
  if (!executable.ok())
    return executable.status();
  PrivateDirectory temp;
  if (temp.path.empty())
    return absl::InternalError("Could not create a private rink projection directory");
  const auto project_path = temp.path / "project.pto", input_path = temp.path / "points.txt";
  {
    std::ofstream project(project_path), points(input_path);
    project << pto;
    points << input;
    project.close();
    points.close();
    if (!project || !points)
      return absl::InternalError("Could not write rink projection inputs");
  }
  const int input_fd = open(input_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input_fd < 0)
    return absl::InternalError("Could not open rink projection input");
  std::vector<std::string> arguments{*executable};
  if (reverse)
    arguments.push_back("-r");
  arguments.push_back(project_path.string());
  const auto start = std::chrono::steady_clock::now();
  std::string output, errors;
  bool oversized = false;
  auto stopped = [&] {
    return oversized || (cancelled && cancelled()) ||
        std::chrono::steady_clock::now() - start > std::chrono::seconds(20);
  };
  const int code = hm::run_command(
      arguments,
      temp.path.string(),
      {{"LC_ALL", "C"}},
      [&](const std::string& error, const std::string& line) {
        if (output.size() + line.size() > 4 * 1024 * 1024 || errors.size() + error.size() > 65536) {
          oversized = true;
          return;
        }
        if (!line.empty())
          output += line + '\n';
        errors += error;
      },
      stopped,
      input_fd);
  close(input_fd);
  if (stopped())
    return absl::CancelledError("Rink projection stopped or exceeded its startup limits");
  if (code != 0)
    return absl::InternalError("Could not prepare rink projection: " + errors);
  std::istringstream stream(output);
  stream.imbue(std::locale::classic());
  std::vector<cv::Vec3d> rays;
  rays.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    std::string sx, sy;
    double x = 0, y = 0;
    if (!(stream >> sx >> sy) || !absl::SimpleAtod(sx, &x) || !absl::SimpleAtod(sy, &y))
      return absl::DataLossError("Invalid rink projection coordinates");
    if (!std::isfinite(x) || !std::isfinite(y) || x < -0.50001 || x > 3599.50001 || y < -0.50001 || y > 1799.50001) {
      if (!allow_outside)
        return absl::DataLossError("A rink observation lies outside the calibrated camera");
      rays.push_back({0, 0, 0});
    } else {
      rays.push_back(sphereRay(x, y));
    }
  }
  stream >> std::ws;
  if (!stream.eof())
    return absl::DataLossError("Rink projection returned extra coordinates");
  return rays;
}
} // namespace

absl::StatusOr<std::string> RinkMapCalibrationRevision(const std::filesystem::path& directory) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
    return absl::InternalError("Could not initialize rink calibration digest");
  for (const char* name : {"autooptimiser_out.pto", "stitching_canvas_provenance", "left.png", "right.png"}) {
    auto bytes = read_bounded_regular_file_no_follow(directory / name, 128 * 1024 * 1024, "rink-map calibration");
    if (!bytes.ok())
      return bytes.status();
    const std::string size = std::to_string(bytes->size());
    for (std::string_view part : {std::string_view(name), std::string_view(size), std::string_view(*bytes)})
      if (EVP_DigestUpdate(hash.get(), part.data(), part.size()) != 1)
        return absl::InternalError("Could not hash rink calibration");
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (EVP_DigestFinal_ex(hash.get(), digest, &length) != 1)
    return absl::InternalError("Could not finalize rink calibration digest");
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (unsigned i = 0; i < length; ++i)
    result << std::setw(2) << unsigned(digest[i]);
  return result.str();
}

absl::StatusOr<std::vector<cv::Vec3d>> RinkProjectionRays(
    const std::string& pto,
    const MappingCanvasPlacement& canvas,
    size_t columns,
    size_t rows,
    const std::function<bool()>& cancelled) {
  if (columns < 2 || columns > 512 || rows < 2 || rows > 256 || columns * rows > 65536)
    return absl::InvalidArgumentError("Invalid rink projection grid dimensions");
  std::istringstream lines(pto);
  std::string line, panorama;
  while (std::getline(lines, line))
    if (line.rfind("p ", 0) == 0) {
      if (!panorama.empty())
        return absl::InvalidArgumentError("Multiple panorama definitions");
      panorama = line;
    }
  // Split only actual PTO fields: quoted output names may contain w/h/S text.
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false, escaped = false;
  for (char ch : panorama) {
    if (!quoted && (ch == ' ' || ch == '\t' || ch == '\r')) {
      if (!field.empty()) {
        fields.push_back(std::move(field));
        field.clear();
      }
      continue;
    }
    field += ch;
    if (ch == '"' && !escaped)
      quoted = !quoted;
    escaped = ch == '\\' && !escaped;
  }
  if (quoted)
    return absl::InvalidArgumentError("Unterminated panorama field");
  if (!field.empty())
    fields.push_back(std::move(field));
  size_t width = 0, height = 0;
  std::string crop;
  for (const auto& token : fields) {
    if (token[0] == 'w') {
      if (width || !absl::SimpleAtoi(token.substr(1), &width))
        return absl::InvalidArgumentError("Invalid panorama width");
    } else if (token[0] == 'h') {
      if (height || !absl::SimpleAtoi(token.substr(1), &height))
        return absl::InvalidArgumentError("Invalid panorama height");
    } else if (token[0] == 'S') {
      if (!crop.empty())
        return absl::InvalidArgumentError("Duplicate panorama crop");
      crop = token;
    }
  }
  size_t left = 0, right = width, top = 0, bottom = height;
  if (!crop.empty()) {
    std::replace(crop.begin(), crop.end(), ',', ' ');
    std::istringstream values(crop.substr(1));
    values.imbue(std::locale::classic());
    std::string l, r, t, b, extra;
    if (!(values >> l >> r >> t >> b) || (values >> extra) || !absl::SimpleAtoi(l, &left) ||
        !absl::SimpleAtoi(r, &right) || !absl::SimpleAtoi(t, &top) || !absl::SimpleAtoi(b, &bottom))
      return absl::InvalidArgumentError("Invalid panorama crop");
  }
  if (!width || !height || width > 1000000 || height > 1000000 || right > width || bottom > height || left >= right ||
      top >= bottom || !canvas.width || !canvas.height || !std::isfinite(canvas.left) || !std::isfinite(canvas.top) ||
      canvas.left < double(left) - 1 || canvas.top < double(top) - 1 ||
      canvas.left + canvas.width > double(right) + 1 || canvas.top + canvas.height > double(bottom) + 1)
    return absl::FailedPreconditionError("Panorama crop does not match its saved canvas");
  // Synthetic sphere camera uses the same PT axes as PrepareRinkLevelingProject.
  // TIFF placements may cover only part of the PTO crop. Use their actual
  // normalized origin/span, including NONA's possible one-pixel rounding.
  const std::string project = panorama + "\nm i0\ni w3600 h1800 f4 v360 y0 p0 r0 n\"sphere.png\"\n";
  std::ostringstream input;
  input.imbue(std::locale::classic());
  input << std::setprecision(17);
  for (size_t y = 0; y < rows; ++y)
    for (size_t x = 0; x < columns; ++x)
      input << "0 " << canvas.left - 0.5 + double(x) * canvas.width / (columns - 1) << ' '
            << canvas.top - 0.5 + double(y) * canvas.height / (rows - 1) << '\n';
  return transformRays(project, input.str(), columns * rows, true, true, cancelled);
}

absl::StatusOr<std::shared_ptr<const PlaybackRinkMap>> LoadPlaybackRinkMap(
    const std::filesystem::path& directory,
    const std::function<bool()>& cancelled) {
  auto result = std::make_shared<PlaybackRinkMap>();
  RinkMapDocument document;
  std::string pto;
  MappingCanvasPlacement canvas;
  {
    auto lock = try_lock_canvas_constraint_artifacts(directory);
    if (!lock.ok())
      return lock.status();
    if (!*lock)
      return absl::UnavailableError("Calibration is changing; restart playback after saving the rink map");
    auto config_lock = GameConfigTransactionLock::TryAcquire(directory.string());
    if (!config_lock.ok())
      return config_lock.status();
    auto saved = read_bounded_regular_file_no_follow(directory / "rink-map.yaml", 512 * 1024, "saved rink map");
    if (!saved.ok())
      return absl::FailedPreconditionError("Save a reviewed rink map before using rink-aware framing or the mini-map");
    auto parsed = ParseRinkMap(*saved);
    if (!parsed.ok())
      return parsed.status();
    document = *parsed;
    result->source_document = *saved;
    auto generation = stitch_artifact_preflight_generation_id_locked(directory);
    if (!generation.ok())
      return generation.status();
    result->hugin_generation = *generation;
    auto digest = assets::AssetManager::Sha256Bytes(*generation);
    auto revision = RinkMapCalibrationRevision(directory);
    if (!digest.ok() || !revision.ok() || *digest != document.generation || *revision != document.calibration_revision)
      return absl::FailedPreconditionError("The rink map belongs to an older calibration; review and save it again");
    auto config = read_bounded_regular_file_no_follow(directory / "config.yaml", 4 * 1024 * 1024, "rink configuration");
    if (!config.ok())
      return config.status();
    try {
      const auto calibration = YAML::Load(*config)["hstream_ui"]["stitching_calibration"];
      if (!calibration.IsMap() || calibration["status"].as<std::string>("") != "complete" ||
          !calibration["stale_from"].as<std::string>("").empty())
        return absl::FailedPreconditionError("Complete calibration before using the rink map");
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError(error.what());
    }
    auto provenance = HuginProject::ReadCanvasProvenanceLocked(directory);
    if (!provenance.ok() || !provenance->has_value() || (**provenance).mapping_backend != MappingBackend::kNona)
      return absl::FailedPreconditionError("The rink map requires a NONA calibration");
    auto bounds = validate_stitch_generation_artifact_bounds_locked(directory);
    if (!bounds.ok())
      return bounds;
    auto placement = read_mapping_canvas_placement_locked(directory);
    if (!placement.ok())
      return placement.status();
    canvas = *placement;
    if (canvas.width != (**provenance).canvas_width || canvas.height != (**provenance).canvas_height)
      return absl::FailedPreconditionError("Mapping TIFFs do not match their published canvas");
    auto project =
        read_bounded_regular_file_no_follow(directory / "autooptimiser_out.pto", 4 * 1024 * 1024, "rink project");
    if (!project.ok())
      return project.status();
    pto = *project;
  }
  auto sphere = PrepareRinkLevelingProject(pto);
  if (!sphere.ok())
    return sphere.status();
  std::ostringstream input;
  input.imbue(std::locale::classic());
  input << std::setprecision(17);
  size_t count = 0;
  for (const auto& observation : document.observations) {
    if (observation.camera < 0 || size_t(observation.camera) >= sphere->image_sizes.size())
      return absl::InvalidArgumentError("Invalid rink observation camera");
    const auto size = sphere->image_sizes[observation.camera];
    for (const auto p : observation.pixels) {
      if (p.x < 0 || p.y < 0 || p.x >= size[0] || p.y >= size[1])
        return absl::InvalidArgumentError("Rink observation is outside the camera image");
      input << observation.camera << ' ' << p.x << ' ' << p.y << '\n';
      ++count;
    }
  }
  if (!count)
    return absl::FailedPreconditionError("The saved rink map is empty; add anchors before playback");
  auto observation_rays = transformRays(sphere->pto, input.str(), count, false, false, cancelled);
  if (!observation_rays.ok())
    return observation_rays.status();
  size_t offset = 0;
  for (auto& observation : document.observations) {
    observation.rays.assign(
        observation_rays->begin() + offset, observation_rays->begin() + offset + observation.pixels.size());
    offset += observation.pixels.size();
  }
  auto fit = FitRinkMap(document);
  if (!fit.ok())
    return fit.status();
  if (fit->state != RinkMapState::kReady)
    return absl::FailedPreconditionError(fit->message);
  result->layout = document.layout;
  result->fit = *fit;
  auto rays = RinkProjectionRays(pto, canvas, result->columns, result->rows, cancelled);
  if (!rays.ok())
    return rays.status();
  result->rays = std::move(*rays);
  return std::shared_ptr<const PlaybackRinkMap>(std::move(result));
}

std::optional<double> PlaybackRinkMap::OutputRotation(const std::string& generation, unsigned width, unsigned height)
    const {
  if (!width || !height || hugin_generation.empty() ||
      !validate_stitched_output_generation_hugin(generation, hugin_generation).ok())
    return std::nullopt;
  constexpr std::string_view prefix = "post-stitch-rotate-degrees:";
  const auto marker = generation.rfind(prefix);
  const auto end = generation.find('\n', marker);
  double angle = 0;
  if (marker == std::string::npos ||
      !absl::SimpleAtod(generation.substr(marker + prefix.size(), end - marker - prefix.size()), &angle) ||
      !std::isfinite(angle))
    return std::nullopt;
  if (generation.substr(end + 1) != "output-size:" + std::to_string(width) + "x" + std::to_string(height) + "\n")
    return std::nullopt;
  return angle;
}

std::optional<cv::Vec3d> PlaybackRinkMap::ViewingRay(
    double x,
    double y,
    double rotation,
    unsigned width,
    unsigned height) const {
  if (columns < 2 || rows < 2 || rays.size() != columns * rows || !width || !height || !std::isfinite(x) ||
      !std::isfinite(y) || !std::isfinite(rotation))
    return std::nullopt;
  if (x < 0 || x > 1 || y < 0 || y > 1)
    return std::nullopt;
  const double angle = -rotation * kPi / 180, c = std::cos(angle), s = std::sin(angle);
  const double dx = (x - 0.5) * width, dy = (y - 0.5) * height;
  x = 0.5 + (c * dx + s * dy) / width;
  y = 0.5 + (-s * dx + c * dy) / height;
  if (x < 0 || x > 1 || y < 0 || y > 1)
    return std::nullopt;
  const double gx = x * (columns - 1), gy = y * (rows - 1);
  const size_t ix = std::min(size_t(gx), columns - 2), iy = std::min(size_t(gy), rows - 2);
  const double tx = gx - ix, ty = gy - iy;
  const auto a = rays[iy * columns + ix], b = rays[iy * columns + ix + 1];
  const auto c0 = rays[(iy + 1) * columns + ix], d = rays[(iy + 1) * columns + ix + 1];
  if (!finite(a) || !finite(b) || !finite(c0) || !finite(d))
    return std::nullopt;
  auto ray = a * ((1 - tx) * (1 - ty)) + b * (tx * (1 - ty)) + c0 * ((1 - tx) * ty) + d * (tx * ty);
  return ray;
}

std::optional<cv::Point2d> PlaybackRinkMap::Locate(double x, double y, double rotation, unsigned width, unsigned height)
    const {
  if (fit.state != RinkMapState::kReady)
    return std::nullopt;
  const auto ray = ViewingRay(x, y, rotation, width, height);
  if (!ray)
    return std::nullopt;
  auto point = RinkRayToPoint(fit, *ray);
  if (!point.ok() || std::abs(point->x) > layout.length / 2 || std::abs(point->y) > layout.width / 2)
    return std::nullopt;
  const double corner_x = std::max(0.0, std::abs(point->x) - (layout.length / 2 - layout.corner_radius));
  const double corner_y = std::max(0.0, std::abs(point->y) - (layout.width / 2 - layout.corner_radius));
  if (std::hypot(corner_x, corner_y) > layout.corner_radius + 1e-6)
    return std::nullopt;
  return *point;
}

std::optional<cv::Point2d> PlaybackRinkMap::Project(cv::Point2d point, double rotation, unsigned width, unsigned height)
    const {
  if (columns < 2 || rows < 2 || columns > 512 || rows > 256 || rays.size() != columns * rows || !width || !height ||
      !std::isfinite(rotation))
    return std::nullopt;
  const auto target = RinkPointToRay(fit, point);
  if (!target.ok())
    return std::nullopt;
  const cv::Vec3d direction = *target / cv::norm(*target);
  double best = -1;
  cv::Point2d pixel;
  for (size_t i = 0; i < rays.size(); ++i) {
    if (!finite(rays[i]))
      continue;
    const double similarity = direction.dot(rays[i]) / cv::norm(rays[i]);
    if (similarity > best) {
      best = similarity;
      pixel = {double(i % columns) / (columns - 1), double(i / columns) / (rows - 1)};
    }
  }
  if (best < 0)
    return std::nullopt;
  for (int iteration = 0; iteration < 10; ++iteration) {
    const auto sample = ViewingRay(pixel.x, pixel.y, 0, width, height);
    if (!sample)
      return std::nullopt;
    const auto ray = *sample / cv::norm(*sample);
    const auto residual = direction - ray;
    if (cv::norm(residual) < 1e-7)
      break;
    constexpr double epsilon = 1e-5;
    const double dx = pixel.x < 0.5 ? epsilon : -epsilon, dy = pixel.y < 0.5 ? epsilon : -epsilon;
    const auto x = ViewingRay(pixel.x + dx, pixel.y, 0, width, height);
    const auto y = ViewingRay(pixel.x, pixel.y + dy, 0, width, height);
    if (!x || !y)
      return std::nullopt;
    const auto a = (*x / cv::norm(*x) - ray) / dx, b = (*y / cv::norm(*y) - ray) / dy;
    const double aa = a.dot(a), ab = a.dot(b), bb = b.dot(b), determinant = aa * bb - ab * ab;
    if (determinant < 1e-12)
      return std::nullopt;
    pixel.x += std::clamp((bb * a.dot(residual) - ab * b.dot(residual)) / determinant, -0.05, 0.05);
    pixel.y += std::clamp((aa * b.dot(residual) - ab * a.dot(residual)) / determinant, -0.05, 0.05);
    if (pixel.x < 0 || pixel.x > 1 || pixel.y < 0 || pixel.y > 1)
      return std::nullopt;
  }
  const auto check = ViewingRay(pixel.x, pixel.y, 0, width, height);
  if (!check || cv::norm(*check / cv::norm(*check) - direction) > 1e-5)
    return std::nullopt;
  const double angle = rotation * kPi / 180, c = std::cos(angle), s = std::sin(angle);
  const double dx = (pixel.x - 0.5) * width, dy = (pixel.y - 0.5) * height;
  pixel = {0.5 + (c * dx + s * dy) / width, 0.5 + (-s * dx + c * dy) / height};
  if (pixel.x < 0 || pixel.x > 1 || pixel.y < 0 || pixel.y > 1)
    return std::nullopt;
  return pixel;
}
} // namespace hm::stitching
