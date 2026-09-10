#include "hstream/src/libs/stitching/CalibrationFrameExif.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>

#include "hstream/src/libs/common/Process.h"
#include "tools/cpp/runfiles/runfiles.h"
#include "yaml-cpp/yaml.h"

extern char** environ;

namespace hm::stitching {
namespace {
// Only request useful EXIF counterparts and timing. In particular, do not retain
// the millions of gyro/accelerometer samples in long action-camera recordings.
const char* const kTags[] = {
    "Make",
    "Model",
    "CameraSerialNumber",
    "SerialNumber",
    "LensSerialNumber",
    "LensModel",
    "LensMake",
    "FirmwareVersion",
    "Firmware",
    "Software",
    "FNumber",
    "FocalLength",
    "FocalLengthIn35mmFormat",
    "ExposureTime",
    "ExposureTimes",
    "ISO",
    "ISOSpeeds",
    "ExposureCompensation",
    "WhiteBalance",
    "MeteringMode",
    "Sharpness",
    "Contrast",
    "Saturation",
    "ExposureProgram",
    "ExposureMode",
    "DateTimeOriginal",
    "CreateDate",
    "GPSLatitude",
    "GPSLongitude",
    "GPSAltitude",
    "GPSSpeed",
    "GPSDateTime",
    "GPSMeasureMode",
    "GPSDOP",
    "GPSHPositioningError",
    "SampleTime",
    "SampleDuration",
    "TimeCode",
    "VideoFrameRate"};

std::optional<double> number(const std::string& value) {
  try {
    size_t end = 0;
    const double n = std::stod(value, &end);
    if (end == value.size() && std::isfinite(n))
      return n;
  } catch (const std::exception&) {
  }
  return std::nullopt;
}
std::optional<double> number(const frame_exif::Tags& tags, const std::string& key) {
  const auto found = tags.find(key);
  return found == tags.end() ? std::nullopt : number(found->second);
}
std::string decimal(double value) {
  std::ostringstream out;
  out << std::setprecision(15) << value;
  return out.str();
}

absl::StatusOr<std::string> exiftool(std::vector<std::string> arguments, const std::function<bool()>& cancelled) {
  const auto perl = hm::findExecutable("perl", {"PATH"});
  if (!perl)
    return absl::NotFoundError("PNG EXIF requires the Perl interpreter (Debian package perl)");
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  std::filesystem::path executable = self.parent_path().parent_path() / "share/exiftool/exiftool";
  if (!std::filesystem::is_regular_file(executable, ec)) {
    std::string error;
    const std::unique_ptr<bazel::tools::cpp::runfiles::Runfiles> runfiles(
        bazel::tools::cpp::runfiles::Runfiles::Create(self.string(), &error));
    executable = runfiles ? runfiles->Rlocation("exiftool/exiftool") : "";
  }
  if (executable.empty() || !std::filesystem::is_regular_file(executable, ec))
    return absl::NotFoundError("Pinned ExifTool runtime is missing from the installation or Bazel runfiles");
  arguments.insert(arguments.begin(), {*perl, executable.string()});
  // Do not load a user's Perl configuration while reading arbitrary camera files.
  arguments.insert(arguments.begin() + 2, {"-config", ""});
  std::map<std::string, std::string> environment;
  for (char** entry = environ; entry && *entry; ++entry) {
    const std::string value(*entry);
    const size_t separator = value.find('=');
    if (separator != std::string::npos)
      environment[value.substr(0, separator)] = value.substr(separator + 1);
  }
  environment["LC_ALL"] = "C";
  std::string output, errors;
  bool oversized = false;
  const auto start = std::chrono::steady_clock::now();
  const auto timed_out = [&] { return std::chrono::steady_clock::now() - start > std::chrono::seconds(90); };
  const int code = hm::run_command(
      arguments,
      "",
      environment,
      [&](const std::string& error, const std::string& line) {
        if (errors.size() < 4096 && !error.empty())
          errors += error + "\n";
        if (!line.empty() && !oversized) {
          if (output.size() + line.size() > 64 * 1024 * 1024)
            oversized = true;
          else
            output += line + "\n";
        }
      },
      [&] { return oversized || timed_out() || (cancelled && cancelled()); });
  if (cancelled && cancelled())
    return absl::CancelledError("PNG EXIF extraction cancelled");
  if (oversized || timed_out())
    return absl::ResourceExhaustedError("PNG EXIF extraction exceeded its time or metadata size limit");
  if (code != 0)
    return absl::InternalError("ExifTool failed: " + errors);
  return output;
}

uint64_t little_endian(const unsigned char* bytes, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i)
    value |= uint64_t(bytes[i]) << (8 * i);
  return value;
}

// Read just two scalar protobuf fields, skipping all others by wire type. These
// fields describe Insta360's clock, not image data. Layout documented by
// https://github.com/AdrianEddy/telemetry-parser/blob/master/src/insta360/extra_info.rs
std::optional<frame_exif::Insta360Clock> insta_clock(const std::vector<unsigned char>& data) {
  size_t position = 0;
  auto varint = [&](uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64 && position < data.size(); shift += 7) {
      const unsigned char byte = data[position++];
      if (shift == 63 && byte > 1)
        return false;
      value |= uint64_t(byte & 127) << shift;
      if (!(byte & 128))
        return true;
    }
    return false;
  };
  std::optional<uint64_t> first;
  bool raw = false;
  while (position < data.size()) {
    uint64_t key, value = 0;
    if (!varint(key) || key < 8)
      return std::nullopt;
    switch (key & 7) {
      case 0:
        if (!varint(value))
          return std::nullopt;
        if (key >> 3 == 24)
          first = value;
        if (key >> 3 == 62)
          raw = value != 0;
        continue;
      case 1:
        value = 8;
        break;
      case 2:
        if (!varint(value))
          return std::nullopt;
        break;
      case 5:
        value = 4;
        break;
      default:
        return std::nullopt;
    }
    if (value > data.size() - position)
      return std::nullopt;
    position += value;
  }
  if (!first || *first > uint64_t(std::numeric_limits<int64_t>::max()))
    return std::nullopt;
  return frame_exif::Insta360Clock{static_cast<double>(*first) / 1000.0, raw ? 0.001 : 1.0};
}
} // namespace

namespace frame_exif {
absl::StatusOr<std::optional<Insta360Clock>> ReadInsta360Clock(const std::filesystem::path& video) {
  std::ifstream input(video, std::ios::binary | std::ios::ate);
  if (!input)
    return absl::NotFoundError("Cannot open camera metadata source: " + video.string());
  const auto file_size = input.tellg();
  if (file_size < 78)
    return std::optional<Insta360Clock>{};
  // Most recordings end in the trailer; MP4 also permits boxes after 'inst'.
  std::streamoff trailer_end = file_size;
  unsigned char footer[78];
  auto read_footer = [&] {
    input.seekg(trailer_end - std::streamoff(sizeof(footer)));
    return bool(input.read(reinterpret_cast<char*>(footer), sizeof(footer))) &&
        std::string(reinterpret_cast<char*>(footer + 46), 32) == "8db42d694ccc418790edff439fe026bf";
  };
  if (!read_footer()) {
    input.clear();
    bool found = false;
    std::streamoff position = 0;
    for (size_t boxes = 0; position + 8 <= file_size && boxes < 4096; ++boxes) {
      unsigned char header[16];
      input.seekg(position);
      if (!input.read(reinterpret_cast<char*>(header), 8))
        break;
      auto big_endian = [](const unsigned char* data, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i)
          value = (value << 8) | data[i];
        return value;
      };
      uint64_t length = big_endian(header, 4);
      size_t header_size = 8;
      if (length == 1) {
        if (!input.read(reinterpret_cast<char*>(header + 8), 8))
          break;
        length = big_endian(header + 8, 8);
        header_size = 16;
      } else if (length == 0) {
        length = file_size - position;
      }
      if (length < header_size || length > static_cast<uint64_t>(file_size - position))
        break;
      if (std::string(reinterpret_cast<char*>(header + 4), 4) == "inst" && length >= header_size + 78) {
        trailer_end = position + static_cast<std::streamoff>(length);
        found = read_footer();
        break;
      }
      position += static_cast<std::streamoff>(length);
    }
    if (!found)
      return std::optional<Insta360Clock>{};
  }
  const uint64_t trailer_size = little_endian(footer + 38, 4);
  if (trailer_size < 78 || trailer_size > static_cast<uint64_t>(trailer_end))
    return absl::DataLossError("Invalid Insta360 trailer size");
  const auto start = trailer_end - std::streamoff(trailer_size);
  auto end = trailer_end - std::streamoff(72);
  // Records end with a 2-byte type and 4-byte length. Seek over gyro/preview data.
  for (size_t records = 0; end - start >= 6 && records < 4096; ++records) {
    unsigned char header[6];
    input.seekg(end - std::streamoff(6));
    if (!input.read(reinterpret_cast<char*>(header), 6))
      return absl::DataLossError("Truncated Insta360 record header");
    const uint64_t length = little_endian(header + 2, 4);
    if (length > static_cast<uint64_t>(end - start - 6))
      return absl::DataLossError("Invalid Insta360 record length");
    end -= std::streamoff(6 + length);
    if (little_endian(header, 2) == 0 && length % 10 == 0 && length <= 40960) {
      // Ace Pro directory entries may point across padding between records.
      std::vector<unsigned char> directory(length);
      input.seekg(end);
      if (!input.read(reinterpret_cast<char*>(directory.data()), length))
        return absl::DataLossError("Truncated Insta360 directory");
      for (size_t offset = 0; offset < directory.size(); offset += 10) {
        if (directory[offset] != 1)
          continue;
        const uint64_t size = little_endian(directory.data() + offset + 2, 4);
        const uint64_t location = little_endian(directory.data() + offset + 6, 4);
        if (size > trailer_size || location > trailer_size - size || trailer_size - location - size < 6)
          return absl::DataLossError("Invalid Insta360 directory entry");
        end = start + static_cast<std::streamoff>(location + size + 6);
        break;
      }
      continue;
    }
    if ((little_endian(header, 2) >> 8) != 1)
      continue;
    if (length > 4 * 1024 * 1024)
      return absl::ResourceExhaustedError("Insta360 clock metadata is too large");
    std::vector<unsigned char> data(length);
    input.seekg(end);
    if (!input.read(reinterpret_cast<char*>(data.data()), data.size()))
      return absl::DataLossError("Truncated Insta360 clock metadata");
    return insta_clock(data);
  }
  return std::optional<Insta360Clock>{};
}

absl::StatusOr<Metadata> Parse(const std::string& json, const std::optional<Insta360Clock>& clock) {
  Metadata result;
  std::map<std::string, Tags> documents;
  bool gopro = false, insta = false;
  try {
    const YAML::Node root = YAML::Load(json);
    if (!root.IsSequence() || root.size() != 1 || !root[0].IsMap())
      return absl::DataLossError("Expected one ExifTool metadata object");
    for (const auto& entry : root[0]) {
      const std::string key = entry.first.as<std::string>();
      if (!entry.second.IsScalar())
        continue;
      const size_t separator = key.rfind(':');
      if (separator == std::string::npos)
        continue;
      const std::string group = key.substr(0, separator);
      const std::string tag = key.substr(separator + 1);
      const std::string value = entry.second.as<std::string>();
      gopro |= group.find("GoPro") != std::string::npos;
      insta |= group.find("Insta360") != std::string::npos;
      if (group.find(":Doc") != std::string::npos) {
        documents[group][tag] = value;
      } else if (group.find("Composite") == std::string::npos) {
        // Prefer camera metadata to generic track values when both exist.
        if (!result.camera.count(tag) || group.find("GoPro") != std::string::npos ||
            group.find("Insta360") != std::string::npos)
          result.camera[tag] = value;
      }
    }
  } catch (const YAML::Exception& error) {
    return absl::DataLossError("Invalid ExifTool JSON: " + std::string(error.what()));
  }
  const auto make = result.camera.find("Make"), model = result.camera.find("Model");
  const std::string identity =
      (make != result.camera.end() ? make->second : "") + (model != result.camera.end() ? model->second : "");
  gopro |= identity.find("GoPro") != std::string::npos;
  insta |= identity.find("Insta360") != std::string::npos;
  if (!gopro && !insta)
    return Metadata{};
  result.camera["Make"] = gopro ? "GoPro" : "Insta360";
  const auto fps = number(result.camera, "VideoFrameRate");
  const double frame_duration = fps && *fps > 0 ? 1.0 / *fps : 0;
  // GPMF stores the first GPS fix in the parent document and further fixes as
  // DocN-1, DocN-2, etc. Divide the packet interval across the entire series.
  std::map<std::string, std::map<unsigned, Tags>> gps_series;
  for (const auto& [group, tags] : documents) {
    const size_t child = group.find('-', group.find(":Doc"));
    if (child == std::string::npos || !tags.count("GPSLatitude"))
      continue;
    const auto index = number(group.substr(child + 1));
    if (index && *index >= 1 && *index <= 100000 && std::floor(*index) == *index)
      gps_series[group.substr(0, child)][static_cast<unsigned>(*index)] = tags;
  }
  for (const auto& [group, tags] : documents) {
    auto time = number(tags, "SampleTime");
    double duration = number(tags, "SampleDuration").value_or(frame_duration);
    if (!time && clock && group.find("Insta360") != std::string::npos && tags.count("ExposureTime")) {
      if (const auto code = number(tags, "TimeCode")) {
        time = (*code - clock->origin) * clock->scale;
        duration = frame_duration;
      }
    }
    if (!time || !std::isfinite(*time) || !std::isfinite(duration) || duration <= 0)
      continue;
    Tags parent = tags;
    const auto children = gps_series.find(group);
    if (children != gps_series.end() && tags.count("GPSLatitude")) {
      auto fixes = children->second;
      fixes[0] = tags;
      const unsigned count = fixes.rbegin()->first + 1;
      const double interval = duration / count;
      for (const auto& [index, fix] : fixes) {
        Tags gps;
        for (const auto& [key, value] : fix) {
          if (key.rfind("GPS", 0) == 0)
            gps[key] = value;
        }
        for (const char* key : {"GPSMeasureMode", "GPSDOP", "GPSHPositioningError"})
          if (!gps.count(key) && tags.count(key))
            gps[key] = tags.at(key);
        result.samples.push_back({*time + index * interval, interval, std::move(gps)});
      }
      for (auto item = parent.begin(); item != parent.end();) {
        if (item->first.rfind("GPS", 0) == 0)
          item = parent.erase(item);
        else
          ++item;
      }
    }
    result.samples.push_back({*time, duration, std::move(parent)});
  }
  std::sort(
      result.samples.begin(), result.samples.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
  return result;
}

Tags ForFrame(const Metadata& metadata, double seconds) {
  if (!std::isfinite(seconds) || seconds < 0 || metadata.camera.empty())
    return {};
  Tags values = metadata.camera;
  for (const auto& sample : metadata.samples) {
    if (sample.seconds > seconds + 1e-6)
      break;
    if (seconds + 1e-6 >= sample.seconds + sample.duration)
      continue;
    for (const auto& [key, value] : sample.tags)
      values[key] = value;
    for (const auto& [plural, singular] : {std::pair{"ExposureTimes", "ExposureTime"}, {"ISOSpeeds", "ISO"}}) {
      const auto found = sample.tags.find(plural);
      if (found == sample.tags.end())
        continue;
      std::istringstream input(found->second);
      std::vector<std::string> entries;
      for (std::string item; input >> item;)
        entries.push_back(item);
      if (!entries.empty()) {
        const double fraction = std::max(0.0, (seconds - sample.seconds) / sample.duration);
        values[singular] = entries[std::min(entries.size() - 1, size_t(fraction * entries.size()))];
      }
    }
  }
  Tags out;
  for (const char* name : {"Make", "Model", "LensMake", "LensModel", "LensSerialNumber"}) {
    if (values.count(name))
      out[name] = values[name];
  }
  for (const char* name : {"SerialNumber", "CameraSerialNumber"})
    if (values.count(name))
      out["SerialNumber"] = values[name];
  for (const char* name : {"Software", "Firmware", "FirmwareVersion"})
    if (values.count(name))
      out["Software"] = values[name];
  for (const char* name :
       {"FNumber",
        "FocalLength",
        "FocalLengthIn35mmFormat",
        "ExposureTime",
        "ISO",
        "ExposureCompensation",
        "MeteringMode",
        "ExposureProgram",
        "ExposureMode",
        "GPSDOP",
        "GPSHPositioningError",
        "GPSMeasureMode"}) {
    if (const auto value = number(values, name)) {
      if (*value >= 0 && (std::string(name) != "ExposureTime" || *value > 0))
        out[name] = decimal(*value);
      else if (std::string(name) == "ExposureCompensation")
        out[name] = decimal(*value);
    }
  }
  // Vendor enums do not use EXIF's numeric codes. Map only unambiguous values.
  if (values.count("WhiteBalance")) {
    const auto& wb = values["WhiteBalance"];
    if (wb == "AUTO" || wb == "Auto" || wb == "0")
      out["WhiteBalance"] = "0";
    else if (wb == "MANUAL" || wb == "Manual" || (number(wb) && *number(wb) >= 2000))
      out["WhiteBalance"] = "1";
  }
  for (const char* name : {"Sharpness", "Contrast", "Saturation"}) {
    const auto found = values.find(name);
    if (found == values.end())
      continue;
    const std::string& value = found->second;
    if (value == "MED" || value == "Normal")
      out[name] = "0";
    if (value == "LOW" || value == "Soft" || value == "Low")
      out[name] = "1";
    if (value == "HIGH" || value == "Hard" || value == "High")
      out[name] = "2";
  }
  const auto latitude = number(values, "GPSLatitude"), longitude = number(values, "GPSLongitude");
  if (latitude && longitude && std::abs(*latitude) <= 90 && std::abs(*longitude) <= 180 &&
      number(values, "GPSMeasureMode").value_or(3) >= 2) {
    out["GPSLatitude"] = decimal(std::abs(*latitude));
    out["GPSLatitudeRef"] = *latitude < 0 ? "S" : "N";
    out["GPSLongitude"] = decimal(std::abs(*longitude));
    out["GPSLongitudeRef"] = *longitude < 0 ? "W" : "E";
    if (const auto altitude = number(values, "GPSAltitude")) {
      out["GPSAltitude"] = decimal(std::abs(*altitude));
      out["GPSAltitudeRef"] = *altitude < 0 ? "1" : "0";
    }
    // ExifTool reports speed in km/h for both formats.
    if (const auto speed = number(values, "GPSSpeed"); speed && *speed >= 0) {
      out["GPSSpeed"] = decimal(*speed);
      out["GPSSpeedRef"] = "K";
    }
    if (values.count("GPSDateTime") && values["GPSDateTime"].size() >= 19) {
      out["GPSDateStamp"] = values["GPSDateTime"].substr(0, 10);
      out["GPSTimeStamp"] = values["GPSDateTime"].substr(11);
    }
  }
  // These camera formats store chapter creation time as QuickTime UTC. Add the
  // original chapter PTS, not the pipeline's zero-based synchronized time.
  const auto created = values.find("CreateDate");
  if (created != values.end() && created->second.size() == 19 && created->second.substr(0, 4) != "0000") {
    std::tm date{};
    std::istringstream input(created->second);
    input >> std::get_time(&date, "%Y:%m:%d %H:%M:%S");
    if (!input.fail() && seconds < 366 * 86400.0) {
      const time_t epoch = timegm(&date) + static_cast<time_t>(std::floor(seconds));
      std::tm utc{};
      if (gmtime_r(&epoch, &utc)) {
        std::ostringstream timestamp;
        timestamp << std::put_time(&utc, "%Y:%m:%d %H:%M:%S");
        out["DateTimeOriginal"] = timestamp.str();
        out["OffsetTimeOriginal"] = "+00:00";
        std::ostringstream fraction;
        fraction << std::setw(6) << std::setfill('0') << static_cast<int>((seconds - std::floor(seconds)) * 1000000);
        out["SubSecTimeOriginal"] = fraction.str();
      }
    }
  }
  return out;
}
} // namespace frame_exif

CalibrationFrameExifWriter::CalibrationFrameExifWriter(std::function<bool()> is_cancelled)
    : is_cancelled_(std::move(is_cancelled)) {}

absl::Status CalibrationFrameExifWriter::Write(const std::filesystem::path& png, const CalibrationFrameSource& source) {
  if (source.video.empty())
    return absl::OkStatus(); // Live/test inputs have no physical recording.
  if (!std::isfinite(source.seconds) || source.seconds < 0)
    return absl::InvalidArgumentError("Invalid calibration frame source timestamp");
  auto cached = cache_.find(source.video);
  if (cached == cache_.end()) {
    std::vector<std::string> arguments{
        "-ee",
        "-a",
        "-j",
        "-G1:3",
        "-n",
        "-api",
        "IgnoreTags=all",
        "-api",
        "LargeFileSupport=1",
        "-api",
        "MaxDataLen=268435456"};
    std::string requested;
    for (const char* tag : kTags) {
      arguments.push_back("-" + std::string(tag));
      if (!requested.empty())
        requested += ',';
      requested += tag;
    }
    arguments.insert(
        arguments.end(), {"-api", "RequestTags=" + requested, "--", std::filesystem::absolute(source.video).string()});
    const auto json = exiftool(std::move(arguments), is_cancelled_);
    absl::StatusOr<frame_exif::Metadata> metadata = json.ok()
        ? frame_exif::Parse(*json, frame_exif::ReadInsta360Clock(source.video).value_or(std::nullopt))
        : absl::StatusOr<frame_exif::Metadata>(json.status());
    cached = cache_.emplace(source.video, std::move(metadata)).first;
  }
  if (!cached->second.ok())
    return cached->second.status();
  const auto tags = frame_exif::ForFrame(*cached->second, source.seconds);
  if (tags.empty())
    return absl::OkStatus();
  // ExifTool rewrites PNG chunks without re-encoding IDAT. Its temporary file is
  // renamed on success; -overwrite_original suppresses an unwanted backup PNG.
  std::vector<std::string> arguments{"-overwrite_original", "-n"};
  for (const auto& [name, value] : tags)
    arguments.push_back("-EXIF:" + name + "=" + value);
  arguments.insert(arguments.end(), {"--", std::filesystem::absolute(png).string()});
  const auto written = exiftool(std::move(arguments), is_cancelled_);
  return written.ok() ? absl::OkStatus() : written.status();
}
} // namespace hm::stitching
