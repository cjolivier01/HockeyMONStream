#include "src/apps/hstream-ui/SourceCameraPlayer.h"

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>
}

#include <QtCore/QElapsedTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "hstream/src/libs/media/Mp4VideoTrack.h"
#ifndef HSTREAM_USE_HIP
#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#endif

namespace hm::ui {
namespace {
struct CameraTrack {
  media::VideoCodec codec;
  int width, height;
  int64_t duration_ns;
  const char* demux;
};

absl::StatusOr<CameraTrack> ReadCameraTrack(const QString& file, qint64 timeout_ms) {
  if (!file.endsWith(".mkv", Qt::CaseInsensitive) && !file.endsWith(".avi", Qt::CaseInsensitive)) {
    auto track = media::ReadMp4VideoTrack(file.toStdString(), timeout_ms * GST_MSECOND, {});
    if (!track.ok())
      return track.status();
    return CameraTrack{track->codec, track->width, track->height, track->duration_ns, "qtdemux"};
  }
  // These containers do not share MP4's strict header timing contract. Inspect
  // headers and, if necessary, compressed timestamps only; never open a codec.
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  const auto timeout = std::chrono::milliseconds(timeout_ms);
  auto deadline = timeout > Clock::time_point::max() - now ? Clock::time_point::max() : now + timeout;
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR || (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
    return absl::FailedPreconditionError("Camera metadata requires matching FFmpeg headers and libraries");
  AVFormatContext* context = avformat_alloc_context();
  if (!context)
    return absl::ResourceExhaustedError("Cannot allocate camera metadata demuxer");
  context->interrupt_callback = {
      [](void* value) { return int(Clock::now() >= *static_cast<const Clock::time_point*>(value)); }, &deadline};
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  av_dict_set(&options, "format_whitelist", "matroska,webm,avi", 0);
  const int opened = avformat_open_input(&context, file.toLocal8Bit().constData(), nullptr, &options);
  av_dict_free(&options);
  const auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
  std::unique_ptr<AVFormatContext, decltype(close)> owner(context, close);
  if (opened < 0 || Clock::now() >= deadline)
    return absl::DataLossError("Cannot read camera container metadata within its deadline");
  AVStream* video = nullptr;
  for (unsigned index = 0; index < context->nb_streams; ++index) {
    auto* stream = context->streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    if (video || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
      return absl::InvalidArgumentError("Camera preview requires one unambiguous video track");
    video = stream;
  }
  if (!video || video->codecpar->width <= 0 || video->codecpar->height <= 0 || video->time_base.num <= 0 ||
      video->time_base.den <= 0)
    return absl::DataLossError("Camera recording has no usable video track");
  const auto* codec = video->codecpar;
  if (codec->codec_id != AV_CODEC_ID_H264 && codec->codec_id != AV_CODEC_ID_HEVC)
    return absl::UnimplementedError("Camera hardware preview requires H.264 or HEVC video");
  const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
  std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
  if (!packet)
    return absl::ResourceExhaustedError("Cannot allocate camera metadata packet");
  for (unsigned index = 0; index < context->nb_streams; ++index)
    if (context->streams[index] != video)
      context->streams[index]->discard = AVDISCARD_ALL;
  const double unit = av_q2d(video->time_base);
  double origin = video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time * unit;
  if (video->start_time == AV_NOPTS_VALUE) {
    bool found = false;
    for (size_t count = 0; count < 100000 && Clock::now() < deadline; ++count) {
      if (av_read_frame(context, packet.get()) < 0)
        break;
      if (packet->stream_index == video->index && packet->pts != AV_NOPTS_VALUE) {
        origin = packet->pts * unit;
        found = true;
      }
      av_packet_unref(packet.get());
      if (found)
        break;
    }
    if (!found)
      return absl::DataLossError("Camera video has no bounded presentation origin");
  }
  double seconds = video->duration > 0 ? video->duration * unit : 0;
  // Matroska DURATION is a track end timestamp, not a length. In particular,
  // container duration includes audio tails and cannot define this timeline.
  if (seconds <= 0)
    if (const auto* tag = av_dict_get(video->metadata, "DURATION", nullptr, 0)) {
      int64_t end = 0;
      if (av_parse_time(&end, tag->value, 1) == 0)
        seconds = end / double(AV_TIME_BASE) - origin;
    }
  const double fps = av_q2d(video->avg_frame_rate);
  if (seconds <= 0 && video->nb_frames > 0 && std::isfinite(fps) && fps > 0)
    seconds = video->nb_frames / fps;
  if (seconds <= 0 && context->duration > 0) {
    // Container duration is only a seek hint. Confirm the actual video end
    // from bounded compressed packets, including containers without track tags.
    const auto hint = av_rescale_q(context->duration, AVRational{1, AV_TIME_BASE}, video->time_base);
    if (av_seek_frame(context, video->index, hint, AVSEEK_FLAG_BACKWARD) >= 0) {
      double end = origin;
      for (size_t count = 0; count < 100000 && Clock::now() < deadline; ++count) {
        const int result = av_read_frame(context, packet.get());
        if (result == AVERROR_EOF) {
          seconds = end - origin;
          break;
        }
        if (result < 0)
          break;
        if (packet->stream_index == video->index && packet->pts != AV_NOPTS_VALUE) {
          const double length = packet->duration > 0 ? packet->duration * unit
              : std::isfinite(fps) && fps > 0        ? 1.0 / fps
                                                     : 0;
          if (length > 0)
            end = std::max(end, packet->pts * unit + length);
        }
        av_packet_unref(packet.get());
      }
    }
  }
  if (Clock::now() >= deadline || !std::isfinite(seconds) || seconds <= 0 ||
      seconds * 1e9 >= std::numeric_limits<int64_t>::max())
    return absl::DataLossError("Camera video duration is missing, invalid or exceeded its inspection deadline");
  return CameraTrack{
      codec->codec_id == AV_CODEC_ID_H264 ? media::VideoCodec::kH264 : media::VideoCodec::kHevc,
      codec->width,
      codec->height,
      static_cast<int64_t>(std::llround(seconds * 1e9)),
      g_strcmp0(context->iformat->name, "avi") == 0 ? "avidemux" : "matroskademux"};
}
} // namespace

struct SourceCameraPlayer::Impl {
  struct Chapter {
    QString file;
    CameraTrack track;
    qint64 start_ms;
  };
  std::vector<Chapter> chapters;
  GstElement* graph{nullptr};
  GstElement* parser{nullptr};
  GstElement* video_input{nullptr};
  GstElement* sink{nullptr};
  GstBus* bus{nullptr};
  Status status;
  size_t chapter{0};
  quint64 window{0};
  std::uint64_t generation{0};
  qint64 timeout_ms{0};
  std::optional<qint64> seek_ms;
  bool preroll{false};
  bool seeking{false};
  QElapsedTimer transition;

  void retire() {
    if (graph)
      gst_element_set_state(graph, GST_STATE_NULL);
#ifndef HSTREAM_USE_HIP
    if (sink)
      hm::gpu_preview::quiesce(sink, 0);
#endif
    if (bus)
      gst_object_unref(bus);
    if (graph)
      gst_object_unref(graph);
    graph = parser = video_input = sink = nullptr;
    bus = nullptr;
    preroll = seeking = false;
    status.open = status.ready = false;
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
          "queue name=camera_audio_input ! decodebin ! audioconvert ! "
          "audioresample ! autoaudiosink sync=true",
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
    auto* file = gst_element_factory_make("filesrc", nullptr);
    auto* demux = gst_element_factory_make(item.track.demux, nullptr);
    const bool hevc = item.track.codec == media::VideoCodec::kHevc;
    parser = gst_element_factory_make(hevc ? "h265parse" : "h264parse", nullptr);
    video_input = gst_element_factory_make("queue", nullptr);
#ifdef HSTREAM_USE_HIP
    auto* decoder = gst_element_factory_make(hevc ? "vah265dec" : "vah264dec", nullptr);
    // VA exports DMABufs directly to EGL/GL. Never negotiate system-memory video.
    auto* convert = gst_element_factory_make("identity", nullptr);
    sink = gst_element_factory_make("glimagesink", nullptr);
    auto* caps = gst_caps_from_string("video/x-raw(memory:DMABuf)");
#else
    auto* decoder = gst_element_factory_make("nvv4l2decoder", nullptr);
    auto* convert = gst_element_factory_make("nvvideoconvert", nullptr);
    sink = gst_element_factory_make("hmgpupreviewsink", nullptr);
    auto* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
#endif
    auto* filter = gst_element_factory_make("capsfilter", nullptr);
    const std::vector<GstElement*> elements{file, demux, video_input, parser, decoder, convert, filter, sink};
    if (!graph || std::any_of(elements.begin(), elements.end(), [](auto* e) { return !e; })) {
      for (auto* element : elements)
        if (element)
          gst_object_unref(element);
      parser = video_input = sink = nullptr;
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
    g_object_set(sink, "sync", TRUE, "async", TRUE, "qos", TRUE, nullptr);
#ifdef HSTREAM_USE_HIP
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), window);
#else
    g_object_set(sink, "window-id", window, "channel", "source-camera", "enable-last-sample", TRUE, nullptr);
    hm::gpu_preview::set_source_geometry(sink, item.track.width, item.track.height);
#endif
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

bool SourceCameraPlayer::Open(const QStringList& files, quint64 window, qint64 timeout_ms, QString* error) {
  Close();
  auto& s = *impl_;
  const auto fail = [&](const QString& message) {
    s.fail(message);
    if (error)
      *error = message;
    return false;
  };
  if (files.isEmpty() || !window || timeout_ms <= 0 || timeout_ms > std::numeric_limits<qint64>::max() / GST_MSECOND)
    return fail("Camera preview needs original recordings, a native window and a positive timeout.");
  gst_init(nullptr, nullptr);
#ifndef HSTREAM_USE_HIP
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  if (!hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements())
    return fail("Camera preview requires a GPU renderer and an X11 display.");
#endif
  s.window = window;
  s.timeout_ms = timeout_ms;
  QElapsedTimer probe;
  probe.start();
  for (const auto& file : files) {
    const auto remaining = timeout_ms - probe.elapsed();
    if (remaining <= 0)
      return fail("Camera recording metadata timed out.");
    auto track = ReadCameraTrack(file, remaining);
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
  if (s.status.position_ms >= s.status.duration_ms)
    Seek(0);
  if (s.status.ready && !s.seek_ms && !s.seeking &&
      gst_element_set_state(s.graph, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    s.fail("Could not play the camera recording.");
}
void SourceCameraPlayer::Pause() {
  auto& s = *impl_;
  s.status.playing = false;
  if (s.graph && gst_element_set_state(s.graph, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    s.fail("Could not pause the camera recording.");
}
bool SourceCameraPlayer::Redraw() {
  auto& s = *impl_;
  if (!s.sink || !s.status.ready || s.status.playing || s.seeking || s.seek_ms)
    return false;
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
  s.status.position_ms = target;
  if (index != s.chapter)
    s.openChapter(index);
}
void SourceCameraPlayer::Close() {
  impl_->retire();
  impl_->chapters.clear();
  impl_->status = {};
  impl_->seek_ms.reset();
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
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ASYNC_DONE) {
      s.seeking = false;
    }
    gst_message_unref(message);
  }
  if (!error.isEmpty()) {
    s.fail(error);
    return s.status;
  }
  if (s.preroll && gst_element_get_state(s.graph, nullptr, nullptr, 0) == GST_STATE_CHANGE_SUCCESS) {
    s.preroll = false;
    s.status.ready = true;
  }
  if ((s.preroll || s.seeking) && s.transition.elapsed() > s.timeout_ms) {
    s.fail("Camera preview timed out while opening or seeking.");
    return s.status;
  }
  if (s.status.ready && !s.seeking && s.seek_ms) {
    const auto target = *s.seek_ms;
    s.seek_ms.reset();
    const auto& item = s.chapters[s.chapter];
#ifndef HSTREAM_USE_HIP
    hm::gpu_preview::set_renderer_generation(s.sink, ++s.generation);
#endif
    s.status.presented_ns = -1;
    if (!gst_element_seek(
            s.graph,
            1.0,
            GST_FORMAT_TIME,
            GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET,
            (target - item.start_ms) * GST_MSECOND,
            GST_SEEK_TYPE_SET,
            item.track.duration_ns)) {
      s.fail("Camera recording rejected the seek.");
      return s.status;
    }
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
    if (s.status.playing && gst_element_set_state(s.graph, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      s.fail("Could not resume the camera recording.");
    gint64 position = 0;
    if (s.graph && s.status.position_ms < s.status.duration_ms &&
        gst_element_query_position(s.graph, GST_FORMAT_TIME, &position) && position >= 0)
      s.status.position_ms =
          std::min(s.status.duration_ms, s.chapters[s.chapter].start_ms + position / qint64(GST_MSECOND));
  }
  if (s.sink && s.status.ready && !s.seeking && !s.seek_ms) {
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
  return s.status;
}
} // namespace hm::ui
