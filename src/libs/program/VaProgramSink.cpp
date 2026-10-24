#include "hstream/src/libs/program/VaProgramSink.h"

#include <fcntl.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "hstream/src/libs/gpu/vaapi/VaElement.h"
#include "hstream/src/libs/program/EncodedColorimetry.h"

namespace hm::program {
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point Deadline(GstClockTime timeout) {
  const auto now = Clock::now();
  const auto delay = std::chrono::nanoseconds(timeout);
  return delay > Clock::time_point::max() - now ? Clock::time_point::max() : now + delay;
}
bool SetUnsigned(GstElement* element, const char* name, unsigned number) {
  auto* spec = element ? g_object_class_find_property(G_OBJECT_GET_CLASS(element), name) : nullptr;
  if (!spec || G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_UINT)
    return false;
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_UINT);
  g_value_set_uint(&value, number);
  const bool valid = !g_param_value_validate(spec, &value);
  if (valid)
    g_object_set_property(G_OBJECT(element), name, &value);
  g_value_unset(&value);
  return valid;
}
} // namespace
struct VaProgramSink::Impl {
  VaProgramSinkConfig config;
  std::unique_ptr<gpu::VaOutputPool> pool;
  GstElement *pipeline{}, *input{}, *sink{};
  GstBus* bus{};
  int fd{-1};
  GstClockTime previous{GST_CLOCK_TIME_NONE};
  bool retired{false}, eos_sent{false}, finished{false};
  std::atomic<bool> cancelled{false};
  ~Impl() {
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus)
      gst_object_unref(bus);
    if (input)
      gst_object_unref(input);
    if (sink)
      gst_object_unref(sink);
    if (pipeline)
      gst_object_unref(pipeline);
    if (fd >= 0)
      close(fd);
  }
  absl::Status Retire(absl::Status status) {
    retired = true;
    return status;
  }
  absl::Status Poll() {
    if (cancelled.load(std::memory_order_relaxed))
      return absl::CancelledError("Program output cancelled");
    if (retired)
      return absl::FailedPreconditionError("Program output retired after failure");
    while (auto* message =
               gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error{};
        gchar* debug{};
        gst_message_parse_error(message, &error, &debug);
        const std::string detail =
            std::string(error ? error->message : "Unknown output error") + (debug ? std::string(": ") + debug : "");
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        return Retire(absl::UnavailableError(detail));
      }
      gst_message_unref(message);
      if (!eos_sent)
        return Retire(absl::DataLossError("Program output ended before input EOS"));
      finished = true;
    }
    return absl::OkStatus();
  }
  void PauseUntil(Clock::time_point deadline) const {
    const auto remaining = deadline - Clock::now();
    if (remaining > Clock::duration::zero())
      std::this_thread::sleep_for(std::min(
          remaining, std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(config.cancellation_poll))));
  }
};
VaProgramSink::VaProgramSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VaProgramSink::~VaProgramSink() = default;
absl::StatusOr<std::unique_ptr<VaProgramSink>> VaProgramSink::Create(const VaProgramSinkConfig& config) {
  if (!config.pool_frames || !config.operation_timeout || config.operation_timeout > uint64_t(INT64_MAX) ||
      !config.cancellation_poll || config.cancellation_poll > config.operation_timeout || config.video.fps_n <= 0 ||
      config.video.fps_d <= 0)
    return absl::InvalidArgumentError("Program output requires explicit positive bounds and frame rate");
  std::string graph;
  switch (config.kind) {
    case VaProgramSinkKind::kDiscard:
      graph = "appsrc name=frames format=time ! fakesink name=output enable-last-sample=false";
      break;
    case VaProgramSinkKind::kHevcMp4:
      if (config.output_fd < 0 || !config.bitrate_kbps || !config.keyframe_interval ||
          !config.maximum_encoded_packet_bytes)
        return absl::InvalidArgumentError("HEVC output requires a descriptor, bitrate, key interval and packet bound");
      break;
    case VaProgramSinkKind::kPreview:
      if (config.pool_frames < 2)
        return absl::InvalidArgumentError(
            "GL preview requires two surfaces so a displayed frame cannot block its replacement");
      graph = "appsrc name=frames format=time ! glupload ! glcolorconvert ! glimagesink name=output";
      break;
    default:
      return absl::InvalidArgumentError("Unknown Program output kind");
  }
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  auto pool = gpu::VaOutputPool::Create(config.device, config.video, config.pool_frames);
  if (!pool.ok())
    return pool.status();
  impl->pool = std::move(*pool);
  if (config.kind == VaProgramSinkKind::kHevcMp4) {
    auto* selected = gpu::MakeVaElement("h265enc", impl->pool->render_node());
    if (!selected)
      return absl::UnavailableError("No HEVC VA encoder on the selected HIP render node");
    const std::string factory = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(selected)));
    gst_object_unref(selected);
    graph = "appsrc name=frames format=time ! " + factory +
        " name=encoder b-frames=0 ! h265parse ! mp4mux ! "
        "fdsink name=output enable-last-sample=false";
  }
  GError* error{};
  impl->pipeline = gst_parse_launch(graph.c_str(), &error);
  if (error || !impl->pipeline) {
    const std::string detail = error ? error->message : "Program output graph creation failed";
    g_clear_error(&error);
    return absl::UnavailableError(detail);
  }
  impl->input = gst_bin_get_by_name(GST_BIN(impl->pipeline), "frames");
  impl->sink = gst_bin_get_by_name(GST_BIN(impl->pipeline), "output");
  impl->bus = gst_element_get_bus(impl->pipeline);
  if (!impl->input || !impl->sink || !impl->bus)
    return absl::InternalError("Program output graph is missing an endpoint");
  impl->pool->SetDisplayContext(impl->pipeline);
  if (config.gl_context)
    gst_element_set_context(impl->pipeline, config.gl_context);
  gst_app_src_set_caps(GST_APP_SRC(impl->input), impl->pool->caps());
  // The surface pool bounds every in-flight frame, including the appsrc queue.
  // Never block inside push_buffer: Acquire owns cancellable backpressure.
  g_object_set(
      impl->input, "block", FALSE, "max-bytes", guint64(0), "max-buffers", guint64(config.pool_frames), nullptr);
  g_object_set(impl->sink, "sync", config.realtime, nullptr);
  if (config.kind == VaProgramSinkKind::kHevcMp4) {
    impl->fd = fcntl(config.output_fd, F_DUPFD_CLOEXEC, 0);
    struct stat file {};
    if (impl->fd < 0 || lseek(impl->fd, 0, SEEK_CUR) != 0 || fstat(impl->fd, &file) || !S_ISREG(file.st_mode) ||
        file.st_size != 0)
      return absl::InvalidArgumentError("MP4 output descriptor must select an empty regular file at offset zero");
    const auto flags = fcntl(impl->fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND))
      return absl::InvalidArgumentError("MP4 output requires a writable descriptor without append mode");
    g_object_set(impl->sink, "fd", impl->fd, nullptr);
    auto* encoder = gst_bin_get_by_name(GST_BIN(impl->pipeline), "encoder");
    auto* encoder_input = encoder ? gst_element_get_static_pad(encoder, "sink") : nullptr;
    auto* supported = encoder_input ? gst_pad_query_caps(encoder_input, nullptr) : nullptr;
    const bool compatible = supported && gst_caps_can_intersect(supported, impl->pool->caps());
    if (supported)
      gst_caps_unref(supported);
    if (encoder_input)
      gst_object_unref(encoder_input);
    if (!compatible) {
      if (encoder)
        gst_object_unref(encoder);
      return absl::InvalidArgumentError("The selected VA encoder cannot accept the output dimensions or format");
    }
    if (!SetUnsigned(encoder, "bitrate", config.bitrate_kbps) ||
        !SetUnsigned(encoder, "key-int-max", config.keyframe_interval)) {
      if (encoder)
        gst_object_unref(encoder);
      return absl::InvalidArgumentError("HEVC bitrate or key interval exceeds the selected encoder's limits");
    }
    const auto color =
        InstallEncodedColorimetry(encoder, EncodedCodec::kHevc, config.video, config.maximum_encoded_packet_bytes);
    gst_object_unref(encoder);
    if (!color.ok())
      return color;
  } else if (config.kind == VaProgramSinkKind::kPreview && config.window) {
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(impl->sink), config.window);
  }
  if (gst_element_set_state(impl->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    const auto status = impl->Poll();
    return status.ok() ? absl::UnavailableError("Program output could not start") : status;
  }
  return std::unique_ptr<VaProgramSink>(new VaProgramSink(std::move(impl)));
}
absl::StatusOr<GstBuffer*> VaProgramSink::Acquire() {
  auto& impl = *impl_;
  const auto deadline = Deadline(impl.config.operation_timeout);
  for (;;) {
    const auto status = impl.Poll();
    if (!status.ok())
      return status;
    if (impl.eos_sent)
      return absl::FailedPreconditionError("Cannot acquire Program output after EOS");
    auto buffer = impl.pool->Acquire();
    if (buffer.ok())
      return buffer;
    if (!absl::IsResourceExhausted(buffer.status()))
      return impl.Retire(buffer.status());
    if (Clock::now() >= deadline)
      return impl.Retire(absl::DeadlineExceededError("Program output pool stalled"));
    impl.PauseUntil(deadline);
  }
}
absl::Status VaProgramSink::Push(GstBuffer* buffer, GstClockTime time, GstClockTime duration) {
  std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> owner(buffer, gst_buffer_unref);
  auto& impl = *impl_;
  const auto status = impl.Poll();
  if (!status.ok())
    return status;
  if (impl.eos_sent)
    return absl::FailedPreconditionError("Cannot push Program output after EOS");
  if (!buffer || !gst_buffer_is_writable(buffer) || !GST_CLOCK_TIME_IS_VALID(time) ||
      !GST_CLOCK_TIME_IS_VALID(duration) || !duration || duration > uint64_t(INT64_MAX) ||
      time > uint64_t(INT64_MAX) - duration || (GST_CLOCK_TIME_IS_VALID(impl.previous) && time <= impl.previous))
    return impl.Retire(
        absl::InvalidArgumentError("Program output requires an owned surface and increasing finite timestamps"));
  GST_BUFFER_PTS(buffer) = time;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = duration;
  if (gst_app_src_push_buffer(GST_APP_SRC(impl.input), owner.release()) != GST_FLOW_OK)
    return impl.Retire(absl::UnavailableError("Program output refused a frame"));
  impl.previous = time;
  return absl::OkStatus();
}
absl::Status VaProgramSink::Finish() {
  auto& impl = *impl_;
  const auto status = impl.Poll();
  if (!status.ok())
    return status;
  if (impl.finished)
    return absl::OkStatus();
  if (!impl.eos_sent) {
    impl.eos_sent = true;
    if (gst_app_src_end_of_stream(GST_APP_SRC(impl.input)) != GST_FLOW_OK)
      return impl.Retire(absl::UnavailableError("Program output refused EOS"));
  }
  const auto deadline = Deadline(impl.config.operation_timeout);
  for (;;) {
    const auto polled = impl.Poll();
    if (!polled.ok() || impl.finished)
      return polled;
    if (Clock::now() >= deadline)
      return impl.Retire(absl::DeadlineExceededError("Program output did not finish EOS"));
    impl.PauseUntil(deadline);
  }
}
absl::Status VaProgramSink::SetWindow(uintptr_t window) {
  auto& impl = *impl_;
  const auto status = impl.Poll();
  if (!status.ok())
    return status;
  if (impl.config.kind != VaProgramSinkKind::kPreview || !window || impl.eos_sent)
    return absl::FailedPreconditionError("An active preview and positive window ID are required");
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(impl.sink), window);
  return absl::OkStatus();
}
void VaProgramSink::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
}
const GstVideoInfo& VaProgramSink::video_info() const {
  return impl_->config.video;
}
} // namespace hm::program
