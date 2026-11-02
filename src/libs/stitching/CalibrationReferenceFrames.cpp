#include "hstream/src/libs/stitching/CalibrationReferenceFrames.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

namespace hm::stitching {
absl::StatusOr<std::vector<std::array<CalibrationFrameSource, 2>>> PlanCalibrationReferenceFrames(
    const std::array<CameraRecordingWindow, 2>& cameras,
    int64_t reference_ns,
    size_t count) {
  const double duration = std::min(cameras[0].duration_seconds, cameras[1].duration_seconds);
  if (!std::isfinite(cameras[0].fps) || !std::isfinite(cameras[1].fps) || cameras[0].fps <= 0 || cameras[1].fps <= 0)
    return absl::InvalidArgumentError("Calibration requires known camera frame rates");
  const double interval = 1.0 / std::min(cameras[0].fps, cameras[1].fps);
  const double reference = reference_ns / 1e9;
  const double first = reference < 0 ? duration + reference : reference;
  if (!std::isfinite(duration) || duration < interval || count == 0 || !std::isfinite(first) || first < 0 ||
      first >= duration)
    return absl::OutOfRangeError("First calibration frame is outside the synchronized recording");
  std::vector<std::array<CalibrationFrameSource, 2>> result(count);
  for (size_t index = 0; index < count; ++index) {
    const double time = std::min(std::fmod(first + duration * (double(index) / count), duration), duration - interval);
    for (size_t camera = 0; camera < 2; ++camera) {
      double remaining = time;
      for (const auto& chapter : cameras[camera].chapters) {
        if (remaining < chapter.duration_seconds) {
          const double last_frame = chapter.start_seconds + chapter.duration_seconds - 1.0 / cameras[camera].fps;
          if (!std::isfinite(last_frame) || last_frame < chapter.start_seconds)
            return absl::OutOfRangeError("Calibration sample has no complete video frame after the camera start");
          result[index][camera] = {
              chapter.file, chapter.timestamp_origin_seconds + std::min(chapter.start_seconds + remaining, last_frame)};
          break;
        }
        remaining -= chapter.duration_seconds;
      }
      if (result[index][camera].video.empty())
        return absl::OutOfRangeError("Calibration sample has no source chapter");
    }
  }
  return result;
}

namespace {
CalibrationImageWriter Writer(
    std::string ffmpeg,
    std::shared_ptr<std::array<CalibrationFrameSource, 2>> sources,
    size_t camera,
    std::function<bool()> is_cancelled,
    bool exact = false) {
  return [ffmpeg = std::move(ffmpeg), sources = std::move(sources), camera, is_cancelled, exact](
             const std::filesystem::path& output) {
    if (is_cancelled && is_cancelled())
      return absl::CancelledError("Calibration frame extraction cancelled");
    const auto began = std::chrono::steady_clock::now();
    const auto requested = sources->at(camera);
    std::ostringstream time;
    // FFmpeg seeks at microsecond precision. Start one microsecond before an
    // exact selected PTS to avoid rounding past that frame, then verify it.
    time << std::fixed << std::setprecision(9) << (exact ? requested.seconds - 0.000001 : requested.seconds);
    std::optional<double> actual;
    int64_t time_base_n = 0, time_base_d = 0;
    std::string errors;
    // Input -ss uses the preceding keyframe, then accurate decode/discard. Only
    // one still is encoded. CPU image access is confined to offline calibration,
    // whose feature matcher and PNG/EXIF store already consume CPU images.
    const int status = hm::run_command(
        {ffmpeg,      "-seek_timestamp", "1",        "-nostdin",  "-v",      "info",         "-hwaccel",
         "auto",      "-noautorotate",   "-ss",      time.str(),  "-copyts", "-i",           requested.video.string(),
         "-map",      "0:v:0",           "-an",      "-frames:v", "1",       "-vf",          "showinfo",
         "-fps_mode", "passthrough",     "-pix_fmt", "rgb48be",   "-y",      output.string()},
        "",
        {{"LC_ALL", "C"}},
        [&](const std::string& error, const std::string&) {
          if (error.find("Parsed_showinfo") != std::string::npos) {
            const auto base = error.find("config in time_base:");
            if (base != std::string::npos) {
              char slash = 0;
              std::istringstream values(error.substr(base + 20));
              values >> time_base_n >> slash >> time_base_d;
              if (slash != '/')
                time_base_d = 0;
            }
            const auto position = error.find(" pts:");
            if (!actual && position != std::string::npos && time_base_n > 0 && time_base_d > 0) {
              try {
                actual = double(std::stoll(error.substr(position + 5))) * time_base_n / time_base_d;
              } catch (const std::exception&) {
              }
            }
          }
          if (errors.size() < 4096)
            errors += error.substr(0, 4096 - errors.size()) + "\n";
        },
        is_cancelled);
    if (is_cancelled && is_cancelled())
      return absl::CancelledError("Calibration frame extraction cancelled");
    if (status != 0 || !actual || !std::isfinite(*actual) || !std::filesystem::is_regular_file(output))
      return absl::FailedPreconditionError(
          "Could not seek/decode calibration frame at " + time.str() + " in " + requested.video.string() + ": " +
          errors);
    if (exact && std::abs((*actual - requested.seconds) * 1e9) > 2.0)
      return absl::FailedPreconditionError("Selected player frame decoded to a different source timestamp");
    sources->at(camera).seconds = *actual;
    std::cout << "Calibration still: keyframe seek/decode " << requested.video.filename()
              << " requested=" << std::setprecision(12) << requested.seconds << " decoded=" << *actual << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count()
              << std::endl;
    return absl::OkStatus();
  };
}
} // namespace

namespace {
absl::StatusOr<std::array<CameraRecordingWindow, 2>> RecordingWindows(
    const std::filesystem::path& game,
    const YAML::Node& config) {
  try {
    std::array<CameraRecordingWindow, 2> cameras;
    for (size_t index = 0; index < 2; ++index) {
      const auto role = index == 0 ? "left" : "right";
      const auto videos = config["game"]["videos"][role];
      if (!videos || !videos.IsSequence() || videos.size() == 0)
        return absl::FailedPreconditionError("End-relative calibration requires resolved camera playlists");
      std::vector<std::string> files;
      for (const auto& video : videos) {
        const std::filesystem::path path(video.as<std::string>());
        files.push_back((path.is_absolute() ? path : game / path).string());
      }
      CameraRecordingWindow full;
      HM_ASSIGN_OR_RETURN(full, CameraRecordingWindowAt(files, 0, std::numeric_limits<double>::max()));
      const double frames = config["game"]["stitching"]["frame_offsets"][role].as<double>();
      HM_ASSIGN_OR_RETURN(
          cameras[index], CameraRecordingWindowAt(files, frames / full.fps, std::numeric_limits<double>::max()));
    }
    return cameras;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot read calibration recording timeline: " + std::string(error.what()));
  }
}
} // namespace

absl::StatusOr<CalibrationRecordingTimeline> ReadCalibrationRecordingTimeline(
    const std::filesystem::path& game,
    const YAML::Node& config) {
  std::array<CameraRecordingWindow, 2> cameras;
  HM_ASSIGN_OR_RETURN(cameras, RecordingWindows(game, config));
  const double duration = std::min(cameras[0].duration_seconds, cameras[1].duration_seconds);
  const double interval = std::max(1.0 / cameras[0].fps, 1.0 / cameras[1].fps);
  if (!std::isfinite(duration) || !std::isfinite(interval) || duration <= 0 || interval <= 0 ||
      duration * 1e9 > static_cast<double>(std::numeric_limits<int64_t>::max()))
    return absl::InvalidArgumentError("Invalid synchronized recording duration");
  return CalibrationRecordingTimeline{
      static_cast<uint64_t>(std::llround(duration * 1e9)), static_cast<uint64_t>(std::llround(interval * 1e9))};
}

absl::StatusOr<std::vector<CalibrationFrameInput>> EndRelativeCalibrationInputs(
    const std::filesystem::path& game,
    const YAML::Node& config,
    size_t count,
    const std::function<bool()>& is_cancelled) {
  try {
    const auto ffmpeg = hm::findExecutable("ffmpeg", {"PATH"});
    if (!ffmpeg)
      return absl::NotFoundError("End-relative calibration requires ffmpeg");
    std::vector<std::array<CalibrationFrameSource, 2>> planned;
    const auto selection = config["stitching"]["calibration_frame_selection"];
    const bool selected = selection && !selection.IsNull();
    if (selected) {
      PlayerFrameSelectionPlan plan;
      HM_ASSIGN_OR_RETURN(plan, ParsePlayerFrameSelectionPlan(selection));
      HM_RETURN_IF_ERROR(validate_player_frame_selection_sources(config));
      if (plan.selected.size() != count || plan.context.find("scan_clock") == plan.context.end() ||
          plan.context.at("scan_clock") != "wrapped-elapsed-v1")
        return absl::FailedPreconditionError("Lazy selected-frame capture requires the exact wrapped scan plan");
      for (const auto& observation : plan.selected) {
        const auto& pair = observation.pair.cameras;
        planned.push_back(
            {CalibrationFrameSource{pair[0].path, double(pair[0].source_pts_ns) / 1e9},
             CalibrationFrameSource{pair[1].path, double(pair[1].source_pts_ns) / 1e9}});
      }
    } else {
      // A frozen scan already owns exact physical pairs. Only ordinary
      // sampling needs to resolve a reference and walk recording windows;
      // compact game configs may omit a reference equal to the baseline.
      const auto reference =
          hm::stitch_frame_time_to_nanoseconds(config["stitching"]["stitch_frame_time"].as<std::string>());
      std::array<CameraRecordingWindow, 2> cameras;
      HM_ASSIGN_OR_RETURN(cameras, RecordingWindows(game, config));
      HM_ASSIGN_OR_RETURN(planned, PlanCalibrationReferenceFrames(cameras, reference, count));
    }
    std::vector<CalibrationFrameInput> inputs;
    for (const auto& pair : planned) {
      auto decoded = std::make_shared<std::array<CalibrationFrameSource, 2>>(pair);
      CalibrationFrameInput input{
          Writer(*ffmpeg, decoded, 0, is_cancelled, selected),
          Writer(*ffmpeg, decoded, 1, is_cancelled, selected),
          pair[0],
          pair[1]};
      input.decoded_sources = decoded;
      inputs.push_back(std::move(input));
    }
    return inputs;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot plan end-relative calibration: " + std::string(error.what()));
  }
}
} // namespace hm::stitching
