#include "src/apps/hstream-ui/SourceCameraPlayer.h"

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <QtCore/QElapsedTimer>
#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#include "src/apps/hstream-ui/CameraExperimentPreviewOutput.h"
#include "src/apps/hstream-ui/SourceCameraTrack.h"
#ifndef HSTREAM_USE_HIP
#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#endif

namespace hm::ui {
struct SourceCameraPlayer::Impl {
  struct Chapter {
    QString file;
    SourceCameraTrack track;
    qint64 start_ms;
  };
  std::vector<Chapter> chapters;
  GstElement* graph{nullptr};
  GstElement* parser{nullptr};
  GstElement* decoder{nullptr};
  GstElement* video_input{nullptr};
  GstElement* sink{nullptr};
  GstBus* bus{nullptr};
  Status status;
  size_t chapter{0};
  quint64 window{0};
  std::uint64_t generation{0};
  qint64 timeout_ms{0};
  std::optional<qint64> seek_ms;
  GstClockTime stream_origin{GST_CLOCK_TIME_NONE};
  bool preroll{false};
  bool seeking{false};
  QElapsedTimer transition;
  std::shared_ptr<CameraExperimentPreviewOutput> output;

  void retire() {
    if (output)
      output->RetireSource();
    if (graph)
      gst_element_set_state(graph, GST_STATE_NULL);
    if (output)
      output->ReleaseSink();
#ifndef HSTREAM_USE_HIP
    if (sink && !output)
      hm::gpu_preview::quiesce(sink, 0);
#endif
    if (bus)
      gst_object_unref(bus);
    if (graph)
      gst_object_unref(graph);
    graph = parser = decoder = video_input = sink = nullptr;
    bus = nullptr;
    preroll = seeking = false;
    stream_origin = GST_CLOCK_TIME_NONE;
    status.open = status.ready = status.seeking = false;
    status.presented_ns = -1;
  }
  void fail(const QString& error) {
    status.error = error;
    status.playing = false;
    retire();
  }
  static void padAdded(GstElement* demux, GstPad* pad, gpointer data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    GstElement* target = self.video_input;
    if (g_strcmp0(GST_PAD_NAME(pad), "audio_0") == 0) {
      GError* error = nullptr;
      target = gst_parse_bin_from_description(
          self.output
              ? "queue name=camera_audio_input ! decodebin ! audioconvert ! audioresample ! identity name=camera_audio_output"
              : "queue name=camera_audio_input ! decodebin ! audioconvert ! audioresample ! autoaudiosink sync=true",
          FALSE,
          &error);
      if (!target || error) {
        if (target)
          gst_object_unref(target);
        GST_ELEMENT_ERROR(
            demux,
            CORE,
            MISSING_PLUGIN,
            ("Cannot create camera audio preview"),
            ("%s", error ? error->message : "missing audio elements"));
        g_clear_error(&error);
        return;
      }
      if (self.output) {
        std::string message;
        auto* audio = self.output->CreateAudioSink(&message);
        auto* tail = gst_bin_get_by_name(GST_BIN(target), "camera_audio_output");
        const bool linked = audio && gst_bin_add(GST_BIN(target), audio) && tail && gst_element_link(tail, audio);
        if (tail)
          gst_object_unref(tail);
        if (!linked) {
          gst_object_unref(target);
          GST_ELEMENT_ERROR(
              demux, CORE, MISSING_PLUGIN, ("Cannot create camera audio transport"), ("%s", message.c_str()));
          return;
        }
      }
      auto* queue = gst_bin_get_by_name(GST_BIN(target), "camera_audio_input");
      // Encoded audio must buffer ahead of video decoder reorder/preroll. A
      // packet-count bound can deadlock both streams before the first picture.
      // One MiB is a memory safety ceiling for this compressed-only queue; it
      // never contains video frames or an unbounded PCM history.
      if (queue)
        g_object_set(
            queue, "max-size-buffers", 0U, "max-size-bytes", 1024U * 1024U, "max-size-time", guint64{0}, nullptr);
      auto* input = queue ? gst_element_get_static_pad(queue, "sink") : nullptr;
      auto* ghost = input ? gst_ghost_pad_new("sink", input) : nullptr;
      if (input)
        gst_object_unref(input);
      if (queue)
        gst_object_unref(queue);
      if (ghost)
        gst_object_ref_sink(ghost);
      const bool added = ghost && gst_element_add_pad(target, ghost);
      if (ghost)
        gst_object_unref(ghost);
      if (!added) {
        gst_object_unref(target);
        GST_ELEMENT_ERROR(demux, CORE, PAD, ("Cannot attach camera audio input"), (nullptr));
        return;
      }
      gst_bin_add(GST_BIN(self.graph), target);
      gst_element_sync_state_with_parent(target);
    } else if (g_strcmp0(GST_PAD_NAME(pad), "video_0") != 0) {
      // Discard extra compressed tracks and metadata without blocking preroll.
      target = gst_element_factory_make("valve", nullptr);
      if (!target) {
        GST_ELEMENT_ERROR(demux, CORE, MISSING_PLUGIN, ("Missing stream discard element"), (nullptr));
        return;
      }
      g_object_set(target, "drop", TRUE, nullptr);
      gst_bin_add(GST_BIN(self.graph), target);
      gst_element_sync_state_with_parent(target);
    }
    auto* input = gst_element_get_static_pad(target, "sink");
    const auto linked = input ? gst_pad_link(pad, input) : GST_PAD_LINK_REFUSED;
    if (input)
      gst_object_unref(input);
    if (linked != GST_PAD_LINK_OK)
      GST_ELEMENT_ERROR(
          demux,
          CORE,
          PAD,
          ("Cannot link camera recording stream %s: %s", GST_PAD_NAME(pad), gst_pad_link_get_name(linked)),
          (nullptr));
  }
  bool openChapter(size_t index) {
    retire();
    chapter = index;
    const auto& item = chapters.at(index);
    graph = gst_pipeline_new("source-camera-preview");
    if (graph && output) {
      auto* clock = gst_system_clock_obtain();
      gst_pipeline_use_clock(GST_PIPELINE(graph), clock);
      gst_object_unref(clock);
    }
    auto* file = gst_element_factory_make("filesrc", nullptr);
    auto* demux = gst_element_factory_make(item.track.demux, nullptr);
    const bool hevc = item.track.codec == media::VideoCodec::kHevc;
    parser = gst_element_factory_make(hevc ? "h265parse" : "h264parse", nullptr);
    video_input = gst_element_factory_make("queue", nullptr);
#ifdef HSTREAM_USE_HIP
    decoder = gst_element_factory_make(hevc ? "vah265dec" : "vah264dec", nullptr);
    // VA exports DMABufs directly to EGL/GL. Never negotiate system-memory video.
    auto* convert = gst_element_factory_make("identity", nullptr);
    sink = output ? nullptr : gst_element_factory_make("glimagesink", nullptr);
    auto* caps = gst_caps_from_string("video/x-raw(memory:DMABuf)");
#else
    decoder = gst_element_factory_make("nvv4l2decoder", nullptr);
    auto* convert = gst_element_factory_make("nvvideoconvert", nullptr);
    sink = output ? nullptr : gst_element_factory_make("hmgpupreviewsink", nullptr);
    auto* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
#endif
    if (output) {
      std::string message;
      sink = output->CreateSink({0, true, true, false, false, false, {}}, &message);
    }
    auto* filter = gst_element_factory_make("capsfilter", nullptr);
    const std::vector<GstElement*> elements{file, demux, video_input, parser, decoder, convert, filter, sink};
    if (!graph || std::any_of(elements.begin(), elements.end(), [](auto* e) { return !e; })) {
      for (auto* element : elements)
        if (element)
          gst_object_unref(element);
      parser = decoder = video_input = sink = nullptr;
      gst_caps_unref(caps);
      fail("Camera preview hardware decoder or renderer is unavailable.");
      return false;
    }
    for (auto* element : elements)
      gst_bin_add(GST_BIN(graph), element);
    g_object_set(file, "location", item.file.toLocal8Bit().constData(), nullptr);
    // Separate demux from video preroll so audio can receive its first packet.
    g_object_set(video_input, "max-size-buffers", 2U, "max-size-bytes", 0U, "max-size-time", guint64{0}, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "sync", TRUE, "async", TRUE, "qos", TRUE, "enable-last-sample", TRUE, nullptr);
    if (output) {
      output->SetGeneration(++generation);
    } else {
#ifdef HSTREAM_USE_HIP
      gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), window);
#else
      g_object_set(sink, "window-id", window, "channel", "source-camera", "enable-last-sample", TRUE, nullptr);
      hm::gpu_preview::set_source_geometry(sink, item.track.width, item.track.height);
#endif
    }
    g_signal_connect(demux, "pad-added", G_CALLBACK(padAdded), this);
    if (!gst_element_link(file, demux) ||
        !gst_element_link_many(video_input, parser, decoder, convert, filter, sink, nullptr)) {
      fail("Could not link the camera hardware preview graph.");
      return false;
    }
    bus = gst_element_get_bus(graph);
    status.open = true;
    status.width = item.track.width;
    status.height = item.track.height;
    preroll = true;
    transition.restart();
    if (gst_element_set_state(graph, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      fail("Could not preroll the camera recording.");
      return false;
    }
    return true;
  }
};

SourceCameraPlayer::SourceCameraPlayer() : impl_(std::make_unique<Impl>()) {}
SourceCameraPlayer::~SourceCameraPlayer() {
  Close();
}

bool SourceCameraPlayer::Open(
    const QStringList& files,
    quint64 window,
    qint64 timeout_ms,
    QString* error,
    std::shared_ptr<CameraExperimentPreviewOutput> output) {
  Close();
  auto& s = *impl_;
  const auto fail = [&](const QString& message) {
    s.fail(message);
    if (error)
      *error = message;
    return false;
  };
  if (files.isEmpty() || (!window && !output) || timeout_ms <= 0 ||
      timeout_ms > std::numeric_limits<qint64>::max() / GST_MSECOND)
    return fail("Camera preview needs original recordings, a native window and a positive timeout.");
  gst_init(nullptr, nullptr);
#ifndef HSTREAM_USE_HIP
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  if (!output && (!hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements()))
    return fail("Camera preview requires a GPU renderer and an X11 display.");
#endif
  s.window = window;
  s.output = std::move(output);
  s.timeout_ms = timeout_ms;
  QElapsedTimer probe;
  probe.start();
  for (const auto& file : files) {
    const auto remaining = timeout_ms - probe.elapsed();
    if (remaining <= 0)
      return fail("Camera recording metadata timed out.");
    auto track = ReadSourceCameraTrack(file, remaining);
    if (!track.ok())
      return fail(QString::fromStdString(track.status().ToString()));
    const qint64 duration = track->duration_ns / GST_MSECOND;
    if (duration <= 0 || duration > std::numeric_limits<qint64>::max() - s.status.duration_ms)
      return fail("Camera recording has an invalid video duration.");
    s.chapters.push_back({file, *track, s.status.duration_ms});
    s.status.duration_ms += duration;
  }
  s.seek_ms = 0;
  if (!s.openChapter(0)) {
    if (error)
      *error = s.status.error;
    return false;
  }
  return true;
}
void SourceCameraPlayer::Play() {
  auto& s = *impl_;
  if (!s.graph)
    return;
  s.status.playing = true;
  if (s.output && s.status.ready && !s.seek_ms && !s.seeking)
    s.output->SetAudioEnabled(true);
  if (s.status.position_ms >= s.status.duration_ms)
    Seek(0);
  if (s.status.ready && !s.seek_ms && !s.seeking &&
      gst_element_set_state(s.graph, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    s.fail("Could not play the camera recording.");
}
void SourceCameraPlayer::Pause() {
  auto& s = *impl_;
  s.status.playing = false;
  if (s.output)
    s.output->SetAudioEnabled(false);
  if (s.graph && gst_element_set_state(s.graph, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    s.fail("Could not pause the camera recording.");
}
bool SourceCameraPlayer::Redraw() {
  auto& s = *impl_;
  if (!s.sink || !s.status.ready || s.status.playing || s.seeking || s.seek_ms)
    return false;
  if (s.output) {
    s.output->Redraw();
    return true;
  }
#ifdef HSTREAM_USE_HIP
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(s.sink));
  return true;
#else
  return hm::gpu_preview::redraw_last_sample(s.sink);
#endif
}
void SourceCameraPlayer::Seek(qint64 position_ms) {
  auto& s = *impl_;
  if (!s.graph)
    return;
  const auto target = std::clamp(position_ms, qint64{0}, s.status.duration_ms - 1);
  size_t index = 0;
  while (index + 1 < s.chapters.size() && s.chapters[index + 1].start_ms <= target)
    ++index;
  s.seek_ms = target;
  s.status.seeking = true;
  if (s.output) {
    s.output->SetAudioEnabled(false);
    if (gst_element_set_state(s.graph, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      s.fail("Could not pause the camera before seeking.");
      return;
    }
  }
  s.status.position_ms = target;
  if (index != s.chapter)
    s.openChapter(index);
}
void SourceCameraPlayer::Close() {
  impl_->retire();
  impl_->chapters.clear();
  impl_->status = {};
  impl_->seek_ms.reset();
  impl_->output.reset();
}
SourceCameraPlayer::Status SourceCameraPlayer::GetStatus() const {
  return impl_->status;
}

SourceCameraPlayer::Status SourceCameraPlayer::Poll() {
  auto& s = *impl_;
  if (!s.graph)
    return s.status;
  bool eos = false;
  QString error;
  while (auto* message = gst_bus_pop(s.bus)) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* detail = nullptr;
      gst_message_parse_error(message, &detail, nullptr);
      error = detail ? QString::fromUtf8(detail->message) : "Camera preview failed.";
      g_clear_error(&detail);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      eos = true;
    }
    gst_message_unref(message);
  }
  if (!error.isEmpty()) {
    s.fail(error);
    return s.status;
  }
  // A recovery seek can leave an ASYNC_DONE from the positioning seek queued.
  // The current graph state, not an older bus message, owns completion.
  const bool settled = gst_element_get_state(s.graph, nullptr, nullptr, 0) == GST_STATE_CHANGE_SUCCESS;
  if (s.seeking && settled)
    s.seeking = false;
  if (s.preroll && settled) {
    GstSample* sample = nullptr;
    g_object_get(s.sink, "last-sample", &sample, nullptr);
    const auto* segment = sample ? gst_sample_get_segment(sample) : nullptr;
    auto* buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    if (segment && buffer && GST_BUFFER_PTS_IS_VALID(buffer))
      s.stream_origin = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    if (sample)
      gst_sample_unref(sample);
    // MP4 edit lists and Matroska use different timestamp origins. Convert the
    // first decoded frame through its actual segment without reading pixels.
    if (!GST_CLOCK_TIME_IS_VALID(s.stream_origin) ||
        s.stream_origin > guint64(std::numeric_limits<gint64>::max() - s.chapters[s.chapter].track.duration_ns)) {
      s.fail("Camera preroll did not establish a bounded video timeline.");
      return s.status;
    }
    s.preroll = false;
    s.status.ready = true;
  }
  if ((s.preroll || s.seeking) && s.transition.elapsed() > s.timeout_ms) {
    s.fail("Camera preview timed out while opening or seeking.");
    return s.status;
  }
  if (s.status.ready && !s.seeking && s.seek_ms) {
    if (s.output && !settled) {
      s.status.seeking = true;
      return s.status;
    }
    const auto target = *s.seek_ms;
    s.seek_ms.reset();
    const auto& item = s.chapters[s.chapter];
    if (s.output)
      s.output->SetGeneration(++s.generation);
#ifndef HSTREAM_USE_HIP
    else
      hm::gpu_preview::set_renderer_generation(s.sink, ++s.generation);
#endif
    s.status.presented_ns = -1;
    const auto seek = [&] {
      return gst_element_seek(
          s.graph,
          1.0,
          GST_FORMAT_TIME,
          GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
          GST_SEEK_TYPE_SET,
          s.stream_origin + (target - item.start_ms) * GST_MSECOND,
          GST_SEEK_TYPE_SET,
          s.stream_origin + item.track.duration_ns);
    };
    if (!seek()) {
      s.fail("Camera recording rejected the seek.");
      return s.status;
    }
#ifndef HSTREAM_USE_HIP
    // NVDEC can retain stale regions of an 8K HEVC picture after a flushing
    // seek from preroll. Position the demuxer, retire the decoder's surfaces,
    // then repeat the exact seek with a fresh decoder, as URI-playlist and
    // highlight playback do. Keep the target and requested Play/Pause state.
    if (item.track.codec == media::VideoCodec::kHevc) {
      if (gst_element_set_state(s.decoder, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE ||
          !gst_element_sync_state_with_parent(s.decoder)) {
        s.fail("Could not restart the camera decoder after seeking.");
        return s.status;
      }
      // Any picture from the positioning seek belongs to the retired decoder.
      if (s.output)
        s.output->SetGeneration(++s.generation);
      else
        hm::gpu_preview::set_renderer_generation(s.sink, ++s.generation);
      if (!seek()) {
        s.fail("Camera recording rejected the seek after decoder restart.");
        return s.status;
      }
    }
#endif
    s.seeking = true;
    s.transition.restart();
    eos = false;
  }
  if (eos && !s.seek_ms && !s.seeking) {
    if (s.chapter + 1 < s.chapters.size()) {
      s.seek_ms = s.chapters[s.chapter + 1].start_ms;
      s.openChapter(s.chapter + 1);
    } else {
      Pause();
      s.status.position_ms = s.status.duration_ms;
    }
  } else if (s.status.ready && !s.seeking && !s.seek_ms) {
    if (s.output)
      s.output->SetAudioEnabled(s.status.playing);
    if (s.status.playing && gst_element_set_state(s.graph, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      s.fail("Could not resume the camera recording.");
    gint64 position = 0;
    if (s.graph && s.status.position_ms < s.status.duration_ms &&
        gst_element_query_position(s.graph, GST_FORMAT_TIME, &position) && position >= 0)
      s.status.position_ms = std::min(
          s.status.duration_ms,
          s.chapters[s.chapter].start_ms +
              std::max<qint64>(0, position - qint64(s.stream_origin)) / qint64(GST_MSECOND));
  }
  if (s.sink && s.status.ready && !s.seeking && !s.seek_ms) {
    if (s.output) {
      std::uint64_t generation = 0;
      GstClockTime pts = GST_CLOCK_TIME_NONE;
      if (s.output->Presented(&generation, &pts) && generation == s.generation)
        s.status.presented_ns = pts;
    } else {
#ifdef HSTREAM_USE_HIP
      GstSample* sample = nullptr;
      g_object_get(s.sink, "last-sample", &sample, nullptr);
      if (sample) {
        auto* buffer = gst_sample_get_buffer(sample);
        if (buffer && GST_BUFFER_PTS_IS_VALID(buffer))
          s.status.presented_ns = GST_BUFFER_PTS(buffer);
        gst_sample_unref(sample);
      }
#else
      std::uint64_t generation = 0;
      GstClockTime pts = GST_CLOCK_TIME_NONE;
      if (hm::gpu_preview::presented_frame(s.sink, &generation, &pts) && generation == s.generation)
        s.status.presented_ns = pts;
#endif
    }
  }
  s.status.seeking = s.preroll || s.seeking || s.seek_ms.has_value() ||
      (s.graph && gst_element_get_state(s.graph, nullptr, nullptr, 0) != GST_STATE_CHANGE_SUCCESS);
  return s.status;
}
} // namespace hm::ui
