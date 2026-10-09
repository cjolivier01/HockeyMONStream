#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <nvbufsurface.h>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "src/apps/hstream-ui/HighlightScene.h"
#include "src/libs/highlights/TextureBlend.h"

namespace hm::ui {
namespace {
constexpr int kRate = 48000, kAudioBytes = 4; // stereo signed 16-bit
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
void cuda_check(cudaError_t e) {
  if (e != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(e));
}
GstElement* element(GstElement* graph, const char* factory, const char* name = nullptr) {
  GstElement* e = gst_element_factory_make(factory, name);
  if (!e)
    throw std::runtime_error(std::string("Required GStreamer element unavailable: ") + factory);
  check(gst_bin_add(GST_BIN(graph), e), "Could not add reel element");
  return e;
}
GstCaps* video_caps(int w, int h, bool ten, int fps_n, int fps_d) {
  GstCaps* c = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      ten ? "RGB10A2_LE" : "RGBA",
      "width",
      G_TYPE_INT,
      w,
      "height",
      G_TYPE_INT,
      h,
      "framerate",
      GST_TYPE_FRACTION,
      fps_n,
      fps_d,
      nullptr);
  gst_caps_set_features(c, 0, gst_caps_features_new("memory:NVMM", nullptr));
  return c;
}
void set_caps(GstElement* e, GstCaps* c) {
  g_object_set(e, "caps", c, nullptr);
  gst_caps_unref(c);
}
GstClockTime source_time(GstSample* sample) {
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  const GstSegment* segment = gst_sample_get_segment(sample);
  if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer) || !segment || segment->format != GST_FORMAT_TIME)
    return GST_CLOCK_TIME_NONE;
  // MP4 edit lists can rebase decoded PTS while retaining the original archive
  // position in segment.time. Trims and audio placement use that stream time.
  return gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
}
QString bus_error(GstElement* graph) {
  GstBus* bus = gst_element_get_bus(graph);
  GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (!m)
    return {};
  GError* e = nullptr;
  gchar* debug = nullptr;
  gst_message_parse_error(m, &e, &debug);
  const QString result = QString::fromUtf8(e ? e->message : "Native reel pipeline failed") + " (" +
      QString::fromUtf8(GST_OBJECT_NAME(m->src)) + ")";
  g_clear_error(&e);
  g_free(debug);
  gst_message_unref(m);
  return result;
}
struct Graph {
  GstElement* value{gst_pipeline_new(nullptr)};
  std::function<void()> before_destroy;
  ~Graph() {
    if (before_destroy)
      before_destroy();
    if (value) {
      gst_element_set_state(value, GST_STATE_NULL);
      gst_object_unref(value);
    }
  }
};
struct Surface {
  NvBufSurface* value{nullptr};
  Surface(int w, int h, bool ten) {
    NvBufSurfaceCreateParams p{};
    p.gpuId = 0;
    p.width = w;
    p.height = h;
    p.layout = NVBUF_LAYOUT_PITCH;
    p.colorFormat = ten ? NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709 : NVBUF_COLOR_FORMAT_RGBA;
    p.memType = NVBUF_MEM_CUDA_DEVICE;
    check(NvBufSurfaceCreate(&value, 1, &p) == 0, "Could not allocate reel GPU frame");
    value->numFilled = 1;
  }
  ~Surface() {
    if (value)
      NvBufSurfaceDestroy(value);
  }
};
struct Texture {
  void* value{nullptr};
  int width{0}, height{0};
  QPointF origin;
  QSize reference_size;
  explicit Texture(const HighlightTexture& tile)
      : width(tile.image.width()),
        height(tile.image.height()),
        origin(tile.origin),
        reference_size(tile.reference_size) {
    const QImage image = tile.image.convertToFormat(QImage::Format_RGBA8888);
    cuda_check(cudaMalloc(&value, size_t(width) * height * 4));
    const auto result = cudaMemcpy2D(
        value, width * 4, image.constBits(), image.bytesPerLine(), width * 4, height, cudaMemcpyHostToDevice);
    if (result != cudaSuccess) {
      cudaFree(value);
      value = nullptr;
      cuda_check(result);
    }
  }
  ~Texture() {
    if (value)
      cudaFree(value);
  }
};
struct Input {
  Graph graph;
  GstElement* decoder{nullptr};
  GstElement* video{nullptr};
  GstElement* audio{nullptr};
  GstElement* hardware{nullptr};
  GstElement* video_queue{nullptr};
  GstElement* audio_queue{nullptr};
  std::atomic<bool> video_linked{false}, audio_linked{false};
  static void pad(GstElement*, GstPad* pad, gpointer data) {
    auto* s = static_cast<Input*>(data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps)
      caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
      if (caps)
        gst_caps_unref(caps);
      return;
    }
    const char* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    GstElement* target = nullptr;
    std::atomic<bool>* flag = nullptr;
    if (g_str_has_prefix(name, "video/x-raw")) {
      target = s->video_queue;
      flag = &s->video_linked;
    } else if (g_str_has_prefix(name, "audio/x-raw")) {
      target = s->audio_queue;
      flag = &s->audio_linked;
    }
    gst_caps_unref(caps);
    if (!target || flag->exchange(true))
      return;
    GstPad* sink = gst_element_get_static_pad(target, "sink");
    if (gst_pad_link(pad, sink) != GST_PAD_LINK_OK)
      flag->store(false);
    gst_object_unref(sink);
  }
  static void added(GstBin*, GstBin*, GstElement* e, gpointer data) {
    auto* self = static_cast<Input*>(data);
    auto* f = gst_element_get_factory(e);
    if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)), "nvv4l2decoder") == 0) {
      self->hardware = e;
      g_object_set(e, "num-extra-surfaces", 4u, nullptr);
    }
  }
  static gint select(GstElement*, GstPad*, GstCaps*, GstElementFactory* f, gpointer) {
    const char* klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
    const char* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));
    return klass && g_strrstr(klass, "Decoder/Video") && g_strcmp0(name, "nvv4l2decoder") ? 2 : 0;
  }
  Input(const QString& path, int w, int h, bool ten) {
    decoder = element(graph.value, "uridecodebin");
    video_queue = element(graph.value, "queue");
    GstElement *convert = element(graph.value, "nvvideoconvert"), *caps = element(graph.value, "capsfilter");
    video = element(graph.value, "appsink");
    // Decoder rate is retained; the output timeline is explicitly timestamped.
    GstCaps* vc = video_caps(w, h, ten, 30, 1);
    gst_structure_remove_field(gst_caps_get_structure(vc, 0), "framerate");
    set_caps(caps, vc);
    g_object_set(convert, "output-buffers", 4, "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, nullptr);
    g_object_set(video_queue, "max-size-buffers", 4, "max-size-bytes", 0, "max-size-time", guint64(0), nullptr);
    g_object_set(
        video, "sync", FALSE, "async", TRUE, "max-buffers", 4, "drop", FALSE, "enable-last-sample", FALSE, nullptr);
    check(gst_element_link_many(video_queue, convert, caps, video, nullptr), "Could not link reel video decode");
    audio_queue = element(graph.value, "queue");
    GstElement *ac = element(graph.value, "audioconvert"), *ar = element(graph.value, "audioresample"),
               *af = element(graph.value, "capsfilter");
    audio = element(graph.value, "appsink");
    set_caps(
        af,
        gst_caps_new_simple(
            "audio/x-raw",
            "format",
            G_TYPE_STRING,
            "S16LE",
            "rate",
            G_TYPE_INT,
            kRate,
            "channels",
            G_TYPE_INT,
            2,
            "layout",
            G_TYPE_STRING,
            "interleaved",
            nullptr));
    g_object_set(audio_queue, "max-size-buffers", 8, "max-size-bytes", 0, "max-size-time", guint64(0), nullptr);
    g_object_set(
        audio, "sync", FALSE, "async", FALSE, "max-buffers", 8, "drop", FALSE, "enable-last-sample", FALSE, nullptr);
    check(gst_element_link_many(audio_queue, ac, ar, af, audio, nullptr), "Could not link reel audio decode");
    gchar* uri = gst_filename_to_uri(QFile::encodeName(path).constData(), nullptr);
    check(uri, "Invalid archive filename");
    g_object_set(decoder, "uri", uri, nullptr);
    g_free(uri);
    g_signal_connect(decoder, "deep-element-added", G_CALLBACK(added), this);
    graph.before_destroy = [this] {
      // Release queued sink references before the NVIDIA converter's pool is
      // deactivated. Otherwise pool acquisition can hold its stream lock.
      gst_element_set_state(video, GST_STATE_NULL);
      gst_element_set_state(audio, GST_STATE_NULL);
    };
    g_signal_connect(decoder, "pad-added", G_CALLBACK(pad), this);
    g_signal_connect(decoder, "autoplug-select", G_CALLBACK(select), nullptr);
  }
};
} // namespace

struct HighlightReelPipeline::Impl {
  Request request;
  mutable std::mutex mutex;
  Status status;
  std::atomic<bool> cancel{false}, failed{false};
  std::thread worker;
  GstElement* output{nullptr}; // borrowed while worker graph lives; guarded by mutex
  GstElement *video_src{nullptr}, *audio_src{nullptr};
  int width{0}, height{0}, fps_n{30}, fps_d{1};
  bool ten{false};
  std::vector<std::shared_ptr<Surface>> pool;
  cudaStream_t stream{nullptr};
  ~Impl() {
    cancel = true;
    if (worker.joinable())
      worker.join();
  }
  void error(const QString& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (status.error.isEmpty())
      status.error = message;
    failed = true;
  }
  bool stopped() {
    return cancel || failed;
  }
  void check_output() {
    if (!output)
      return;
    const QString e = bus_error(output);
    if (!e.isEmpty())
      error(e);
  }
  bool push(GstElement* src, GstBuffer* buffer) {
    while (!stopped() && gst_app_src_get_current_level_buffers(GST_APP_SRC(src)) >= 3) {
      check_output();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stopped()) {
      gst_buffer_unref(buffer);
      return false;
    }
    if (gst_app_src_push_buffer(GST_APP_SRC(src), buffer) != GST_FLOW_OK) {
      error("Output pipeline rejected a reel buffer");
      return false;
    }
    return true;
  }
  std::shared_ptr<Surface> acquire() {
    while (!stopped()) {
      for (const auto& s : pool)
        if (s.use_count() == 1)
          return s;
      check_output();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return {};
  }
  bool frame(
      GstBuffer* source,
      const HighlightInterval& item,
      qint64 game_ms,
      GstClockTime pts,
      GstClockTime duration,
      const std::vector<std::unique_ptr<Texture>>& textures) {
    auto owned = acquire();
    if (!owned)
      return false;
    auto& out = owned->value->surfaceList[0];
    if (source) {
      GstMapInfo map{};
      check(gst_buffer_map(source, &map, GST_MAP_READ), "Could not inspect decoded NVMM descriptor");
      // Mapping is the NvBufSurface descriptor only; no video CPU mapping/readback.
      NvBufSurface* in = map.size >= sizeof(NvBufSurface) ? reinterpret_cast<NvBufSurface*>(map.data) : nullptr;
      const bool valid = in && in->surfaceList && in->numFilled == 1 && in->surfaceList[0].dataPtr &&
          in->surfaceList[0].width == unsigned(width) && in->surfaceList[0].height == unsigned(height) &&
          in->gpuId == 0 && in->surfaceList[0].pitch >= unsigned(width) * 4 &&
          in->surfaceList[0].layout == NVBUF_LAYOUT_PITCH &&
          in->surfaceList[0].colorFormat == (ten ? NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709 : NVBUF_COLOR_FORMAT_RGBA) &&
          (in->memType == NVBUF_MEM_CUDA_DEVICE || in->memType == NVBUF_MEM_CUDA_UNIFIED);
      cudaError_t copied = cudaErrorInvalidValue;
      if (valid)
        copied = cudaMemcpy2DAsync(
            out.dataPtr,
            out.pitch,
            in->surfaceList[0].dataPtr,
            in->surfaceList[0].pitch,
            width * 4,
            height,
            cudaMemcpyDeviceToDevice,
            stream);
      gst_buffer_unmap(source, &map);
      cuda_check(copied);
    } else
      cuda_check(hm::highlights::ClearFrame(out.dataPtr, out.pitch, width, height, ten, stream));
    if (item.is_card) {
      const auto& t = *textures.front();
      cuda_check(
          hm::highlights::BlendTexture(
              out.dataPtr, out.pitch, width, height, ten, t.value, t.width, t.height, 0, 0, width, height, stream));
    } else {
      for (int i = 0; i < item.annotations.size(); ++i) {
        const auto anchor = HighlightAnchor(item.annotations[i], game_ms);
        if (!anchor)
          continue;
        const auto& t = *textures[i];
        cuda_check(
            hm::highlights::BlendTexture(
                out.dataPtr,
                out.pitch,
                width,
                height,
                ten,
                t.value,
                t.width,
                t.height,
                std::lround((anchor->x() + t.origin.x()) * width),
                std::lround((anchor->y() + t.origin.y()) * height),
                std::max(1, int(std::lround(double(t.width) * width / t.reference_size.width()))),
                std::max(1, int(std::lround(double(t.height) * height / t.reference_size.height()))),
                stream));
      }
    }
    cuda_check(cudaStreamSynchronize(stream)); // complete writes before encoder's independent stream
    auto* hold = new std::shared_ptr<Surface>(owned);
    GstBuffer* buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY, owned->value, sizeof(NvBufSurface), 0, sizeof(NvBufSurface), hold, [](gpointer p) {
          delete static_cast<std::shared_ptr<Surface>*>(p);
        });
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = duration;
    const bool pushed = push(video_src, buffer);
    return pushed;
  }
  bool audio_buffer(const unsigned char* data, qint64 count, qint64 index) {
    GstBuffer* b = gst_buffer_new_allocate(nullptr, count * kAudioBytes, nullptr);
    if (data)
      gst_buffer_fill(b, 0, data, count * kAudioBytes);
    else
      gst_buffer_memset(b, 0, 0, count * kAudioBytes);
    GST_BUFFER_PTS(b) = gst_util_uint64_scale(index, GST_SECOND, kRate);
    GST_BUFFER_DURATION(b) = gst_util_uint64_scale(index + count, GST_SECOND, kRate) - GST_BUFFER_PTS(b);
    return push(audio_src, b);
  }
  void audio_item(Input* in, qint64 start_ms, qint64 length_ms, qint64 base_ms) {
    try {
      const qint64 start = start_ms * kRate / 1000, first = base_ms * kRate / 1000,
                   end = (base_ms + length_ms) * kRate / 1000;
      qint64 cursor = first;
      auto silence = [&](qint64 until) {
        while (cursor < until && !stopped()) {
          const qint64 n = std::min<qint64>(480, until - cursor);
          if (!audio_buffer(nullptr, n, cursor))
            break;
          cursor += n;
        }
      };
      if (in && in->audio_linked) {
        auto last = Clock::now();
        while (!stopped()) {
          GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(in->audio), 50 * GST_MSECOND);
          if (!sample) {
            if (gst_app_sink_is_eos(GST_APP_SINK(in->audio)))
              break;
            check_output();
            const QString e = bus_error(in->graph.value);
            if (!e.isEmpty()) {
              error(e);
              break;
            }
            if (Clock::now() - last > std::chrono::seconds(30)) {
              error("Archive audio decoder stalled");
              break;
            }
            continue;
          }
          last = Clock::now();
          GstBuffer* b = gst_sample_get_buffer(sample);
          const GstClockTime time = source_time(sample);
          if (!GST_CLOCK_TIME_IS_VALID(time)) {
            gst_sample_unref(sample);
            throw std::runtime_error("Decoded audio has no source timestamp");
          }
          const qint64 index = first + qint64(gst_util_uint64_scale_round(time, kRate, GST_SECOND)) - start;
          if (index >= end) {
            gst_sample_unref(sample);
            break;
          }
          GstMapInfo map{};
          if (!gst_buffer_map(b, &map, GST_MAP_READ)) {
            gst_sample_unref(sample);
            throw std::runtime_error("Invalid decoded audio");
          }
          const qint64 count = map.size / kAudioBytes;
          const qint64 from = std::max(cursor, index), to = std::min(end, index + count);
          if (from < to) {
            silence(std::min(end, from));
            if (!stopped() && audio_buffer(map.data + (from - index) * kAudioBytes, to - from, from))
              cursor = to;
          }
          gst_buffer_unmap(b, &map);
          gst_sample_unref(sample);
          if (cursor >= end)
            break;
          // The input remains open-ended; stop locally after this sample range.
          // A bounded queue holds the small lead over the video boundary.
        }
      }
      silence(end);
    } catch (const std::exception& e) {
      error(QString::fromUtf8(e.what()));
    }
  }
  bool wait_input(Input& in) {
    gst_element_set_state(in.graph.value, GST_STATE_PAUSED);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!stopped() && Clock::now() < deadline) {
      const auto state = gst_element_get_state(in.graph.value, nullptr, nullptr, 50 * GST_MSECOND);
      const QString e = bus_error(in.graph.value);
      if (!e.isEmpty()) {
        error(e);
        return false;
      }
      if (state == GST_STATE_CHANGE_SUCCESS && in.video_linked) {
        GstSample* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(in.video), 50 * GST_MSECOND);
        if (preroll) {
          gst_sample_unref(preroll);
          return true;
        }
      }
      if (state == GST_STATE_CHANGE_FAILURE)
        return false;
    }
    return false;
  }
  void output_graph(Graph& graph) {
    video_src = element(graph.value, "appsrc", "reel-video");
    audio_src = element(graph.value, "appsrc", "reel-audio");
    for (auto* src : {video_src, audio_src})
      g_object_set(
          src,
          "format",
          GST_FORMAT_TIME,
          "is-live",
          FALSE,
          "block",
          FALSE,
          "max-bytes",
          guint64(0),
          "max-buffers",
          guint64(3),
          nullptr);
    set_caps(video_src, video_caps(width, height, ten, fps_n, fps_d));
    set_caps(
        audio_src,
        gst_caps_new_simple(
            "audio/x-raw",
            "format",
            G_TYPE_STRING,
            "S16LE",
            "rate",
            G_TYPE_INT,
            kRate,
            "channels",
            G_TYPE_INT,
            2,
            "layout",
            G_TYPE_STRING,
            "interleaved",
            nullptr));
    GstElement *vq = element(graph.value, "queue"), *aq = element(graph.value, "queue");
    for (auto* q : {vq, aq})
      g_object_set(q, "max-size-buffers", 3, "max-size-bytes", 0, "max-size-time", guint64(0), nullptr);
    check(gst_element_link(video_src, vq) && gst_element_link(audio_src, aq), "Could not link reel sources");
    if (request.window_id) {
      GstElement* sink = element(graph.value, "hmgpupreviewsink", "reel-preview");
      g_object_set(
          sink,
          "window-id",
          guint64(request.window_id),
          "channel",
          "highlights",
          "sync",
          TRUE,
          "qos",
          FALSE,
          "enable-last-sample",
          FALSE,
          nullptr);
      hm::gpu_preview::set_source_geometry(sink, width, height);
      check(gst_element_link(vq, sink), "Could not link reel GPU preview");
      GstElement *ac = element(graph.value, "audioconvert"), *as = element(graph.value, "autoaudiosink");
      check(gst_element_link_many(aq, ac, as, nullptr), "Could not link reel audio preview");
    } else {
      GstElement *convert = element(graph.value, "nvvideoconvert"), *filter = element(graph.value, "capsfilter");
      GstCaps* caps = video_caps(width, height, false, fps_n, fps_d);
      gst_structure_set(gst_caps_get_structure(caps, 0), "format", G_TYPE_STRING, ten ? "P010_10LE" : "NV12", nullptr);
      set_caps(filter, caps);
      const bool hevc = ten || request.media.video_codec == "hevc";
      GstElement* enc = element(graph.value, hevc ? "nvv4l2h265enc" : "nvv4l2h264enc");
      const qint64 rate = request.media.video_bit_rate > 0 ? request.media.video_bit_rate
                                                           : qint64(width) * height * double(fps_n) / fps_d * 0.14;
      g_object_set(
          enc,
          "bitrate",
          guint(std::clamp<qint64>(rate, 2000000, 240000000)),
          "iframeinterval",
          guint(std::max(1, fps_n / fps_d) * 2),
          nullptr);
      GstElement *parser = element(graph.value, hevc ? "h265parse" : "h264parse"),
                 *mux = element(graph.value, "mp4mux"), *file = element(graph.value, "filesink");
      g_object_set(mux, "faststart", TRUE, nullptr);
      g_object_set(file, "location", QFile::encodeName(request.output_path).constData(), nullptr);
      check(
          gst_element_link_many(vq, convert, filter, enc, parser, mux, file, nullptr),
          "Could not link native reel encoder/mux");
      GstElement *ac = element(graph.value, "audioconvert"), *ae = element(graph.value, "voaacenc"),
                 *ap = element(graph.value, "aacparse");
      g_object_set(ae, "bitrate", 192000, nullptr);
      check(gst_element_link_many(aq, ac, ae, ap, mux, nullptr), "Could not link native reel audio encoder");
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      output = graph.value;
    }
    check(
        gst_element_set_state(graph.value, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
        "Could not start reel output");
  }
  void item(const HighlightInterval& item, qint64 base_ms) {
    std::vector<std::unique_ptr<Texture>> textures;
    size_t bytes = 0;
    auto upload = [&](HighlightTexture t) {
      bytes += size_t(t.image.width()) * t.image.height() * 4;
      check(bytes <= 128 * 1024 * 1024, "Authored artwork exceeds the 128 MiB per-item limit");
      textures.push_back(std::make_unique<Texture>(t));
    };
    if (item.is_card) {
      QImage image;
      QString e;
      if (!RasterHighlightCard(item.card, request.asset_root, &image, &e, QSize(width, height)))
        throw std::runtime_error(e.toStdString());
      upload({image, {}});
    } else
      for (const auto& a : item.annotations)
        upload(RasterHighlightAnnotation(a, QSize(width, height)));
    qint64 length = HighlightItemDuration(item);
    const GstClockTime step = gst_util_uint64_scale(GST_SECOND, fps_d, fps_n), base = base_ms * GST_MSECOND;
    if (item.is_card) {
      std::thread audio([&] { audio_item(nullptr, 0, length, base_ms); });
      try {
        for (GstClockTime t = 0; t < GstClockTime(length) * GST_MSECOND && !stopped(); t += step)
          if (!frame(nullptr, item, 0, base + t, std::min(step, GstClockTime(length) * GST_MSECOND - t), textures))
            break;
      } catch (...) {
        failed = true;
        audio.join();
        throw;
      }
      audio.join();
      return;
    }
    Input in(request.archive_path, width, height, ten);
    check(wait_input(in), "Archive decoder did not prepare its video stream");
    qint64 begin = item.start_ms - request.archive_offset_ms, end = item.end_ms - request.archive_offset_ms;
    if (request.inspect_game_ms >= 0) {
      begin = request.inspect_game_ms - request.archive_offset_ms;
      end = begin + std::max<qint64>(100, step / GST_MSECOND * 3);
      length = end - begin;
    }
    auto seek = [&] {
      return gst_element_seek(
          in.decoder,
          1.0,
          GST_FORMAT_TIME,
          GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET,
          begin * GST_MSECOND,
          GST_SEEK_TYPE_NONE,
          GST_CLOCK_TIME_NONE);
    };
    check(seek(), "Archive rejected the highlight seek");
    // NVIDIA can retain a partially initialized output surface after a seek
    // from preroll. Use the same decoder restart as the recorded-source path.
    check(in.hardware, "Archive did not select the NVIDIA decoder");
    check(
        gst_element_set_state(in.hardware, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE &&
            gst_element_sync_state_with_parent(in.hardware),
        "Could not restart archive decoder after preroll seek");
    check(seek(), "Archive rejected its post-restart highlight seek");
    gst_element_set_state(in.graph.value, GST_STATE_PLAYING);
    std::thread audio([&] { audio_item(request.inspect_game_ms >= 0 ? nullptr : &in, begin, length, base_ms); });
    auto last = Clock::now();
    qint64 frames = 0;
    GstClockTime completed_source = 0;
    try {
      while (!stopped()) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(in.video), 50 * GST_MSECOND);
        if (!sample) {
          if (gst_app_sink_is_eos(GST_APP_SINK(in.video)))
            break;
          check_output();
          const QString e = bus_error(in.graph.value);
          if (!e.isEmpty()) {
            error(e);
            break;
          }
          check(Clock::now() - last < std::chrono::seconds(30), "Archive video decoder stalled");
          continue;
        }
        last = Clock::now();
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        const GstClockTime source_pts = source_time(sample);
        if (!GST_CLOCK_TIME_IS_VALID(source_pts)) {
          gst_sample_unref(sample);
          throw std::runtime_error("Archive video has no timestamp");
        }
        if (source_pts >= GstClockTime(end) * GST_MSECOND) {
          gst_sample_unref(sample);
          break;
        }
        const GstClockTime source_duration = GST_BUFFER_DURATION_IS_VALID(buffer) ? GST_BUFFER_DURATION(buffer) : step;
        const GstClockTime trim_start = std::max(source_pts, GstClockTime(begin) * GST_MSECOND);
        const GstClockTime trim_end = std::min(source_pts + source_duration, GstClockTime(end) * GST_MSECOND);
        // Rational frame timestamps can leave a one-nanosecond overlap at an
        // exact cut. The hardware encoder uses microsecond timestamps; discard
        // these rounding tails rather than submitting two frames at one PTS.
        if (trim_end <= trim_start + GST_USECOND) {
          gst_sample_unref(sample);
          continue;
        }
        const GstClockTime local = trim_start - begin * GST_MSECOND;
        const GstClockTime duration = trim_end - trim_start;
        const qint64 game_ms = request.archive_offset_ms + trim_start / GST_MSECOND;
        try {
          frame(buffer, item, game_ms, base + local, duration, textures);
        } catch (...) {
          cudaStreamSynchronize(stream);
          gst_sample_unref(sample);
          throw;
        }
        gst_sample_unref(sample);
        ++frames;
        completed_source = source_pts + source_duration;
        if (request.inspect_game_ms >= 0)
          break;
      }
      check(frames > 0 || stopped(), "Archive ended before producing this clip");
      check(
          stopped() || request.inspect_game_ms >= 0 ||
              completed_source + GST_MSECOND >= GstClockTime(end) * GST_MSECOND,
          "Archive ended before the requested clip boundary");
    } catch (const std::exception& e) {
      error(QString::fromUtf8(e.what()));
      audio.join();
      throw;
    }

    audio.join();
  }
  void run() {
    try {
      cuda_check(cudaSetDevice(0));
      cuda_check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
      const double scale =
          request.window_id ? std::min({1.0, 1280.0 / request.media.width, 720.0 / request.media.height}) : 1.0;
      width = std::max(2, int(request.media.width * scale) & ~1);
      height = std::max(2, int(request.media.height * scale) & ~1);
      ten = !request.window_id && request.media.bit_depth > 8;
      gst_util_double_to_fraction(request.media.frame_rate > 0 ? request.media.frame_rate : 30, &fps_n, &fps_d);
      for (int i = 0; i < 6; ++i)
        pool.push_back(std::make_shared<Surface>(width, height, ten));
      Graph graph;
      graph.before_destroy = [this] {
        std::lock_guard<std::mutex> lock(mutex);
        output = nullptr;
      };
      output_graph(graph);
      qint64 base_ms = 0;
      do {
        for (int i = 0; i < request.items.size() && !stopped(); ++i) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            status.item = i;
          }
          item(request.items[i], base_ms);
          base_ms += HighlightItemDuration(request.items[i]);
        }
      } while (request.loop && !stopped());
      if (!stopped()) {
        gst_app_src_end_of_stream(GST_APP_SRC(video_src));
        gst_app_src_end_of_stream(GST_APP_SRC(audio_src));
        GstBus* bus = gst_element_get_bus(graph.value);
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        bool ended = false;
        while (!stopped() && Clock::now() < deadline) {
          GstMessage* m =
              gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
          if (!m)
            continue;
          if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
            GError* e = nullptr;
            gst_message_parse_error(m, &e, nullptr);
            error(QString::fromUtf8(e ? e->message : "Reel finalization failed"));
            g_clear_error(&e);
          } else
            ended = true;
          gst_message_unref(m);
          break;
        }
        gst_object_unref(bus);
        check(ended || stopped(), "Reel encoder/mux did not finalize within 60 seconds");
      }
      // Inspection keeps the GPU sink and displayed frame alive for authoring.
      if (request.inspect_game_ms >= 0 && !stopped()) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          status.finished = true;
        }
        while (!cancel)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        output = nullptr;
      }
    } catch (const std::exception& e) {
      error(QString::fromUtf8(e.what()));
      std::lock_guard<std::mutex> lock(mutex);
      output = nullptr;
    }
    pool.clear();
    if (stream) {
      cudaStreamSynchronize(stream);
      cudaStreamDestroy(stream);
      stream = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    status.finished = true;
    status.cancelled = cancel;
  }
};
HighlightReelPipeline::HighlightReelPipeline() : impl_(std::make_unique<Impl>()) {}
HighlightReelPipeline::~HighlightReelPipeline() = default;
bool HighlightReelPipeline::PreviewAvailable() {
  gst_init(nullptr, nullptr);
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  return hm::gpu_preview::renderer_available() && hm::gpu_preview::register_elements();
}
bool HighlightReelPipeline::Start(Request request, QString* error) {
  auto fail = [&](const QString& e) {
    if (error)
      *error = e;
    return false;
  };
  if (request.items.isEmpty())
    return fail("No reel items selected");
  if (request.media.width <= 0 || request.media.height <= 0 || request.media.width > 8192 ||
      request.media.height > 8192)
    return fail("Reel output needs even video dimensions up to 8192 pixels per side");
  if ((!request.window_id && request.output_path.isEmpty()) || (request.window_id && !PreviewAvailable()))
    return fail("Reel needs an output file or an available NVIDIA/X11 preview");
  qint64 total = 0;
  for (auto& item : request.items) {
    QString e;
    if (!NormalizeHighlightInterval(&item, &e))
      return fail(e);
    if (!item.is_card) {
      if (!QFileInfo(request.archive_path).isFile() || item.start_ms < request.archive_offset_ms)
        return fail("Clip is outside its selected archive");
      for (const auto& a : item.annotations) {
        const QFileInfo info(request.archive_path);
        if (a.motion == "track" &&
            (QFileInfo(a.archive_path).absoluteFilePath() != info.absoluteFilePath() ||
             a.archive_origin_ms != request.archive_offset_ms || a.archive_size != info.size() ||
             a.archive_mtime_ms != info.lastModified().toMSecsSinceEpoch()))
          return fail(
              "Tracked cue belongs to a different or changed archive/time binding. Select its recorded track again.");
      }
    }
    if (HighlightItemDuration(item) > 24 * 3600000 || total > 24 * 3600000 - HighlightItemDuration(item))
      return fail("A reel must fit within 24 hours");
    total += HighlightItemDuration(item);
  }
  impl_.reset();
  impl_ = std::make_unique<Impl>();
  impl_->request = std::move(request);
  impl_->status.total_ms = total;
  gst_init(nullptr, nullptr);
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  impl_->worker = std::thread([s = impl_.get()] { s->run(); });
  return true;
}
void HighlightReelPipeline::Cancel() {
  impl_->cancel = true;
}
HighlightReelPipeline::Status HighlightReelPipeline::Poll() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto result = impl_->status;
  if (impl_->output) {
    gint64 time = 0;
    if (gst_element_query_position(impl_->output, GST_FORMAT_TIME, &time))
      result.position_ms = time / GST_MSECOND;
  }
  return result;
}
} // namespace hm::ui
