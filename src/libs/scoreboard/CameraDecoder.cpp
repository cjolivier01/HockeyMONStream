#include "hstream/src/libs/scoreboard/CameraDecoder.h"
#include "hstream/src/libs/gpu/vaapi/VaElement.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/scoreboard/CameraClock.h"

#include <gst/app/gstappsink.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video-info-dma.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace hm::scoreboard {
namespace {
using Clock = CameraClock;

} // namespace

struct CameraDecoder::Impl {
  CameraDecoderConfig config;
  GstElement* pipeline = nullptr;
  GstElement* file = nullptr;
  GstElement* demux = nullptr;
  GstElement* parser = nullptr;
  GstElement* decoder = nullptr;
  GstElement* encoded_input = nullptr;
  std::vector<GstElement*> discards;
  GstElement* sink = nullptr;
  GstElement* converter = nullptr;
  GstBus* bus = nullptr;
  std::atomic<bool> cancelled{false};
  absl::Status failure;
  bool opened = false;
  GstClockTime previous = GST_CLOCK_TIME_NONE;
  GstClockTime requested_start = 0;
  int time_base_n = 0, time_base_d = 0;
  GstClockTime stream_origin = GST_CLOCK_TIME_NONE;
  GstClockTime track_origin = 0;
  std::atomic<guint32> decoder_seek_sequence{GST_SEQNUM_INVALID};
  explicit Impl(const CameraDecoderConfig& value) : config(value) {}
  void FlushOutput() {
    // Flush the converter too: its output pool may be exhausted before a
    // buffer reaches appsink. NULL would otherwise wait on that acquisition.
    if (converter) {
      auto* pool = gst_base_transform_get_buffer_pool(GST_BASE_TRANSFORM(converter));
      if (pool) {
        gst_buffer_pool_set_flushing(pool, TRUE);
        gst_object_unref(pool);
      }
      auto* pad = gst_element_get_static_pad(converter, "sink");
      if (pad) {
        gst_pad_send_event(pad, gst_event_new_flush_start());
        gst_object_unref(pad);
      }
    }
  }
  ~Impl() {
    FlushOutput();
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus)
      gst_object_unref(bus);
    if (pipeline)
      gst_object_unref(pipeline);
  }

  // qtdemux may keep delivering audio after video EOS. An unlinked audio
  // tail becomes NOT_LINKED once the video pad is done. Discard compressed
  // non-selected streams without decoding them or retaining their buffers.
  // Only the streaming callback mutates discards until NULL joins that thread.
  static void LinkPad(GstElement* demux, GstPad* pad, gpointer data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    GstElement* target = self.encoded_input;
    try {
      if (!self.config.live && g_strcmp0(GST_PAD_NAME(pad), "video_0") != 0) {
        target = gst_element_factory_make("valve", nullptr);
        if (!target) {
          GST_ELEMENT_ERROR(demux, CORE, MISSING_PLUGIN, ("Missing compressed-stream discard sink"), (nullptr));
          return;
        }
        // A closed valve consumes buffers/events without preroll. A sink on
        // the demux thread can block before a reordered decoder prerolls.
        g_object_set(target, "drop", TRUE, "drop-mode", 0, nullptr);
        if (!gst_bin_add(GST_BIN(self.pipeline), target)) {
          gst_object_unref(target);
          GST_ELEMENT_ERROR(demux, CORE, FAILED, ("Cannot add compressed-stream discard sink"), (nullptr));
          return;
        }
        self.discards.push_back(target);
        if (!gst_element_sync_state_with_parent(target)) {
          GST_ELEMENT_ERROR(demux, CORE, STATE_CHANGE, ("Cannot start compressed-stream discard sink"), (nullptr));
          return;
        }
      }
      auto* sink = gst_element_get_static_pad(target, "sink");
      const bool linked = sink && gst_pad_link(pad, sink) == GST_PAD_LINK_OK;
      if (sink)
        gst_object_unref(sink);
      if (!linked)
        GST_ELEMENT_ERROR(demux, CORE, PAD, ("Cannot link MP4 stream"), ("%s", GST_PAD_NAME(pad)));
    } catch (...) {
      GST_ELEMENT_ERROR(demux, CORE, FAILED, ("Cannot prepare MP4 stream"), (nullptr));
    }
  }

  static GstPadProbeReturn CheckGeometry(GstPad*, GstPadProbeInfo* probe, gpointer data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    auto* event = GST_PAD_PROBE_INFO_EVENT(probe);
    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
      return GST_PAD_PROBE_OK;
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    const auto* structure = caps && gst_caps_get_size(caps) == 1 ? gst_caps_get_structure(caps, 0) : nullptr;
    int width = 0, height = 0;
    if (!structure || !gst_structure_get_int(structure, "width", &width) ||
        !gst_structure_get_int(structure, "height", &height) || width != self.config.source_width ||
        height != self.config.source_height) {
      GST_ELEMENT_ERROR(
          self.decoder, STREAM, FORMAT, ("Scoreboard camera geometry changed; select its region again"), (nullptr));
      return GST_PAD_PROBE_DROP;
    }
    GstVideoColorimetry color{};
    const auto* encoded = gst_structure_get_string(structure, "colorimetry");
    if (encoded && gst_video_colorimetry_from_string(&color, encoded) &&
        (color.transfer == GST_VIDEO_TRANSFER_SMPTE2084 || color.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67)) {
      GST_ELEMENT_ERROR(
          self.decoder,
          STREAM,
          FORMAT,
          ("Scoreboard camera requires SDR video; HDR needs explicit tone mapping"),
          (nullptr));
      return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
  }

  static gboolean SelectLiveStream(GstElement*, guint, GstCaps* caps, gpointer data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    const auto* structure = caps && gst_caps_get_size(caps) == 1 ? gst_caps_get_structure(caps, 0) : nullptr;
    const char* media = structure ? gst_structure_get_string(structure, "media") : nullptr;
    const char* encoding = structure ? gst_structure_get_string(structure, "encoding-name") : nullptr;
    return g_strcmp0(media, "video") == 0 &&
        g_strcmp0(encoding, self.config.codec == CameraCodec::kHevc ? "H265" : "H264") == 0;
  }

  static void CheckVideoPad(GstElement* demux, gpointer data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    auto* sink = gst_element_get_static_pad(self.encoded_input, "sink");
    const bool linked = sink && gst_pad_is_linked(sink);
    if (sink)
      gst_object_unref(sink);
    if (!linked)
      GST_ELEMENT_ERROR(demux, STREAM, WRONG_TYPE, ("MP4 demuxer exposed no selected video track"), (nullptr));
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
      return Retire(absl::CancelledError("Scoreboard video reader cancelled"));
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
        return Retire(absl::DeadlineExceededError("Scoreboard video state transition timed out"));
      const auto result = gst_element_get_state(pipeline, nullptr, nullptr, wait);
      if (result == GST_STATE_CHANGE_SUCCESS || result == GST_STATE_CHANGE_NO_PREROLL)
        return Check();
      if (result != GST_STATE_CHANGE_ASYNC) {
        status = Check();
        return status.ok() ? Retire(absl::UnavailableError("Scoreboard video state transition failed")) : status;
      }
    }
  }
  absl::Status State(GstState state, Clock::time_point deadline) {
    auto status = Check();
    if (!status.ok())
      return status;
    if (gst_element_set_state(pipeline, state) == GST_STATE_CHANGE_FAILURE) {
      status = Check();
      return status.ok() ? Retire(absl::UnavailableError("Cannot change Scoreboard video state")) : status;
    }
    return WaitState(deadline);
  }
  absl::Status Seek(GstClockTime time, Clock::time_point deadline) {
    auto status = State(GST_STATE_PAUSED, deadline);
    if (!status.ok())
      return status;
    const bool retire_hevc = config.backend == CameraDecodeBackend::kNvidia && config.codec == CameraCodec::kHevc;
    const auto sequence = gst_util_seqnum_next();
    if (retire_hevc) {
      decoder_seek_sequence.store(sequence);
      auto* pad = gst_element_get_static_pad(parser, "src");
      const auto probe = gst_pad_add_probe(
          pad,
          GstPadProbeType(
              GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
          [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
            auto& self = *static_cast<Impl*>(data);
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
              auto* event = GST_PAD_PROBE_INFO_EVENT(info);
              if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT &&
                  gst_event_get_seqnum(event) == self.decoder_seek_sequence.load())
                return GST_PAD_PROBE_REMOVE;
              // The guard drops pictures until the second seek. A short file
              // can reach EOS in that interval; forwarding it would block in
              // appsink's PAUSED preroll while decoder NULL waits on its pad.
              if (GST_EVENT_TYPE(event) == GST_EVENT_EOS)
                return GST_PAD_PROBE_DROP;
              return GST_PAD_PROBE_OK;
            }
            // Prevent compressed pictures from reaching surfaces being retired.
            return GST_PAD_PROBE_DROP;
          },
          this,
          nullptr);
      gst_object_unref(pad);
      if (!probe)
        return Retire(absl::UnavailableError("Cannot guard scoreboard decoder retirement"));
    }
    const auto seek = [&](guint32 seq) {
      auto* event = gst_event_new_seek(
          1.0,
          GST_FORMAT_TIME,
          GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_BEFORE),
          GST_SEEK_TYPE_SET,
          time,
          GST_SEEK_TYPE_NONE,
          GST_CLOCK_TIME_NONE);
      gst_event_set_seqnum(event, seq);
      // Send directly to the demuxer: appsink is deliberately flushing and
      // must not be asked to forward this upstream event through its sink pad.
      return gst_element_send_event(demux, event);
    };
    // PAUSED can still have a full decoder/converter behind its preroll.
    // Unblock downstream before qtdemux takes its stream lock for the seek.
    FlushOutput();
    if (auto* pool = gst_base_transform_get_buffer_pool(GST_BASE_TRANSFORM(converter))) {
      // The pads remain flushing until the seek's FLUSH_STOP. Rearm allocation
      // before that event can restart the streaming task.
      gst_buffer_pool_set_flushing(pool, FALSE);
      gst_object_unref(pool);
    }
    if (!seek(retire_hevc ? gst_util_seqnum_next() : sequence))
      return Retire(absl::UnavailableError("Scoreboard video seek refused"));
    if (retire_hevc) {
      // Retire conversion imports with the decoder surfaces. In particular,
      // Jetson can recycle a surface descriptor after NULL; a converter which
      // survives that retirement can retain an import of the previous image.
      if (gst_element_set_state(converter, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE ||
          gst_element_set_state(decoder, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE ||
          !gst_element_sync_state_with_parent(converter) || !gst_element_sync_state_with_parent(decoder) ||
          !seek(sequence))
        return Retire(absl::UnavailableError("Cannot retire scoreboard HEVC seek surfaces"));
    }
    status = WaitState(deadline);
    if (!status.ok())
      return status;
    previous = GST_CLOCK_TIME_NONE;
    return State(GST_STATE_PLAYING, deadline);
  }
};

CameraDecoder::CameraDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraDecoder::~CameraDecoder() = default;

absl::StatusOr<std::unique_ptr<CameraDecoder>> CameraDecoder::Create(const CameraDecoderConfig& config) {
  if (!gst_is_initialized() || (config.codec != CameraCodec::kH264 && config.codec != CameraCodec::kHevc) ||
      (config.backend != CameraDecodeBackend::kNvidia && config.backend != CameraDecodeBackend::kVa) ||
      !config.queued_frames || !config.operation_timeout || config.operation_timeout > G_MAXINT64 ||
      !config.cancellation_poll_interval || config.cancellation_poll_interval > config.operation_timeout ||
      config.source_width < 2 || config.source_height < 2 || config.region.left < 0 || config.region.top < 0 ||
      config.region.width < 2 || config.region.height < 2 || config.region.width > config.source_width ||
      config.region.height > config.source_height || config.region.left > config.source_width - config.region.width ||
      config.region.top > config.source_height - config.region.height || config.region.left % 2 ||
      config.region.top % 2 || config.region.width % 2 || config.region.height % 2)
    return absl::InvalidArgumentError(
        "Scoreboard reader requires explicit codec, geometry, queue bounds and deadlines");
  std::filesystem::path render;
  if (config.backend == CameraDecodeBackend::kVa) {
    std::error_code error;
    render = std::filesystem::canonical(config.render_node, error);
    if (error || !std::filesystem::is_character_file(render, error) || error)
      return absl::InvalidArgumentError("Scoreboard VA reader requires an existing DRM render node");
  }
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
  impl->file = add(gst_element_factory_make(config.live ? "rtspsrc" : "filesrc", nullptr));
  auto* demux = impl->demux = config.live ? impl->file : add(gst_element_factory_make("qtdemux", nullptr));
  impl->parser = add(gst_element_factory_make(config.codec == CameraCodec::kHevc ? "h265parse" : "h264parse", nullptr));
  impl->encoded_input = config.live
      ? add(gst_element_factory_make(config.codec == CameraCodec::kHevc ? "rtph265depay" : "rtph264depay", nullptr))
      : impl->parser;
  const bool nvidia = config.backend == CameraDecodeBackend::kNvidia;
  impl->decoder =
      add(nvidia ? gst_element_factory_make("nvv4l2decoder", nullptr)
                 : gpu::MakeVaElement(config.codec == CameraCodec::kHevc ? "h265dec" : "h264dec", render));
  auto* convert = impl->converter =
      add(nvidia ? gst_element_factory_make("nvvideoconvert", nullptr) : gpu::MakeVaElement("postproc", render));
  auto* filter = add(gst_element_factory_make("capsfilter", nullptr));
  impl->sink = add(gst_element_factory_make("appsink", nullptr));
  if (!impl->file || !demux || !impl->parser || !impl->encoded_input || !impl->decoder || !convert || !filter ||
      !impl->sink)
    return absl::UnavailableError("Missing scoreboard hardware decoder/converter or MP4/appsink element");
  GstCaps* caps = nullptr;
  if (nvidia) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(impl->decoder), "gpu-id"))
      g_object_set(impl->decoder, "gpu-id", config.gpu_id, nullptr);
    else if (config.gpu_id != 0)
      return absl::InvalidArgumentError("This scoreboard hardware decoder only supports GPU zero");
    g_object_set(
        impl->decoder,
        "num-extra-surfaces",
        config.extra_surfaces,
        "drop-frame-interval",
        0,
        "skip-frames",
        0,
        nullptr);
    const std::string crop = std::to_string(config.region.left) + ":" + std::to_string(config.region.top) + ":" +
        std::to_string(config.region.width) + ":" + std::to_string(config.region.height);
    // Use the same CUDA conversion/crop path on dGPU and Jetson. VIC's crop
    // resampling differs from a subregion of full-frame conversion.
    g_object_set(
        convert, "gpu-id", config.gpu_id, "src-crop", crop.c_str(), "compute-hw", 1, "nvbuf-memory-type", 2, nullptr);
    caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "RGBA",
        "width",
        G_TYPE_INT,
        config.region.width,
        "height",
        G_TYPE_INT,
        config.region.height,
        nullptr);
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  } else {
    g_object_set(convert, "disable-passthrough", TRUE, nullptr);
    caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "DMA_DRM", "drm-format", G_TYPE_STRING, "NV12", nullptr);
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:DMABuf", nullptr));
  }
  g_object_set(filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  auto* decoded_pad = gst_element_get_static_pad(impl->decoder, "src");
  gst_pad_add_probe(decoded_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, Impl::CheckGeometry, impl.get(), nullptr);
  gst_object_unref(decoded_pad);
  g_object_set(
      impl->sink,
      "sync",
      FALSE,
      "max-buffers",
      config.queued_frames,
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
  if (config.live)
    g_signal_connect(impl->file, "select-stream", G_CALLBACK(Impl::SelectLiveStream), impl.get());
  g_signal_connect(demux, "pad-added", G_CALLBACK(Impl::LinkPad), impl.get());
  g_signal_connect(demux, "no-more-pads", G_CALLBACK(Impl::CheckVideoPad), impl.get());
  if ((config.live ? !gst_element_link(impl->encoded_input, impl->parser) : !gst_element_link(impl->file, demux)) ||
      !gst_element_link_many(impl->parser, impl->decoder, convert, filter, impl->sink, nullptr))
    return absl::UnavailableError("Cannot link Scoreboard video reader");
  return std::unique_ptr<CameraDecoder>(new CameraDecoder(std::move(impl)));
}

absl::Status CameraDecoder::OpenFile(const std::filesystem::path& path, GstClockTime start) {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  std::error_code error;
  if (impl_->config.live || start > G_MAXINT64 || !std::filesystem::is_regular_file(path, error) || error)
    return impl_->Retire(
        absl::InvalidArgumentError("Scoreboard reader requires a regular local file and finite seek time"));
  const auto deadline = impl_->Deadline();
  const auto track = media::ReadMp4VideoTrack(
      path,
      impl_->config.operation_timeout,
      [this] { return impl_->cancelled.load(std::memory_order_relaxed); },
      [] { return CameraClock::now(); });
  if (!track.ok())
    return impl_->Retire(track.status());
  impl_->time_base_n = track->time_base_n;
  impl_->time_base_d = track->time_base_d;
  impl_->track_origin = track->start_ns;
  impl_->stream_origin = GST_CLOCK_TIME_NONE;
  // GStreamer 1.24's VA transform closes its filter on PAUSED -> READY but
  // opens it only on NULL -> READY. READY alone cannot renegotiate a chapter.
  // Keep the elements, but fully reset their driver state before changing files.
  impl_->FlushOutput();
  status = impl_->State(GST_STATE_NULL, deadline);
  if (!status.ok())
    return status;
  for (auto* discard : impl_->discards)
    gst_bin_remove(GST_BIN(impl_->pipeline), discard);
  impl_->discards.clear();
  g_object_set(impl_->file, "location", path.c_str(), nullptr);
  status = impl_->State(GST_STATE_PAUSED, deadline);
  if (!status.ok())
    return status;
  // qtdemux applies movie edit-list rounding independently of the track's
  // timebase. Bind its actual initial stream origin before any seek, so that
  // both timelines describe the same original picture even with an edit list.
  CameraDecodedFrame preroll;
  while (!preroll.sample) {
    status = impl_->Check();
    if (!status.ok())
      return status;
    const auto wait = impl_->Poll(deadline);
    if (!wait)
      return impl_->Retire(absl::DeadlineExceededError("Scoreboard initial frame timed out"));
    preroll.sample.reset(gst_app_sink_try_pull_preroll(GST_APP_SINK(impl_->sink), wait));
    if (!preroll.sample && gst_app_sink_is_eos(GST_APP_SINK(impl_->sink)))
      return impl_->Retire(absl::DataLossError("Scoreboard recording has no initial frame"));
  }
  auto* buffer = gst_sample_get_buffer(preroll.sample.get());
  const auto* segment = gst_sample_get_segment(preroll.sample.get());
  if (!buffer || !segment || segment->format != GST_FORMAT_TIME)
    return impl_->Retire(absl::DataLossError("Scoreboard initial frame has no time segment"));
  impl_->stream_origin = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  preroll.sample.reset();
  if (!GST_CLOCK_TIME_IS_VALID(impl_->stream_origin))
    return impl_->Retire(absl::DataLossError("Scoreboard initial frame has no stream timestamp"));
  const auto relative_start = start > impl_->track_origin ? start - impl_->track_origin : 0;
  if (relative_start > G_MAXINT64 || impl_->stream_origin > G_MAXINT64 - relative_start)
    return impl_->Retire(absl::OutOfRangeError("Scoreboard seek exceeds the stream timeline"));
  // NULL retired all decoder surfaces. Starting at zero needs no flushing
  // seek, and avoids racing a short file's demux task against its own preroll.
  impl_->previous = GST_CLOCK_TIME_NONE;
  impl_->requested_start = start;
  status = relative_start ? impl_->Seek(impl_->stream_origin + relative_start, deadline)
                          : impl_->State(GST_STATE_PLAYING, deadline);
  if (status.ok())
    impl_->opened = true;
  return status;
}

absl::Status CameraDecoder::OpenLive(
    const std::string& uri,
    GstClock* clock,
    GstClockTime base_time,
    unsigned latency_ms,
    bool ntp_sync) {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  if (!impl_->config.live || !GST_IS_CLOCK(clock) || !GST_CLOCK_TIME_IS_VALID(base_time) ||
      !gst_uri_is_valid(uri.c_str()) || (uri.rfind("rtsp://", 0) != 0 && uri.rfind("rtsps://", 0) != 0))
    return impl_->Retire(absl::InvalidArgumentError("Live scoreboard requires RTSP and the game's clock/base time"));
  const auto deadline = impl_->Deadline();
  impl_->FlushOutput();
  status = impl_->State(GST_STATE_NULL, deadline);
  if (!status.ok())
    return status;
  gst_pipeline_use_clock(GST_PIPELINE(impl_->pipeline), clock);
  gst_element_set_start_time(impl_->pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(impl_->pipeline, base_time);
  g_object_set(
      impl_->file,
      "location",
      uri.c_str(),
      "latency",
      latency_ms,
      "ntp-sync",
      gboolean(ntp_sync),
      "drop-on-latency",
      FALSE,
      nullptr);
  status = impl_->State(GST_STATE_PLAYING, deadline);
  if (status.ok())
    impl_->opened = true;
  return status;
}

absl::Status CameraDecoder::Seek(GstClockTime time) {
  auto status = impl_->Check();
  if (!status.ok())
    return status;
  if (impl_->config.live || !impl_->opened || time > G_MAXINT64)
    return impl_->Retire(absl::FailedPreconditionError("Seek requires an open video and finite time"));
  // Retire queued hardware surfaces before seeking an active reader. Some
  // qtdemux/decoder combinations take their stream lock before flushing and
  // deadlock against a full converter pool. Reopening also prevents old HEVC
  // surfaces from surviving a discontinuity.
  gchar* location = nullptr;
  g_object_get(impl_->file, "location", &location, nullptr);
  const std::filesystem::path path(location ? location : "");
  g_free(location);
  return OpenFile(path, time);
}

absl::StatusOr<std::optional<CameraDecodedFrame>> CameraDecoder::Read() {
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
      return impl_->Retire(absl::DeadlineExceededError("Scoreboard video frame timed out"));
    CameraDecodedFrame frame;
    frame.sample.reset(gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->sink), wait));
    status = impl_->Check();
    if (!status.ok())
      return status;
    if (!frame.sample) {
      if (gst_app_sink_is_eos(GST_APP_SINK(impl_->sink)))
        return std::nullopt;
      continue;
    }
    auto* buffer = gst_sample_get_buffer(frame.sample.get());
    const auto* segment = gst_sample_get_segment(frame.sample.get());
    auto* caps = gst_sample_get_caps(frame.sample.get());
    if (!buffer || !segment || segment->format != GST_FORMAT_TIME || !caps || gst_caps_get_size(caps) != 1)
      return impl_->Retire(absl::DataLossError("Scoreboard decoder returned invalid frame timing or caps"));
    if (impl_->config.backend == CameraDecodeBackend::kVa) {
      GstVideoInfoDmaDrm dma{};
      if (!gst_video_info_dma_drm_from_caps(&dma, caps) || dma.drm_modifier != 0 ||
          GST_VIDEO_INFO_FORMAT(&dma.vinfo) != GST_VIDEO_FORMAT_NV12)
        return impl_->Retire(absl::DataLossError("Scoreboard decoder did not return linear VA NV12"));
      frame.info = dma.vinfo;
    } else {
      if (!gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM") ||
          !gst_video_info_from_caps(&frame.info, caps) || GST_VIDEO_INFO_FORMAT(&frame.info) != GST_VIDEO_FORMAT_RGBA ||
          frame.info.width != impl_->config.region.width || frame.info.height != impl_->config.region.height)
        return impl_->Retire(absl::DataLossError("Scoreboard decoder did not return its cropped NVMM image"));
      frame.cropped = true;
    }
    if (frame.info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        (!impl_->config.live && (frame.info.fps_n <= 0 || frame.info.fps_d <= 0)))
      return impl_->Retire(
          absl::DataLossError(
              "Scoreboard camera requires progressive video with a frame rate: mode=" +
              std::to_string(frame.info.interlace_mode) + " fps=" + std::to_string(frame.info.fps_n) + "/" +
              std::to_string(frame.info.fps_d)));
    frame.time = impl_->config.live ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer))
                                    : gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    frame.duration = GST_BUFFER_DURATION(buffer);
    if (!impl_->config.live && GST_CLOCK_TIME_IS_VALID(frame.time)) {
      // qtdemux rounds a composition offset and its segment origin separately.
      // Their subtraction can move an original MP4 PTS by one nanosecond,
      // incorrectly repeating the preceding frame at a shared clock boundary.
      // Recover the original track tick only within that rounding tolerance;
      // do not infer a constant frame rate or hide real timestamp gaps.
      if (frame.time < impl_->stream_origin)
        return impl_->Retire(absl::DataLossError("Scoreboard frame precedes its original stream origin"));
      frame.time -= impl_->stream_origin;
      const auto unit = uint64_t(impl_->time_base_n) * GST_SECOND;
      const auto tick = gst_util_uint64_scale_round(frame.time, impl_->time_base_d, unit);
      const auto original = gst_util_uint64_scale(tick, unit, impl_->time_base_d);
      const auto difference = original > frame.time ? original - frame.time : frame.time - original;
      if (difference <= 1)
        frame.time = original;
      if (frame.time >= GST_CLOCK_TIME_NONE - impl_->track_origin)
        return impl_->Retire(absl::OutOfRangeError("Scoreboard frame exceeds the track timeline"));
      frame.time += impl_->track_origin;
    }
    if (!GST_CLOCK_TIME_IS_VALID(frame.time) || (!impl_->config.live && !GST_CLOCK_TIME_IS_VALID(frame.duration)) ||
        !frame.duration || (GST_CLOCK_TIME_IS_VALID(impl_->previous) && frame.time <= impl_->previous))
      return impl_->Retire(
          absl::DataLossError(
              "Scoreboard decoder returned invalid or non-increasing frame timestamps: time=" +
              std::to_string(frame.time) + " previous=" + std::to_string(impl_->previous) +
              " duration=" + std::to_string(frame.duration)));
    impl_->previous = frame.time;
    // Keep original presentation intervals through seeks. An accurate segment
    // seek can clip the preceding picture to the requested time, producing a
    // duplicate boundary after nanosecond normalization. Decode keyframe
    // preroll and discard only pictures ending before the requested interval.
    if (!impl_->config.live && frame.time < impl_->requested_start &&
        frame.duration <= impl_->requested_start - frame.time)
      continue;
    return std::optional<CameraDecodedFrame>(std::move(frame));
  }
}

void CameraDecoder::Cancel() noexcept {
  impl_->cancelled.store(true, std::memory_order_relaxed);
}
} // namespace hm::scoreboard
