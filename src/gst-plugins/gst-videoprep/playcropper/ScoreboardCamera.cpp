#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "hstream/src/libs/program/ProgramGpu.h"

#include <cmath>
#include <sstream>

namespace hm::playcropper {
absl::Status PlayCropperPriv::ConfigureScoreboardCamera(const std::string& encoded) {
  gsize size = 0;
  auto padded = encoded;
  while (padded.size() % 4)
    padded.push_back('=');
  auto* bytes = g_base64_decode(padded.c_str(), &size);
  auto* canonical = g_base64_encode(bytes, size);
  const bool valid = canonical && padded == canonical;
  g_free(canonical);
  const std::string text(reinterpret_cast<const char*>(bytes), size);
  g_free(bytes);
  if (!valid)
    return absl::InvalidArgumentError("Invalid encoded scoreboard camera configuration");
  try {
    const auto value = YAML::Load(text);
    if (value["resize_filter"].as<std::string>() != "bilinear" || value["sharpen_amount"].as<double>() != 0)
      return absl::UnimplementedError("Independent scoreboard requires bilinear sampling without sharpening");
    auto config = scoreboard::ResolveCameraConfig(
        value["camera"],
        value["game_directory"].as<std::string>(),
        scoreboard::CameraDecodeBackend::kNvidia,
        m_gpuId,
        {});
    if (!config.ok())
      return config.status();
    CancelScoreboardCamera();
    {
      std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
      scoreboard_camera_session_.reset();
    }
    scoreboard_camera_config_ = std::move(*config);
    scoreboard_camera_enabled_.store(scoreboard_camera_config_.has_value(), std::memory_order_release);
    scoreboard_camera_stopping_.store(false, std::memory_order_release);
    scoreboard_camera_plan_.reset();
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid scoreboard camera configuration: ") + error.what());
  }
}
absl::Status PlayCropperPriv::SetScoreboardCameraTimeline(const std::string& value) {
  std::istringstream input(value);
  int64_t origin;
  int numerator, denominator;
  char first, second;
  if (!(input >> origin >> first >> numerator >> second >> denominator) || first != ',' || second != ',' ||
      !(input >> std::ws).eof() || origin < 0 || numerator <= 0 || denominator <= 0)
    return absl::InvalidArgumentError("Scoreboard timeline requires an origin frame and rational rate");
  CancelScoreboardCamera();
  {
    std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
    scoreboard_camera_session_.reset();
  }
  scoreboard_camera_stopping_.store(false, std::memory_order_release);
  scoreboard_camera_origin_frame_ = origin;
  scoreboard_camera_fps_n_ = numerator;
  scoreboard_camera_fps_d_ = denominator;
  return absl::OkStatus();
}
void PlayCropperPriv::CancelScoreboardCamera() {
  std::shared_ptr<scoreboard::CameraSession> session;
  {
    std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
    session = scoreboard_camera_session_;
  }
  if (session)
    session->Cancel();
}
bool PlayCropperPriv::HandleEvent(GstEvent* event) {
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
    scoreboard_camera_stopping_.store(true, std::memory_order_release);
    CancelScoreboardCamera();
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_CUSTOM_DOWNSTREAM_OOB) {
    const auto* structure = gst_event_get_structure(event);
    if (structure && gst_structure_has_name(structure, "hstream-scoreboard-timeline")) {
      const auto* value = gst_structure_get_string(structure, "value");
      return value && SetProperty({"scoreboard-camera-timeline", value});
    }
  }
  return Super::HandleEvent(event);
}
absl::Status PlayCropperPriv::StartScoreboardCamera(int64_t game_start) {
  auto config = *scoreboard_camera_config_;
  config.game_start_ns = game_start;
  config.epoch = ++scoreboard_camera_epoch_;
  GstClock* clock = nullptr;
  if (config.decoder.live) {
    // Start on the shared clock before the game graph chooses its base time.
    // CameraSession converts running PTS back to absolute clock coordinates.
    clock = gst_system_clock_obtain();
    config.live_clock = clock;
    config.live_base_time = gst_clock_get_time(clock);
  }
  auto started = scoreboard::CameraSession::Start(config);
  if (clock)
    gst_object_unref(clock);
  if (!started.ok())
    return started.status();
  std::shared_ptr<scoreboard::CameraSession> session(std::move(*started));
  {
    std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
    scoreboard_camera_session_ = session;
  }
  if (scoreboard_camera_stopping_.load(std::memory_order_acquire))
    session->Cancel();
  return absl::OkStatus();
}
absl::Status PlayCropperPriv::RenderScoreboardCamera(
    surface::Surface output,
    const NvDsFrameMeta* frame,
    cudaStream_t stream) {
  if (!frame || !GST_CLOCK_TIME_IS_VALID(frame->buf_pts))
    return absl::DataLossError("Scoreboard overlay requires the output presentation timestamp");
  uint64_t game_time;
  if (scoreboard_camera_config_->decoder.live) {
    auto* actual = gst_element_get_clock(scoreboard_camera_element_);
    auto* expected = gst_system_clock_obtain();
    const bool same_clock = actual == expected;
    if (actual)
      gst_object_unref(actual);
    gst_object_unref(expected);
    const auto base = gst_element_get_base_time(scoreboard_camera_element_);
    if (!same_clock || !GST_CLOCK_TIME_IS_VALID(base) || base > INT64_MAX || frame->buf_pts > INT64_MAX - base)
      return absl::FailedPreconditionError("Live scoreboard and game must share the system clock");
    game_time = base + frame->buf_pts;
  } else {
    if (scoreboard_camera_fps_n_ <= 0 || scoreboard_camera_fps_d_ <= 0)
      return absl::FailedPreconditionError("Recorded scoreboard has no game-frame timeline");
    const auto unit = uint64_t(scoreboard_camera_fps_d_) * GST_SECOND;
    const auto relative = gst_util_uint64_scale_round(frame->buf_pts, scoreboard_camera_fps_n_, unit);
    if (relative > UINT64_MAX - scoreboard_camera_origin_frame_)
      return absl::OutOfRangeError("Scoreboard game-frame clock overflow");
    game_time = gst_util_uint64_scale(relative + scoreboard_camera_origin_frame_, unit, scoreboard_camera_fps_n_);
  }
  if (game_time > INT64_MAX)
    return absl::OutOfRangeError("Scoreboard game time exceeds signed clock range");
  // A negative UI start offset deliberately leaves the output unobscured
  // until source time zero. This is the only permitted missing-frame interval.
  const auto overlay_time = scoreboard_camera_config_->decoder.live ? frame->buf_pts : game_time;
  if (scoreboard_camera_config_->offset_ns > 0 && overlay_time < uint64_t(scoreboard_camera_config_->offset_ns))
    return absl::OkStatus();
  std::shared_ptr<scoreboard::CameraSession> session;
  {
    std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
    session = scoreboard_camera_session_;
  }
  if (!session) {
    HM_RETURN_IF_ERROR(StartScoreboardCamera(game_time));
    std::lock_guard<std::mutex> lock(scoreboard_camera_mutex_);
    session = scoreboard_camera_session_;
  }
  const auto position = scoreboard_position_.load(std::memory_order_relaxed);
  if (!scoreboard_camera_plan_ || scoreboard_camera_plan_->output_width != int(output.width()) ||
      scoreboard_camera_plan_->output_height != int(output.height()) || scoreboard_camera_plan_position_ != position) {
    const auto dimension = [](const std::string& value, int extent, double scale) -> absl::StatusOr<int> {
      if (value.empty())
        return absl::InvalidArgumentError("Scoreboard dimensions must be resolved from the baseline");
      const bool percent = value.front() == '%';
      char* end = nullptr;
      const char* start = value.c_str() + (percent ? 1 : 0);
      const double parsed = std::strtod(start, &end);
      const double pixels = scale * parsed * (percent ? extent / 100.0 : 1.0);
      if (end == start || *end || !std::isfinite(pixels) || pixels < 2 || pixels > extent)
        return absl::InvalidArgumentError("Scoreboard dimensions must fit the output");
      return static_cast<int>(pixels);
    };
    int width;
    HM_ASSIGN_OR_RETURN(width, dimension(scoreboard_projected_width_, output.width(), scoreboard_scale_));
    int height;
    HM_ASSIGN_OR_RETURN(height, dimension(scoreboard_projected_height_, output.height(), scoreboard_scale_));
    const auto& region = scoreboard_camera_config_->decoder.region;
    scoreboard::Plan plan;
    HM_ASSIGN_OR_RETURN(
        plan,
        scoreboard::MakePlan(
            region.polygon,
            region.width,
            region.height,
            output.width(),
            output.height(),
            width,
            height,
            position,
            scoreboard_flip_));
    scoreboard_camera_plan_ = plan;
    scoreboard_camera_plan_position_ = position;
  }
  std::shared_ptr<const gpu::Image> image;
  HM_ASSIGN_OR_RETURN(image, session->Select(game_time));
  // Retain all images until GenerateOutput's completion fence has run, including
  // errors and multi-surface batches. The next call releases the previous set.
  scoreboard_camera_images_.push_back(image);
  return to_status(CompositeScoreboard(
      *image,
      {output.dataptr(), output.pitch(), int(output.width()), int(output.height()), gpu::PixelFormat::kRgba8},
      *scoreboard_camera_plan_,
      stream));
}
} // namespace hm::playcropper
