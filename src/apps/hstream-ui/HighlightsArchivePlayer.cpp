#include "src/apps/hstream-ui/HighlightsArchivePlayer.h"

#include "hstream/src/apps/apps-common/HmGpuPreview.h"

#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace hm::ui {
namespace {

// The renderer letterboxes into the preview widget, so there is nothing to be
// gained from pushing native 4K or 8K frames at it.
constexpr int kPreviewMaximumWidth = 1280;
constexpr int kPreviewMaximumHeight = 720;

bool fail(QString* error, const QString& message) {
  if (error)
    *error = message;
  return false;
}

GstCaps* preview_caps(int width, int height) {
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
  if (width > 0 && height > 0)
    gst_caps_set_simple(caps, "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  return caps;
}

void scaled_preview_size(int width, int height, int* out_width, int* out_height) {
  *out_width = 0;
  *out_height = 0;
  if (width <= 0 || height <= 0)
    return;
  const double scale = std::min(
      1.0,
      std::min(
          static_cast<double>(kPreviewMaximumWidth) / width, static_cast<double>(kPreviewMaximumHeight) / height));
  // NVMM RGBA conversion wants even dimensions.
  *out_width = std::max(2, static_cast<int>(std::lround(width * scale)) & ~1);
  *out_height = std::max(2, static_cast<int>(std::lround(height * scale)) & ~1);
}

gint64 to_nanoseconds(qint64 milliseconds) {
  return static_cast<gint64>(milliseconds) * GST_MSECOND;
}

} // namespace

struct HighlightsArchivePlayer::Impl {
  GstElement* graph{nullptr};
  GstElement* decoder{nullptr};
  GstElement* video_convert{nullptr};
  GstElement* sink{nullptr};
  GstElement* audio_convert{nullptr};
  GstBus* bus{nullptr};
  quint64 window_id{0};
  int preview_width{0};
  int preview_height{0};

  QVector<Segment> segments;
  int current{-1};
  bool loop{false};
  bool playing{false};
  bool finished{false};
  bool prerolled{false};
  bool pending_play{false};

  std::mutex mutex;
  bool video_linked{false};
  bool audio_linked{false};
  QString error;

  void set_error(const QString& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (error.isEmpty())
      error = message;
  }

  QString take_error() {
    std::lock_guard<std::mutex> lock(mutex);
    QString taken;
    taken.swap(error);
    return taken;
  }

  bool seek(int index, bool flush);
  void clear_graph();

  // uridecodebin exposes decoded pads on its streaming threads.
  static void pad_added(GstElement*, GstPad* pad, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps)
      caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
      if (caps)
        gst_caps_unref(caps);
      return;
    }
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    const bool video = g_str_has_prefix(name, "video/x-raw");
    const bool audio = g_str_has_prefix(name, "audio/x-raw");
    gst_caps_unref(caps);
    GstElement* target = nullptr;
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      if (video && !self->video_linked)
        target = self->video_convert;
      else if (audio && !self->audio_linked && self->audio_convert)
        target = self->audio_convert;
      if (!target)
        return;
      (video ? self->video_linked : self->audio_linked) = true;
    }
    GstPad* target_pad = gst_element_get_static_pad(target, "sink");
    const bool linked = target_pad && gst_pad_link(pad, target_pad) == GST_PAD_LINK_OK;
    if (target_pad)
      gst_object_unref(target_pad);
    if (!linked) {
      std::lock_guard<std::mutex> lock(self->mutex);
      (video ? self->video_linked : self->audio_linked) = false;
      if (video)
        self->error = "Could not connect the archive video decoder to the preview renderer.";
    }
  }

  // Keep decoding on NVDEC. A software 4K/8K HEVC decoder cannot sustain
  // realtime playback on this hardware, and the renderer wants NVMM buffers.
  static gint autoplug_select(GstElement*, GstPad*, GstCaps*, GstElementFactory* factory, gpointer) noexcept {
    const gchar* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    const gchar* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    // GstAutoplugSelectResult is private to playback: TRY=0, SKIP=2.
    return klass && g_strrstr(klass, "Decoder/Video") && g_strcmp0(name, "nvv4l2decoder") != 0 ? 2 : 0;
  }
};

HighlightsArchivePlayer::HighlightsArchivePlayer() : impl_(std::make_unique<Impl>()) {}

HighlightsArchivePlayer::~HighlightsArchivePlayer() {
  Close();
}

bool HighlightsArchivePlayer::Available() {
  gst_init(nullptr, nullptr);
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  return hm::gpu_preview::renderer_available() && hm::gpu_preview::register_elements();
}

bool HighlightsArchivePlayer::Open(
    const QString& archive_path,
    quint64 window_id,
    int width,
    int height,
    QString* error) {
  Close();
  if (archive_path.isEmpty() || window_id == 0)
    return fail(error, "Archive preview needs a readable archive and a native window.");
  if (!Available())
    return fail(error, "Archive preview requires NVIDIA graphics and an X11 display.");

  auto& s = *impl_;
  s.window_id = window_id;
  scaled_preview_size(width, height, &s.preview_width, &s.preview_height);

  s.graph = gst_pipeline_new("highlights-archive");
  s.decoder = gst_element_factory_make("uridecodebin", "archive-decoder");
  s.video_convert = gst_element_factory_make("nvvideoconvert", "archive-rgba");
  GstElement* video_caps = gst_element_factory_make("capsfilter", "archive-caps");
  s.sink = gst_element_factory_make("hmgpupreviewsink", "archive-renderer");
  s.audio_convert = gst_element_factory_make("audioconvert", "archive-audio");
  GstElement* audio_resample = gst_element_factory_make("audioresample", "archive-audio-resample");
  GstElement* audio_sink = gst_element_factory_make("autoaudiosink", "archive-audio-sink");
  const bool audio = s.audio_convert && audio_resample && audio_sink;
  if (!s.graph || !s.decoder || !s.video_convert || !video_caps || !s.sink) {
    // Nothing has reached the bin yet, so every element that was created still
    // holds its floating reference.
    for (GstElement* element :
         {s.decoder, s.video_convert, video_caps, s.sink, s.audio_convert, audio_resample, audio_sink}) {
      if (element)
        gst_object_unref(g_object_ref_sink(element));
    }
    s.decoder = s.video_convert = s.sink = s.audio_convert = nullptr;
    s.clear_graph();
    return fail(error, "Could not create the NVIDIA decoder or GPU renderer. Check the HStream runtime plugins.");
  }
  gst_bin_add_many(GST_BIN(s.graph), s.decoder, s.video_convert, video_caps, s.sink, nullptr);
  if (audio) {
    gst_bin_add_many(GST_BIN(s.graph), s.audio_convert, audio_resample, audio_sink, nullptr);
  } else {
    for (GstElement* element : {s.audio_convert, audio_resample, audio_sink}) {
      if (element)
        gst_object_unref(g_object_ref_sink(element));
    }
    s.audio_convert = nullptr;
  }

  gchar* uri = gst_filename_to_uri(archive_path.toLocal8Bit().constData(), nullptr);
  if (!uri) {
    s.clear_graph();
    return fail(error, "The archive path does not name a local file: " + archive_path);
  }
  g_object_set(s.decoder, "uri", uri, nullptr);
  g_free(uri);
  g_signal_connect(s.decoder, "pad-added", G_CALLBACK(Impl::pad_added), &s);
  g_signal_connect(s.decoder, "autoplug-select", G_CALLBACK(Impl::autoplug_select), &s);

  g_object_set(s.video_convert, "output-buffers", 4, nullptr);
  GstCaps* caps = preview_caps(s.preview_width, s.preview_height);
  g_object_set(video_caps, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(
      s.sink,
      "window-id",
      static_cast<guint64>(window_id),
      "channel",
      "highlights",
      "sync",
      TRUE,
      "async",
      TRUE,
      // Continuous playback, so let a frame the renderer cannot reach in time
      // be dropped rather than running the whole reel behind the clock.
      "qos",
      TRUE,
      "enable-last-sample",
      FALSE,
      nullptr);
  if (s.preview_width > 0 && s.preview_height > 0)
    hm::gpu_preview::set_source_geometry(s.sink, s.preview_width, s.preview_height);

  if (!gst_element_link(s.video_convert, video_caps) || !gst_element_link(video_caps, s.sink) ||
      (audio && !gst_element_link_many(s.audio_convert, audio_resample, audio_sink, nullptr))) {
    s.clear_graph();
    return fail(error, "Could not link the archive preview graph.");
  }
  s.bus = gst_element_get_bus(s.graph);
  if (gst_element_set_state(s.graph, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    s.clear_graph();
    return fail(error, "Could not open the archive for preview: " + archive_path);
  }
  return true;
}

bool HighlightsArchivePlayer::Play(const QVector<Segment>& segments, bool loop, QString* error) {
  auto& s = *impl_;
  if (!s.graph)
    return fail(error, "No archive is open for preview.");
  if (segments.isEmpty())
    return fail(error, "No highlight intervals were selected.");
  for (const Segment& segment : segments) {
    if (segment.start_ms < 0 || segment.end_ms <= segment.start_ms)
      return fail(error, "A highlight interval does not describe a positive range inside the archive.");
  }
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.error.clear();
  }
  s.segments = segments;
  s.loop = loop;
  s.current = 0;
  s.finished = false;
  s.playing = true;
  if (!s.prerolled) {
    // uridecodebin's pads must exist before a seek can reach the demuxer.
    // Poll() issues this seek once the graph has prerolled.
    s.pending_play = true;
    return true;
  }
  s.pending_play = false;
  if (!s.seek(0, true)) {
    s.playing = false;
    return fail(error, "The archive decoder rejected the seek to the first highlight.");
  }
  return true;
}

bool HighlightsArchivePlayer::Impl::seek(int index, bool flush) {
  if (index < 0 || index >= segments.size())
    return false;
  current = index;
  const Segment& segment = segments[index];
  GstSeekFlags flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_SEGMENT);
  if (flush)
    flags = static_cast<GstSeekFlags>(flags | GST_SEEK_FLAG_FLUSH);
  if (!gst_element_seek(
          graph,
          1.0,
          GST_FORMAT_TIME,
          flags,
          GST_SEEK_TYPE_SET,
          to_nanoseconds(segment.start_ms),
          GST_SEEK_TYPE_SET,
          to_nanoseconds(segment.end_ms)))
    return false;
  if (playing)
    gst_element_set_state(graph, GST_STATE_PLAYING);
  return true;
}

void HighlightsArchivePlayer::Stop() {
  auto& s = *impl_;
  s.playing = false;
  s.pending_play = false;
  s.current = -1;
  s.segments.clear();
  if (s.graph)
    gst_element_set_state(s.graph, GST_STATE_PAUSED);
}

HighlightsArchivePlayer::Status HighlightsArchivePlayer::Poll() {
  auto& s = *impl_;
  Status status;
  if (!s.graph) {
    status.error = s.take_error();
    return status;
  }

  bool segment_done = false;
  bool ended = false;
  while (GstMessage* message = gst_bus_pop(s.bus)) {
    switch (GST_MESSAGE_TYPE(message)) {
      case GST_MESSAGE_ERROR: {
        GError* error = nullptr;
        gst_message_parse_error(message, &error, nullptr);
        s.set_error(error && error->message ? QString::fromUtf8(error->message) : "Archive preview failed.");
        g_clear_error(&error);
        s.playing = false;
        break;
      }
      case GST_MESSAGE_SEGMENT_DONE:
        segment_done = true;
        break;
      case GST_MESSAGE_EOS:
        ended = true;
        break;
      default:
        break;
    }
    gst_message_unref(message);
  }

  if (!s.prerolled && gst_element_get_state(s.graph, nullptr, nullptr, 0) == GST_STATE_CHANGE_SUCCESS) {
    s.prerolled = true;
    if (s.pending_play) {
      s.pending_play = false;
      if (!s.seek(0, true)) {
        s.playing = false;
        s.set_error("The archive decoder rejected the seek to the first highlight.");
      }
    }
  }

  if ((segment_done || ended) && s.playing) {
    const int next = s.current + 1;
    if (next < s.segments.size()) {
      // A non-flushing segment seek queues behind the segment that just
      // finished, so consecutive highlights play back to back. After EOS the
      // pipeline has to be flushed before it will run again.
      if (!s.seek(next, ended)) {
        s.playing = false;
        s.set_error("The archive decoder rejected the seek to the next highlight.");
      }
    } else if (s.loop && !s.segments.isEmpty()) {
      if (!s.seek(0, ended)) {
        s.playing = false;
        s.set_error("The archive decoder rejected the seek back to the first highlight.");
      }
    } else {
      s.playing = false;
      s.finished = true;
      gst_element_set_state(s.graph, GST_STATE_PAUSED);
    }
  }

  gint64 position = -1;
  if (gst_element_query_position(s.graph, GST_FORMAT_TIME, &position) && position >= 0)
    status.position_ms = position / GST_MSECOND;
  status.playing = s.playing;
  status.finished = s.finished;
  status.segment = s.playing ? s.current : -1;
  status.error = s.take_error();
  return status;
}

void HighlightsArchivePlayer::Impl::clear_graph() {
  if (graph) {
    gst_element_set_state(graph, GST_STATE_NULL);
    if (sink)
      hm::gpu_preview::quiesce(sink, 0);
    if (bus)
      gst_object_unref(bus);
    gst_object_unref(graph);
  }
  graph = decoder = video_convert = sink = audio_convert = nullptr;
  bus = nullptr;
  prerolled = false;
  playing = false;
  pending_play = false;
  finished = false;
  current = -1;
  video_linked = false;
  audio_linked = false;
}

void HighlightsArchivePlayer::Close() {
  impl_->clear_graph();
  impl_ = std::make_unique<Impl>();
}

} // namespace hm::ui
