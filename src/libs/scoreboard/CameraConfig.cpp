#include "hstream/src/libs/scoreboard/CameraConfig.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"

#include <cmath>
#include <regex>

namespace hm::scoreboard {
absl::StatusOr<int64_t> ParseCameraStartOffset(const std::string& value) {
  if (value.size() > 40)
    return absl::InvalidArgumentError("Scoreboard start offset is too long");
  static const std::regex pattern(R"(^([+-]?)([0-9]{2,}):([0-5][0-9]):([0-5][0-9])\.([0-9]{4})$)");
  std::smatch parts;
  if (!std::regex_match(value, parts, pattern))
    return absl::InvalidArgumentError("Scoreboard start offset requires signed HH:MM:SS.ssss");
  try {
    const uint64_t hours = std::stoull(parts[2].str());
    const uint64_t remainder = (std::stoull(parts[3].str()) * 60 + std::stoull(parts[4].str())) * GST_SECOND +
        std::stoull(parts[5].str()) * 100000;
    if (hours > (INT64_MAX - remainder) / (3600 * GST_SECOND))
      return absl::OutOfRangeError("Scoreboard start offset exceeds the timestamp range");
    const int64_t time = hours * (3600 * GST_SECOND) + remainder;
    return parts[1].str() == "-" ? -time : time;
  } catch (const std::exception&) {
    return absl::OutOfRangeError("Scoreboard start offset exceeds the timestamp range");
  }
}

absl::StatusOr<std::optional<CameraSessionConfig>> ResolveCameraConfig(
    const YAML::Node& camera,
    const std::filesystem::path& game_directory,
    CameraDecodeBackend backend,
    unsigned device,
    const std::filesystem::path& render_node) {
  try {
    if (!camera.IsMap() || !camera["enabled"].IsScalar())
      return absl::InvalidArgumentError("Effective baseline is missing rink.scoreboard.camera.enabled");
    if (!camera["enabled"].as<bool>())
      return std::nullopt;
    const auto files = camera["files"];
    const auto uri = camera["uri"];
    if (!files.IsSequence() || !uri.IsDefined() || (!uri.IsNull() && !uri.IsScalar()))
      return absl::InvalidArgumentError("Scoreboard camera requires files or a live URI");
    const bool live = !uri.IsNull();
    if (live == (files.size() > 0))
      return absl::InvalidArgumentError("Select exactly one recorded or live scoreboard source");
    const auto dimensions = camera["source_dimensions"];
    const auto polygon = camera["polygon"];
    if (!dimensions.IsSequence() || dimensions.size() != 2 || !polygon.IsSequence() || polygon.size() != 4)
      return absl::InvalidArgumentError("Scoreboard camera requires source_dimensions and its four-point polygon");
    const int width = dimensions[0].as<int>(), height = dimensions[1].as<int>();
    std::array<std::array<float, 2>, 4> points;
    for (size_t i = 0; i < points.size(); ++i) {
      if (!polygon[i].IsSequence() || polygon[i].size() != 2)
        return absl::InvalidArgumentError("Scoreboard camera polygon requires four x,y points");
      for (size_t j = 0; j < 2; ++j)
        points[i][j] = polygon[i][j].as<float>();
    }
    auto region = MakeCameraRegion(points, width, height, 2);
    if (!region.ok())
      return region.status();
    const auto timeout = camera["operation_timeout_ms"].as<int64_t>();
    const auto poll = camera["cancellation_poll_ms"].as<int64_t>();
    const auto queue = camera["queue_frames"].as<int64_t>();
    const auto extra = camera["extra_decode_surfaces"].as<int64_t>();
    if (timeout <= 0 || timeout > INT64_MAX / GST_MSECOND || poll <= 0 || poll > timeout || queue <= 0 ||
        queue > UINT_MAX - 3 || extra < 0 || extra > UINT_MAX)
      return absl::InvalidArgumentError("Invalid scoreboard camera queue bounds or deadlines");
    const auto offset = ParseCameraStartOffset(camera["start_offset"].as<std::string>());
    if (!offset.ok())
      return offset.status();
    CameraSessionConfig result{
        {backend,
         CameraCodec::kH264,
         live,
         device,
         render_node,
         width,
         height,
         *region,
         static_cast<unsigned>(queue),
         static_cast<unsigned>(extra),
         static_cast<GstClockTime>(timeout * GST_MSECOND),
         static_cast<GstClockTime>(poll * GST_MSECOND)},
        {},
        -*offset,
        0,
        0,
        {},
        nullptr,
        GST_CLOCK_TIME_NONE,
        camera["live_latency_ms"].as<unsigned>(),
        camera["live_ntp_sync"].as<bool>()};
    if (live) {
      result.live_uri = uri.as<std::string>();
      const auto codec = camera["codec"].as<std::string>();
      if (codec != "h264" && codec != "hevc")
        return absl::InvalidArgumentError("Live scoreboard codec must be h264 or hevc");
      result.decoder.codec = codec == "hevc" ? CameraCodec::kHevc : CameraCodec::kH264;
      if (!gst_uri_is_valid(result.live_uri.c_str()) ||
          (result.live_uri.rfind("rtsp://", 0) != 0 && result.live_uri.rfind("rtsps://", 0) != 0))
        return absl::InvalidArgumentError("Live scoreboard requires an RTSP or RTSPS URI");
    } else {
      int64_t start = 0;
      for (const auto& file : files) {
        auto path = std::filesystem::path(file.as<std::string>());
        if (path.empty())
          return absl::InvalidArgumentError("Scoreboard camera path is empty");
        if (path.is_relative())
          path = game_directory / path;
        std::error_code error;
        path = std::filesystem::canonical(path, error);
        if (error)
          return absl::NotFoundError("Cannot resolve a scoreboard recording");
        auto track = media::ReadMp4VideoTrack(path, result.decoder.operation_timeout, {});
        if (!track.ok())
          return track.status();
        if (track->width != width || track->height != height)
          return absl::FailedPreconditionError("Scoreboard recording dimensions changed; select its region again");
        const auto codec = track->codec == media::VideoCodec::kHevc ? CameraCodec::kHevc : CameraCodec::kH264;
        if (!result.chapters.empty() && result.decoder.codec != codec)
          return absl::InvalidArgumentError("Scoreboard chapters must use the same video codec");
        result.decoder.codec = codec;
        if (track->duration_ns <= 0 || start > INT64_MAX - track->duration_ns)
          return absl::OutOfRangeError("Scoreboard recording timeline overflow");
        result.chapters.push_back({path, start, track->duration_ns, track->start_ns});
        start += track->duration_ns;
      }
    }
    return std::optional<CameraSessionConfig>(std::move(result));
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid scoreboard camera configuration: ") + error.what());
  }
}
} // namespace hm::scoreboard
