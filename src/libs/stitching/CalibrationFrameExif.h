#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

struct CalibrationFrameSource {
  std::filesystem::path video;
  double seconds{0}; // Physical chapter PTS, before playlist rebasing.
};

namespace frame_exif {
using Tags = std::map<std::string, std::string>;
struct Sample {
  double seconds{0};
  double duration{0};
  Tags tags;
};
struct Metadata {
  Tags camera;
  std::vector<Sample> samples;
};
struct Insta360Clock {
  double origin{0}; // In ExifTool TimeCode units (raw timestamp / 1000).
  double scale{1}; // Converts TimeCode differences to seconds.
};
absl::StatusOr<std::optional<Insta360Clock>> ReadInsta360Clock(const std::filesystem::path& video);
absl::StatusOr<Metadata> Parse(const std::string& json, const std::optional<Insta360Clock>& clock);
Tags ForFrame(const Metadata& metadata, double seconds);
} // namespace frame_exif

// One instance per calibration operation; each chapter's metadata is read only once.
// This only edits PNG metadata; it never decodes or maps video/image pixels.
class CalibrationFrameExifWriter {
 public:
  explicit CalibrationFrameExifWriter(std::function<bool()> is_cancelled = {});
  absl::Status Write(const std::filesystem::path& png, const CalibrationFrameSource& source);

 private:
  std::function<bool()> is_cancelled_;
  std::map<std::filesystem::path, absl::StatusOr<frame_exif::Metadata>> cache_;
};
} // namespace hm::stitching
