#include "hstream/src/libs/stitching/CalibrationMatches.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include <opencv2/calib3d.hpp>

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace {
namespace fs = std::filesystem;
constexpr size_t kMaximumDocumentBytes = 32 * 1024 * 1024;
constexpr size_t kMaximumImageBytes = 512ULL * 1024 * 1024;
struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};
YAML::Node optional_child(const YAML::Node& parent, const char* key) {
  if (!parent || parent.IsNull())
    return YAML::Node(YAML::NodeType::Undefined);
  if (!parent.IsMap())
    throw std::runtime_error("Expected a calibration source configuration map");
  return parent[key];
}
bool fingerprint_valid(const std::string& value) {
  return value.size() == 64 &&
      std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string hex(const unsigned char* bytes, size_t length) {
  std::ostringstream out;
  for (size_t i = 0; i < length; ++i)
    out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
  return out.str();
}
absl::StatusOr<std::string> digest(const std::string& text) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (!EVP_Digest(text.data(), text.size(), bytes, &length, EVP_sha256(), nullptr) || length != 32)
    return absl::InternalError("Cannot hash calibration matches");
  return hex(bytes, length);
}
absl::Status require_directory(const fs::path& path, bool create) {
  std::error_code error;
  if (create)
    fs::create_directory(path, error);
  if (error || fs::symlink_status(path, error).type() != fs::file_type::directory || error)
    return absl::FailedPreconditionError("Calibration matches require a private directory: " + path.string());
  return absl::OkStatus();
}
std::string image_name(size_t index, size_t camera) {
  return std::string(camera ? "right_" : "left_") + std::to_string(index) + ".png";
}
absl::StatusOr<YAML::Node> image_identity(const fs::path& path) {
  Descriptor fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat before{}, after{};
  if (fd.value < 0 || ::fstat(fd.value, &before) || !S_ISREG(before.st_mode) || before.st_size < 33 ||
      uint64_t(before.st_size) > kMaximumImageBytes)
    return absl::FailedPreconditionError("Calibration input is not a bounded regular PNG: " + path.string());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!hash || !EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr))
    return absl::InternalError("Cannot hash calibration input");
  std::array<unsigned char, 65536> buffer{};
  size_t total = 0;
  YAML::Node result;
  for (;;) {
    const auto count = ::read(fd.value, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return absl::InternalError("Cannot read calibration input");
    if (!count)
      break;
    if (!total) {
      const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
      if (count < 33 || std::memcmp(buffer.data(), signature, 8) || std::memcmp(buffer.data() + 12, "IHDR", 4))
        return absl::InvalidArgumentError("Calibration input is not PNG");
      const auto number = [&](int at) {
        return uint32_t(buffer[at]) << 24 | uint32_t(buffer[at + 1]) << 16 | uint32_t(buffer[at + 2]) << 8 |
            uint32_t(buffer[at + 3]);
      };
      const auto w = number(16), h = number(20);
      if (!w || !h || w > 32768 || h > 32768 || uint64_t(w) * h > 128ULL * 1024 * 1024 ||
          (buffer[24] != 8 && buffer[24] != 16) || (buffer[25] != 2 && buffer[25] != 6))
        return absl::InvalidArgumentError("Calibration input PNG dimensions or format exceed limits");
      result["width"] = w;
      result["height"] = h;
    }
    total += count;
    if (total > kMaximumImageBytes || !EVP_DigestUpdate(hash.get(), buffer.data(), count))
      return absl::FailedPreconditionError("Calibration input changed during hashing");
  }
  if (::fstat(fd.value, &after) || total != uint64_t(before.st_size) || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    return absl::AbortedError("Calibration input changed during hashing");
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (!EVP_DigestFinal_ex(hash.get(), bytes, &length) || length != 32)
    return absl::InternalError("Cannot finish calibration input hash");
  result["bytes"] = total;
  result["sha256"] = hex(bytes, length);
  return result;
}
absl::Status copy_image(const fs::path& from, const fs::path& to) {
  Descriptor source{::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  Descriptor destination{::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  struct stat metadata{};
  if (source.value < 0 || destination.value < 0 || ::fstat(source.value, &metadata) || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || uint64_t(metadata.st_size) > kMaximumImageBytes)
    return absl::FailedPreconditionError("Cannot copy bounded calibration input");
  if (::ioctl(destination.value, FICLONE, source.value) != 0) {
    std::array<char, 65536> bytes;
    size_t total = 0;
    for (;;) {
      const auto count = ::read(source.value, bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        return absl::InternalError("Cannot copy calibration input");
      if (!count)
        break;
      total += count;
      if (total > kMaximumImageBytes)
        return absl::FailedPreconditionError("Calibration input grew during copy");
      size_t written = 0;
      while (written < size_t(count)) {
        const auto n = ::write(destination.value, bytes.data() + written, count - written);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          return absl::InternalError("Cannot save calibration input");
        written += n;
      }
    }
  }
  return ::fsync(destination.value) == 0 ? absl::OkStatus() : absl::InternalError("Cannot sync calibration input");
}
YAML::Node lens_yaml(const std::optional<FisheyeLensCalibration>& lens) {
  if (!lens)
    return YAML::Node(YAML::NodeType::Null);
  YAML::Node n;
  n["width"] = lens->resolution.width;
  n["height"] = lens->resolution.height;
  n["fx"] = lens->fx;
  n["fy"] = lens->fy;
  n["cx"] = lens->cx;
  n["cy"] = lens->cy;
  for (double value : lens->distortion)
    n["distortion"].push_back(value);
  return n;
}
std::optional<FisheyeLensCalibration> read_lens(const YAML::Node& n) {
  if (!n || n.IsNull())
    return {};
  FisheyeLensCalibration lens;
  lens.resolution = {n["width"].as<int>(), n["height"].as<int>()};
  lens.fx = n["fx"].as<double>();
  lens.fy = n["fy"].as<double>();
  lens.cx = n["cx"].as<double>();
  lens.cy = n["cy"].as<double>();
  if (!n["distortion"].IsSequence() || n["distortion"].size() != 4)
    throw std::runtime_error("Invalid lens distortion");
  for (size_t i = 0; i < 4; ++i)
    lens.distortion[i] = n["distortion"][i].as<double>();
  if (lens.resolution.width <= 0 || lens.resolution.height <= 0 || !std::isfinite(lens.fx) || lens.fx <= 0 ||
      !std::isfinite(lens.fy) || lens.fy <= 0 || !std::isfinite(lens.cx) || !std::isfinite(lens.cy) ||
      !std::all_of(lens.distortion.begin(), lens.distortion.end(), [](double x) { return std::isfinite(x); }))
    throw std::runtime_error("Invalid lens calibration");
  return lens;
}
absl::Status validate_calibration(const AkazeMatchingCalibration& calibration) {
  if (calibration.left.has_value() != calibration.right.has_value() ||
      (calibration.source_profile_fingerprint &&
       (!calibration.left || !fingerprint_valid(*calibration.source_profile_fingerprint))))
    return absl::InvalidArgumentError("Invalid paired lens calibration identity");
  try {
    (void)read_lens(lens_yaml(calibration.left));
    (void)read_lens(lens_yaml(calibration.right));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
  return absl::OkStatus();
}
bool same_calibration(const AkazeMatchingCalibration& a, const AkazeMatchingCalibration& b) {
  return a.source_profile_fingerprint == b.source_profile_fingerprint &&
      YAML::Dump(lens_yaml(a.left)) == YAML::Dump(lens_yaml(b.left)) &&
      YAML::Dump(lens_yaml(a.right)) == YAML::Dump(lens_yaml(b.right));
}
absl::Status validate(const CalibrationMatchSet& set) {
  HM_RETURN_IF_ERROR(validate_calibration(set.calibration));
  switch (set.matcher) {
    case ControlPointMatcher::kAkazeHamming:
      break;
    case ControlPointMatcher::kSuperPointLightGlue:
    case ControlPointMatcher::kDeDoDeLightGlue:
    case ControlPointMatcher::kLoFTR:
      if (set.calibration.left)
        return absl::InvalidArgumentError("Only calibrated AKAZE matches may contain lens calibration");
      break;
    default:
      return absl::InvalidArgumentError("Unknown calibration match matcher");
  }
  if (set.frames.empty() || set.frames.size() > 16 || !fingerprint_valid(set.source_context) ||
      (!set.fingerprint.empty() && !fingerprint_valid(set.fingerprint)) ||
      (!set.input_fingerprint.empty() && !fingerprint_valid(set.input_fingerprint)) ||
      (!set.selection_fingerprint.empty() && !fingerprint_valid(set.selection_fingerprint)) ||
      (!set.automatic_fingerprint.empty() && !fingerprint_valid(set.automatic_fingerprint)) ||
      (set.manual && set.automatic_fingerprint.empty()) || (!set.manual && !set.automatic_fingerprint.empty()))
    return absl::InvalidArgumentError("Invalid calibration match set identity or pair count");
  for (const auto& frame : set.frames) {
    if (frame.matches.size() > kMaximumEditableMatchesPerPair)
      return absl::InvalidArgumentError("Too many edited matches in one pair");
    for (size_t camera = 0; camera < 2; ++camera) {
      const auto size = frame.sizes[camera];
      if (size.width <= 0 || size.height <= 0 || size.width > 32768 || size.height > 32768 ||
          uint64_t(size.width) * size.height > 128ULL * 1024 * 1024 || !std::isfinite(frame.source_seconds[camera]) ||
          frame.source_seconds[camera] < 0 || frame.source_paths[camera].size() > 4096 ||
          frame.source_paths[camera].find('\0') != std::string::npos)
        return absl::InvalidArgumentError("Invalid calibration frame dimensions or time");
      for (const auto& match : frame.matches) {
        const auto point = camera ? match.right : match.left;
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 || point.x >= size.width ||
            point.y >= size.height || !std::isfinite(match.score))
          return absl::InvalidArgumentError("Match endpoint falls outside its original camera image");
      }
    }
  }
  return absl::OkStatus();
}
absl::StatusOr<std::string> read_document(const fs::path& path, const std::string& expected) {
  std::string text, actual;
  HM_ASSIGN_OR_RETURN(text, read_bounded_regular_file_no_follow(path, kMaximumDocumentBytes, "calibration matches"));
  HM_ASSIGN_OR_RETURN(actual, digest(text));
  if (actual != expected)
    return absl::FailedPreconditionError("Calibration match document fingerprint mismatch");
  return text;
}
absl::Status validate_original(
    const CalibrationMatchSet& set,
    const CalibrationMatchSet& original,
    const std::string& input_fingerprint) {
  if (original.manual || set.automatic_fingerprint != original.fingerprint ||
      input_fingerprint != original.input_fingerprint || set.matcher != original.matcher ||
      set.source_context != original.source_context || set.selection_fingerprint != original.selection_fingerprint ||
      !same_calibration(set.calibration, original.calibration))
    return absl::FailedPreconditionError("Edited matches do not reference their matching automatic original");
  return absl::OkStatus();
}

absl::StatusOr<CalibrationMatchSet> load_matches(
    const fs::path& game,
    const std::string& fingerprint,
    bool verify_images,
    bool require_automatic) {
  if (!fingerprint_valid(fingerprint))
    return absl::InvalidArgumentError("Invalid calibration match fingerprint");
  try {
    const auto root = game / "calibration-matches";
    HM_RETURN_IF_ERROR(require_directory(root, false));
    HM_RETURN_IF_ERROR(require_directory(root / "sets", false));
    std::string text;
    HM_ASSIGN_OR_RETURN(text, read_document(root / "sets" / (fingerprint + ".yaml"), fingerprint));
    const YAML::Node document = YAML::Load(text);
    CalibrationMatchSet set;
    set.fingerprint = fingerprint;
    if (document["version"].as<int>() != 1)
      return absl::InvalidArgumentError("Unsupported calibration match version");
    set.input_fingerprint = document["inputs"].as<std::string>();
    if (!fingerprint_valid(set.input_fingerprint))
      return absl::InvalidArgumentError("Invalid calibration input fingerprint");
    set.manual = document["manual"].as<bool>();
    if (require_automatic && set.manual)
      return absl::FailedPreconditionError("Automatic original cannot refer to edited matches");
    set.automatic_fingerprint = document["automatic"].as<std::string>();
    HM_ASSIGN_OR_RETURN(set.matcher, ParseControlPointMatcher(document["matcher"].as<std::string>()));
    set.calibration.left = read_lens(document["left_lens"]);
    set.calibration.right = read_lens(document["right_lens"]);
    const auto profile = document["lens_fingerprint"].as<std::string>();
    if (!profile.empty())
      set.calibration.source_profile_fingerprint = profile;
    const auto input_dir = root / "inputs" / set.input_fingerprint;
    HM_RETURN_IF_ERROR(require_directory(root / "inputs", false));
    HM_RETURN_IF_ERROR(require_directory(input_dir, false));
    HM_ASSIGN_OR_RETURN(text, read_document(input_dir / "frames.yaml", set.input_fingerprint));
    const YAML::Node inputs = YAML::Load(text);
    if (inputs["version"].as<int>() != 1)
      return absl::InvalidArgumentError("Unsupported calibration input version");
    set.selection_fingerprint = inputs["selection"].as<std::string>();
    set.source_context = inputs["source_context"].as<std::string>();
    const auto frames = inputs["frames"], points = document["frames"];
    if (!frames.IsSequence() || frames.size() == 0 || frames.size() > 16 || !points.IsSequence() ||
        points.size() != frames.size())
      return absl::InvalidArgumentError("Calibration matches have inconsistent frame counts");
    for (size_t i = 0; i < frames.size(); ++i) {
      CalibrationMatchFrame frame;
      for (size_t camera = 0; camera < 2; ++camera) {
        const YAML::Node node = frames[i][camera ? "right" : "left"];
        const auto path = input_dir / image_name(i, camera);
        const auto identity = node["image"];
        if (!fingerprint_valid(identity["sha256"].as<std::string>()) || identity["bytes"].as<uint64_t>() < 33 ||
            identity["bytes"].as<uint64_t>() > kMaximumImageBytes)
          return absl::InvalidArgumentError("Invalid calibration input image identity");
        if (verify_images) {
          YAML::Node actual;
          HM_ASSIGN_OR_RETURN(actual, image_identity(path));
          if (YAML::Dump(actual) != YAML::Dump(identity))
            return absl::FailedPreconditionError("Calibration match input image changed");
        }
        frame.images[camera] = path;
        frame.sizes[camera] = {identity["width"].as<int>(), identity["height"].as<int>()};
        frame.source_paths[camera] = node["source"].as<std::string>();
        frame.source_seconds[camera] = node["seconds"].as<double>();
      }
      const auto values = points[i];
      if (!values.IsSequence() || values.size() > kMaximumEditableMatchesPerPair)
        return absl::InvalidArgumentError("Invalid calibration match list");
      for (const auto& value : values) {
        if (!value.IsSequence() || value.size() != 5)
          return absl::InvalidArgumentError("Invalid match coordinates");
        frame.matches.push_back(
            {{value[0].as<float>(), value[1].as<float>()},
             {value[2].as<float>(), value[3].as<float>()},
             value[4].as<float>()});
      }
      set.frames.push_back(std::move(frame));
    }
    HM_RETURN_IF_ERROR(validate(set));
    if (set.manual) {
      CalibrationMatchSet original;
      HM_ASSIGN_OR_RETURN(original, load_matches(game, set.automatic_fingerprint, false, true));
      HM_RETURN_IF_ERROR(validate_original(set, original, set.input_fingerprint));
    }
    return set;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot load calibration matches: " + std::string(error.what()));
  }
}

} // namespace

absl::StatusOr<CalibrationMatchSet> LoadCalibrationMatches(
    const fs::path& game,
    const std::string& fingerprint,
    bool verify_images) {
  return load_matches(game, fingerprint, verify_images, false);
}

absl::StatusOr<std::string> PublishCalibrationMatches(const fs::path& game, const CalibrationMatchSet& set) {
  HM_RETURN_IF_ERROR(validate(set));
  try {
    const auto root = game / "calibration-matches";
    HM_RETURN_IF_ERROR(require_directory(root, true));
    HM_RETURN_IF_ERROR(require_directory(root / "inputs", true));
    HM_RETURN_IF_ERROR(require_directory(root / "sets", true));
    Descriptor lock{::open((root / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
    struct stat metadata{};
    if (lock.value < 0 || ::fstat(lock.value, &metadata) || !S_ISREG(metadata.st_mode))
      return absl::FailedPreconditionError("Cannot lock calibration match store");
    while (::flock(lock.value, LOCK_EX))
      if (errno != EINTR)
        return absl::InternalError("Cannot lock calibration matches");
    YAML::Node inputs;
    inputs["version"] = 1;
    inputs["selection"] = set.selection_fingerprint;
    inputs["source_context"] = set.source_context;
    for (size_t i = 0; i < set.frames.size(); ++i) {
      for (size_t camera = 0; camera < 2; ++camera) {
        auto node = inputs["frames"][i][camera ? "right" : "left"];
        YAML::Node identity;
        HM_ASSIGN_OR_RETURN(identity, image_identity(set.frames[i].images[camera]));
        if (identity["width"].as<int>() != set.frames[i].sizes[camera].width ||
            identity["height"].as<int>() != set.frames[i].sizes[camera].height)
          return absl::FailedPreconditionError("Calibration match image dimensions changed");
        node["image"] = identity;
        node["source"] = set.frames[i].source_paths[camera];
        node["seconds"] = set.frames[i].source_seconds[camera];
      }
    }
    const std::string input_text = YAML::Dump(inputs) + "\n";
    if (input_text.size() > kMaximumDocumentBytes)
      return absl::ResourceExhaustedError("Calibration input manifest exceeds the storage limit");
    std::string input_fingerprint;
    HM_ASSIGN_OR_RETURN(input_fingerprint, digest(input_text));
    if (!set.input_fingerprint.empty() && set.input_fingerprint != input_fingerprint)
      return absl::AbortedError("Calibration match source images changed after inspection");
    if (set.manual) {
      CalibrationMatchSet original;
      HM_ASSIGN_OR_RETURN(original, load_matches(game, set.automatic_fingerprint, false, true));
      HM_RETURN_IF_ERROR(validate_original(set, original, input_fingerprint));
    }
    const auto input_dir = root / "inputs" / input_fingerprint;
    if (!fs::exists(input_dir)) {
      std::string pattern = (root / "inputs" / ".staging-XXXXXX").string();
      std::vector<char> writable(pattern.begin(), pattern.end());
      writable.push_back('\0');
      const char* created = ::mkdtemp(writable.data());
      if (!created)
        return absl::InternalError("Cannot stage calibration inputs");
      struct Cleanup {
        fs::path path;
        ~Cleanup() {
          std::error_code ignored;
          fs::remove_all(path, ignored);
        }
      } staging{created};
      for (size_t i = 0; i < set.frames.size(); ++i)
        for (size_t camera = 0; camera < 2; ++camera) {
          const auto path = staging.path / image_name(i, camera);
          HM_RETURN_IF_ERROR(copy_image(set.frames[i].images[camera], path));
          YAML::Node copied;
          HM_ASSIGN_OR_RETURN(copied, image_identity(path));
          if (YAML::Dump(copied) != YAML::Dump(inputs["frames"][i][camera ? "right" : "left"]["image"]))
            return absl::AbortedError("Calibration image changed while saving");
        }
      HM_RETURN_IF_ERROR(write_stitch_transaction_file(staging.path / "frames.yaml", input_text));
      HM_RETURN_IF_ERROR(fsync_stitch_path(staging.path, true));
      fs::rename(staging.path, input_dir);
      HM_RETURN_IF_ERROR(fsync_stitch_path(root / "inputs", true));
    } else {
      HM_RETURN_IF_ERROR(require_directory(input_dir, false));
      std::string existing;
      HM_ASSIGN_OR_RETURN(existing, read_document(input_dir / "frames.yaml", input_fingerprint));
      for (size_t i = 0; i < set.frames.size(); ++i)
        for (size_t camera = 0; camera < 2; ++camera) {
          YAML::Node actual;
          HM_ASSIGN_OR_RETURN(actual, image_identity(input_dir / image_name(i, camera)));
          if (YAML::Dump(actual) != YAML::Dump(inputs["frames"][i][camera ? "right" : "left"]["image"]))
            return absl::FailedPreconditionError("Stored calibration match image changed");
        }
    }
    YAML::Node document;
    document["version"] = 1;
    document["inputs"] = input_fingerprint;
    document["manual"] = set.manual;
    document["automatic"] = set.automatic_fingerprint;
    document["matcher"] = ControlPointMatcherName(set.matcher);
    document["left_lens"] = lens_yaml(set.calibration.left);
    document["right_lens"] = lens_yaml(set.calibration.right);
    document["lens_fingerprint"] = set.calibration.source_profile_fingerprint.value_or("");
    for (const auto& frame : set.frames) {
      YAML::Node points(YAML::NodeType::Sequence);
      for (const auto& match : frame.matches) {
        YAML::Node point(YAML::NodeType::Sequence);
        for (float value : {match.left.x, match.left.y, match.right.x, match.right.y, match.score})
          point.push_back(value);
        point.SetStyle(YAML::EmitterStyle::Flow);
        points.push_back(point);
      }
      document["frames"].push_back(points);
    }
    const std::string contents = YAML::Dump(document) + "\n";
    if (contents.size() > kMaximumDocumentBytes)
      return absl::ResourceExhaustedError("Edited matches exceed the storage limit");
    std::string fingerprint;
    HM_ASSIGN_OR_RETURN(fingerprint, digest(contents));
    const auto path = root / "sets" / (fingerprint + ".yaml");
    if (fs::exists(path)) {
      std::string existing;
      HM_ASSIGN_OR_RETURN(existing, read_document(path, fingerprint));
    } else {
      // The store lock serializes publishers. Use a unique, exclusively created
      // temporary file so stale or substituted pending paths cannot be followed.
      HM_RETURN_IF_ERROR(publish_stitch_file_atomically(path, contents));
    }
    HM_RETURN_IF_ERROR(fsync_stitch_path(root, true));
    HM_RETURN_IF_ERROR(fsync_stitch_path(game, true));
    return fingerprint;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot save calibration matches: " + std::string(error.what()));
  }
}

absl::Status CopyCalibrationMatches(
    const fs::path& source,
    const fs::path& destination,
    const std::string& fingerprint) {
  if (fingerprint.empty())
    return absl::OkStatus();
  CalibrationMatchSet set;
  HM_ASSIGN_OR_RETURN(set, LoadCalibrationMatches(source, fingerprint));
  if (set.manual) {
    CalibrationMatchSet automatic;
    HM_ASSIGN_OR_RETURN(automatic, LoadCalibrationMatches(source, set.automatic_fingerprint));
    if (automatic.manual || automatic.input_fingerprint != set.input_fingerprint)
      return absl::FailedPreconditionError("Edited match set has no matching automatic original");
    std::string copied;
    HM_ASSIGN_OR_RETURN(copied, PublishCalibrationMatches(destination, automatic));
    if (copied != set.automatic_fingerprint)
      return absl::AbortedError("Automatic matches changed while copying");
  }
  std::string copied;
  HM_ASSIGN_OR_RETURN(copied, PublishCalibrationMatches(destination, set));
  return copied == fingerprint ? absl::OkStatus() : absl::AbortedError("Calibration matches changed while copying");
}

absl::StatusOr<std::string> CalibrationMatchSourceContext(const YAML::Node& config, const fs::path& game) {
  try {
    YAML::Node context;
    const auto stitching = optional_child(config, "stitching");
    const auto game_config = optional_child(config, "game");
    const auto videos = optional_child(game_config, "videos");
    const auto anchor = optional_child(stitching, "stitch_frame_time");
    context["anchor_ns"] =
        hm::stitch_frame_time_to_nanoseconds(anchor && !anchor.IsNull() ? anchor.as<std::string>() : "00:00:00");
    const auto offsets = optional_child(optional_child(game_config, "stitching"), "frame_offsets");
    for (const char* role : {"left", "right"}) {
      const auto sources = optional_child(videos, role);
      if (!sources || !sources.IsSequence() || sources.size() == 0)
        return absl::FailedPreconditionError("Match editing requires resolved camera sources");
      for (const auto& entry : sources) {
        const auto source = entry.as<std::string>();
        if (source.empty() || source.size() > 4096 || source.find('\0') != std::string::npos)
          return absl::InvalidArgumentError("Invalid match camera source path");
        fs::path path(source);
        if (path.is_relative())
          path = game / path;
        context[role]["videos"].push_back(fs::weakly_canonical(path).generic_string());
      }
      const auto configured_offset = optional_child(offsets, role);
      const double offset = configured_offset && !configured_offset.IsNull() ? configured_offset.as<double>() : 0.0;
      if (!std::isfinite(offset))
        return absl::InvalidArgumentError("Invalid match source synchronization");
      context[role]["offset"] = offset;
    }
    return digest(YAML::Dump(context));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot identify calibration match sources: " + std::string(error.what()));
  }
}
absl::Status ValidateCalibrationMatchInputs(
    const CalibrationMatchSet& set,
    const YAML::Node& config,
    const fs::path& game,
    size_t frame_count) {
  HM_RETURN_IF_ERROR(validate(set));
  std::string context;
  HM_ASSIGN_OR_RETURN(context, CalibrationMatchSourceContext(config, game));
  if (context != set.source_context || frame_count != set.frames.size())
    return absl::FailedPreconditionError(
        "Edited matches belong to different camera sources, synchronization, reference time or frame count; create a new automatic candidate");
  try {
    const auto configured_matcher = optional_child(optional_child(config, "stitching"), "control_point_matcher");
    auto matcher = ParseControlPointMatcher(
        configured_matcher && !configured_matcher.IsNull() ? configured_matcher.as<std::string>()
                                                           : "superpoint-lightglue");
    if (!matcher.ok())
      return matcher.status();
    if (*matcher != set.matcher)
      return absl::FailedPreconditionError("Edited matches belong to a different feature matcher");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid feature matcher configuration: " + std::string(error.what()));
  }
  return absl::OkStatus();
}

absl::Status ValidateCalibrationMatchCalibration(
    const CalibrationMatchSet& set,
    const AkazeMatchingCalibration& current) {
  HM_RETURN_IF_ERROR(validate(set));
  HM_RETURN_IF_ERROR(validate_calibration(current));
  if (!same_calibration(set.calibration, current))
    return absl::FailedPreconditionError("Edited matches belong to a different lens calibration");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<FeatureMatch>> ConvertCalibrationMatchCoordinates(
    const std::vector<FeatureMatch>& matches,
    const std::array<cv::Size, 2>& sizes,
    const AkazeMatchingCalibration& calibration,
    bool to_camera_pixels) {
  HM_RETURN_IF_ERROR(validate_calibration(calibration));
  if (matches.size() > kMaximumEditableMatchesPerPair)
    return absl::InvalidArgumentError("Too many calibration matches for coordinate conversion");
  auto result = matches;
  try {
    for (size_t camera = 0; camera < 2; ++camera) {
      if (sizes[camera].width <= 0 || sizes[camera].height <= 0 || sizes[camera].width > 32768 ||
          sizes[camera].height > 32768 || uint64_t(sizes[camera].width) * sizes[camera].height > 128ULL * 1024 * 1024)
        return absl::InvalidArgumentError("Invalid calibration coordinate image size");
      const auto in_bounds = [&](cv::Point2f point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 0 && point.y >= 0 &&
            point.x < sizes[camera].width && point.y < sizes[camera].height;
      };
      std::vector<cv::Point2d> input;
      input.reserve(matches.size());
      for (const auto& match : matches) {
        const auto point = camera ? match.right : match.left;
        if (!in_bounds(point) || !std::isfinite(match.score))
          return absl::InvalidArgumentError("Invalid calibration match endpoint before lens conversion");
        input.emplace_back(point);
      }
      const auto& lens = camera ? calibration.right : calibration.left;
      if (!lens || input.empty())
        continue;
      const double sx = double(sizes[camera].width) / lens->resolution.width;
      const double sy = double(sizes[camera].height) / lens->resolution.height;
      const cv::Matx33d k(lens->fx * sx, 0, lens->cx * sx, 0, lens->fy * sy, lens->cy * sy, 0, 0, 1);
      const cv::Vec4d d(lens->distortion[0], lens->distortion[1], lens->distortion[2], lens->distortion[3]);
      std::vector<cv::Point2d> output;
      // One OpenCV call per camera, rather than allocating temporary vectors for
      // every endpoint when the editor or solver converts a full frame's matches.
      if (to_camera_pixels) {
        for (auto& point : input)
          point = {(point.x - k(0, 2)) / k(0, 0), (point.y - k(1, 2)) / k(1, 1)};
        cv::fisheye::distortPoints(input, output, k, d);
      } else {
        cv::fisheye::undistortPoints(input, output, k, d, cv::Matx33d::eye(), k);
      }
      for (size_t index = 0; index < output.size(); ++index) {
        auto& point = camera ? result[index].right : result[index].left;
        point = output[index];
        if (!in_bounds(point))
          return absl::InvalidArgumentError("Match endpoint is outside the calibrated image after lens conversion");
      }
    }
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
  return result;
}
} // namespace hm::stitching
