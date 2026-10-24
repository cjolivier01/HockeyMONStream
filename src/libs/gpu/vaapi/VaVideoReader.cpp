#include "hstream/src/libs/gpu/vaapi/VaVideoReader.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video-info-dma.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace hm::gpu {
namespace {
using Clock = std::chrono::steady_clock;

// Each VA factory is bound to one render node. A display context alone cannot
// move a factory to another device; select by the factory's device-path.
GstElement* MakeVaElement(const char* suffix, const std::filesystem::path& render) {
  auto* features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  GstElement* selected = nullptr;
  for (auto* item = features; item && !selected; item = item->next) {
    auto* factory = GST_ELEMENT_FACTORY(item->data);
    const char* plugin = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
    const std::string name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    const size_t length = std::char_traits<char>::length(suffix);
    if (!plugin || std::string(plugin) != "va" || name.size() < length ||
        name.compare(name.size() - length, length, suffix) != 0)
      continue;
    auto* element = gst_element_factory_create(factory, nullptr);
    if (!element)
      continue;
    gchar* device = nullptr;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device-path"))
      g_object_get(element, "device-path", &device, nullptr);
    std::error_code error;
    if (device && std::filesystem::canonical(device, error) == render && !error)
      selected = element;
    else
      gst_object_unref(element);
    g_free(device);
  }
  gst_plugin_feature_list_free(features);
  return selected;
}

// Called from a GStreamer streaming thread. No application work or exceptions.
void LinkVideo(GstElement*, GstPad* pad, gpointer parser) noexcept {
  if (g_strcmp0(GST_PAD_NAME(pad), "video_0") != 0)
    return;
  auto* sink = gst_element_get_static_pad(GST_ELEMENT(parser), "sink");
  if (sink) {
    (void)gst_pad_link(pad, sink);
    gst_object_unref(sink);
  }
}
} // namespace

struct VaVideoReader::Impl {
  VaVideoReaderConfig config;
  GstElement* pipeline = nullptr;
  GstElement* file = nullptr;
  GstElement* sink = nullptr;
  GstBus* bus = nullptr;
  std::atomic<bool> cancelled{false};
  absl::Status failure;
  bool opened = false;
  GstClockTime previous = GST_CLOCK_TIME_NONE;
  explicit Impl(const VaVideoReaderConfig& value) : config(value) {}
  ~Impl() {
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus)
      gst_object_unref(bus);
    if (pipeline)
      gst_object_unref(pipeline);
  }

  absl::Status Retire(absl::Status status) {
    if (failure.ok())
      failure = std::move(status);
    return failure;
  }
  absl::Status Check() {
    if (!failure.ok())
      return failure;
    if (cancelled.load(std::memory_order_relaxed))
      return Retire(absl::CancelledError("VA video reader cancelled"));
    auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (!message)
      return absl::OkStatus();
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const std::string detail =
        std::string(error ? error->message : "Unknown GStreamer error") + ": " + (debug ? debug : "");
    g_clear_error(&error);
    g_free(debug);
    gst_message_unref(message);
    return Retire(absl::UnavailableError(detail));
  }
  Clock::time_point Deadline() const {
    const auto now = Clock::now();
    const auto duration = std::chrono::nanoseconds(config.operation_timeout);
    return duration > Clock::time_point::max() - now ? Clock::time_point::max() : now + duration;
  }
  GstClockTime Poll(Clock::time_point deadline) const {
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
    return remaining <= 0 ? 0 : std::min<GstClockTime>(remaining, config.cancellation_poll_interval);
  }
  absl::Status WaitState(Clock::time_point deadline) {
    for (;;) {
      auto status = Check();
      if (!status.ok())
        return status;
      const auto wait = Poll(deadline);
      if (!wait)
        return Retire(absl::DeadlineExceededError("VA video state transition timed out"));
      const auto result = gst_element_get_state(pipeline, nullptr, nullptr, wait);
      if (result == GST_STATE_CHANGE_SUCCESS)
        return Check();
      if (result != GST_STATE_CHANGE_ASYNC) {
        status = Check();
        return status.ok() ? Retire(absl::UnavailableError("VA video state transition failed")) : status;
      }
    }
  }
  absl::Status State(GstState state, Clock::time_point deadline) {
    auto status = Check();
    if (!status.ok())
      return status;
    if (gst_element_set_state(pipeline, state) == GST_STATE_CHANGE_FAILURE) {
      status = Check();
      return status.ok() ? Retire(absl::UnavailableError("Cannot change VA video state")) : status;
    }
    return WaitState(deadline);
  }
  absl::Status Seek(GstClockTime time, Clock::time_point deadline) {
    auto status = State(GST_STATE_PAUSED, deadline);
    if (!status.ok())
      return status;
    if (!gst_element_seek_simple(
            pipeline, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), time))
      return Retire(absl::UnavailableError("VA video seek refused"));
    status = WaitState(deadline);
    if (!status.ok())
      return status;
    previous = GST_CLOCK_TIME_NONE;
    return State(GST_STATE_PLAYING, deadline);
  }
};

VaVideoReader::VaVideoReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VaVideoReader::~VaVideoReader() = default;

absl::StatusOr<std::unique_ptr<VaVideoReader>> VaVideoReader::Create(const VaVideoReaderConfig& config) {
  if (!gst_is_initialized() || (config.codec != VaVideoCodec::kH264 && config.codec != VaVideoCodec::kHevc) ||
      (config.format != GST_VIDEO_FORMAT_NV12 && config.format != GST_VIDEO_FORMAT_P010_10LE) ||
      !config.maximum_queued_frames || !config.operation_timeout || config.operation_timeout > G_MAXINT64 ||
      !config.cancellation_poll_interval || config.cancellation_poll_interval > config.operation_timeout)
    return absl::InvalidArgumentError("VA reader requires explicit codec, format, queue bound and finite deadlines");
  std::error_code error;
  const auto render = std::filesystem::canonical(config.render_node, error);
  if (error || !std::filesystem::is_character_file(render, error) || error)
    return absl::InvalidArgumentError("VA reader requires an existing DRM render node");
  auto impl = std::make_unique<Impl>(config);
  impl->pipeline = gst_pipeline_new(nullptr);
  impl->bus = gst_element_get_bus(impl->pipeline);
  // Add each element immediately so the pipeline owns every partial setup.
  auto add = [&](GstElement* element) -> GstElement* {
    if (element && !gst_bin_add(GST_BIN(impl->pipeline), element)) {
      gst_object_unref(element);
      return nullptr;
    }
    return element;
  };
  impl->file = add(gst_element_factory_make("filesrc", nullptr));
  auto* demux = add(gst_element_factory_make("qtdemux", nullptr));
  auto* parser =
      add(gst_element_factory_make(config.codec == VaVideoCodec::kHevc ? "h265parse" : "h264parse", nullptr));
  auto* decoder = add(MakeVaElement(config.codec == VaVideoCodec::kHevc ? "h265dec" : "h264dec", render));
  auto* convert = add(MakeVaElement("postproc", render));
  auto* filter = add(gst_element_factory_make("capsfilter", nullptr));
  impl->sink = add(gst_element_factory_make("appsink", nullptr));
  if (!impl->file || !demux || !parser || !decoder || !convert || !filter || !impl->sink)
    return absl::UnavailableError("Missing MP4/VA decoder, postprocessor or appsink on " + render.string());
  g_object_set(convert, "disable-passthrough", TRUE, nullptr);
  auto* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "DMA_DRM",
      "drm-format",
      G_TYPE_STRING,
      config.format == GST_VIDEO_FORMAT_NV12 ? "NV12" : "P010",
      nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:DMABuf", nullptr));
  g_object_set(filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(
      impl->sink,
      "sync",
      FALSE,
      "max-buffers",
      config.maximum_queued_frames,
      "drop",
      FALSE,
      "wait-on-eos",
      FALSE,
      "enable-last-sample",
      FALSE,
      nullptr);
  GstAppSinkCallbacks callbacks{};
  callbacks.propose_allocation = [](GstAppSink*, GstQuery* query, gpointer) -> gboolean {
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    return TRUE;
  };
  gst_app_sink_set_callbacks(GST_APP_SINK(impl->sink), &callbacks, nullptr, nullptr);
  g_signal_connect(demux, "pad-added", G_CALLBACK(LinkVideo), parser);
  if (!gst_element_link(impl->file, demux) ||
      !gst_element_link_many(parser, decoder, convert, filter, impl->sink, nullptr))
    return absl::UnavailableError("Cannot link VA video reader");
  return std::unique_ptr<VaVideoReader>(new VaVideoReader(std::move(impl)));
}

absl::Status VaVideoReader::OpenFile(const std::filesystem::path& path, GstClockTime start) {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  std::error_code error;
  if (start > G_MAXINT64 || !std::filesystem::is_regular_file(path, error) || error)
    return impl_->Retire(absl::InvalidArgumentError("VA reader requires a regular local file and finite seek time"));
  const auto deadline = impl_->Deadline();
  // GStreamer 1.24's VA transform closes its filter on PAUSED -> READY but
  // opens it only on NULL -> READY. READY alone cannot renegotiate a chapter.
  // Keep the elements, but fully reset their driver state before changing files.
  status = impl_->State(GST_STATE_NULL, deadline);
  if (!status.ok())
    return status;
  g_object_set(impl_->file, "location", path.c_str(), nullptr);
  status = impl_->State(GST_STATE_PAUSED, deadline);
  if (!status.ok())
    return status;
  status = impl_->Seek(start, deadline);
  if (status.ok())
    impl_->opened = true;
  return status;
}

absl::Status VaVideoReader::Seek(GstClockTime time) {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  if (!impl_->opened || time > G_MAXINT64)
    return impl_->Retire(absl::FailedPreconditionError("Seek requires an open video and finite time"));
  return impl_->Seek(time, impl_->Deadline());
}

absl::StatusOr<std::optional<VaVideoFrame>> VaVideoReader::Read() {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  if (!impl_->opened)
    return impl_->Retire(absl::FailedPreconditionError("Open a video before reading"));
  const auto deadline = impl_->Deadline();
  for (;;) {
    status = impl_->Check();
    if (!status.ok())
      return status;
    const auto wait = impl_->Poll(deadline);
    if (!wait)
      return impl_->Retire(absl::DeadlineExceededError("VA video frame timed out"));
    VaVideoFrame frame;
    frame.sample.reset(gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->sink), wait));
    status = impl_->Check();
    if (!status.ok())
      return status;
    if (!frame.sample) {
      if (gst_app_sink_is_eos(GST_APP_SINK(impl_->sink)))
        return std::nullopt;
      continue;
    }
    GstVideoInfoDmaDrm dma{};
    auto* buffer = gst_sample_get_buffer(frame.sample.get());
    const auto* segment = gst_sample_get_segment(frame.sample.get());
    if (!buffer || !segment || segment->format != GST_FORMAT_TIME ||
        !gst_video_info_dma_drm_from_caps(&dma, gst_sample_get_caps(frame.sample.get())) || dma.drm_modifier != 0 ||
        GST_VIDEO_INFO_FORMAT(&dma.vinfo) != impl_->config.format ||
        dma.vinfo.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE || dma.vinfo.fps_n <= 0 ||
        dma.vinfo.fps_d <= 0)
      return impl_->Retire(absl::DataLossError("VA decoder returned invalid linear video layout or timing"));
    frame.info = dma.vinfo;
    frame.time = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    frame.duration = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(frame.time) || !GST_CLOCK_TIME_IS_VALID(frame.duration) || !frame.duration ||
        (GST_CLOCK_TIME_IS_VALID(impl_->previous) && frame.time <= impl_->previous))
      return impl_->Retire(absl::DataLossError("VA decoder returned invalid or non-increasing frame timestamps"));
    impl_->previous = frame.time;
    return std::optional<VaVideoFrame>(std::move(frame));
  }
}

void VaVideoReader::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
}
} // namespace hm::gpu
