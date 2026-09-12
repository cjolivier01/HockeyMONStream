#include "src/apps/hstream-ui/CameraExperimentPreview.h"
#include "src/apps/hstream-ui/CameraExperimentSource.h"

#include "hstream/src/apps/apps-common/HmGpuPreview.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtGui/QImage>

#include <gst/gst.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <nvdsmeta.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <utility>

namespace {

bool fail(std::string* error, const std::string& message) {
  if (error)
    *error = message;
  return false;
}

GstCaps* rgba_caps() {
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  return caps;
}

void load_cropper_plugin() {
  GstElementFactory* existing = gst_element_factory_find("playcropper");
  if (existing) {
    gst_object_unref(existing);
    return;
  }
  const QDir app(QCoreApplication::applicationDirPath());
  const QStringList candidates{
      app.filePath("../../gst-plugins/gst-videoprep/libnvdsgst_videoprep.so"),
      app.filePath("../lib/gst-plugins/libnvdsgst_videoprep.so"),
      app.filePath("lib/gst-plugins/libnvdsgst_videoprep.so"),
      "/opt/hstream/lib/gst-plugins/libnvdsgst_videoprep.so"};
  for (const QString& path : candidates) {
    GstPlugin* plugin = gst_plugin_load_file(path.toLocal8Bit().constData(), nullptr);
    if (plugin) {
      gst_object_unref(plugin);
      return;
    }
  }
}

} // namespace

struct CameraExperimentPreview::Impl {
  GstElement* graph{nullptr};
  GstElement* decoder{nullptr};
  GstElement* converter{nullptr};
  GstElement* cropper{nullptr};
  GstElement* sink{nullptr};
  GstBus* bus{nullptr};
  std::unique_ptr<CameraExperimentSource> raw;
  std::future<std::string> rebuilding;
  std::uint64_t raw_start{0};
  std::uint64_t window_id{0};
  double left_rotation{0}, right_rotation{0};
  bool build_graph(std::string* error);
  void clear_graph();
  std::mutex mutex;
  std::condition_variable range_boundary;
  bool closing{false};
  hm::playtracker_replay::MediaBinding media;
  unsigned canvas_width{0};
  unsigned canvas_height{0};
  std::shared_ptr<const std::vector<Frame>> frames;
  std::uint64_t end_pts_ns{0};
  std::uint64_t generation{0};
  std::uint64_t segment_generation{0};
  bool playing{false};
  bool seeking{false};
  bool primed{false};
  bool initial_seek_pending{false};
  std::optional<std::pair<std::size_t, bool>> pending_seek;
  bool loop{true};
  std::size_t target{0};
  std::optional<std::size_t> presented;
  std::string error;
  std::chrono::steady_clock::time_point seek_started;

  std::optional<std::uint64_t> video_pts(std::uint64_t telemetry_pts) const {
    if (telemetry_pts >= media.telemetry_origin_pts_ns) {
      const std::uint64_t delta = telemetry_pts - media.telemetry_origin_pts_ns;
      if (delta > static_cast<std::uint64_t>(G_MAXINT64) - media.video_origin_pts_ns)
        return std::nullopt;
      return media.video_origin_pts_ns + delta;
    }
    const std::uint64_t delta = media.telemetry_origin_pts_ns - telemetry_pts;
    if (delta > media.video_origin_pts_ns)
      return std::nullopt;
    return media.video_origin_pts_ns - delta;
  }

  std::optional<std::size_t> find_frame(std::uint64_t video_time) const {
    if (!frames || frames->empty())
      return std::nullopt;
    constexpr std::uint64_t kPtsTolerance = GST_MSECOND;
    const auto first = video_pts(frames->front().pts_ns);
    const auto last = video_pts(end_pts_ns);
    if (!first || !last || video_time >= *last || (video_time < *first && *first - video_time > kPtsTolerance))
      return std::nullopt;
    auto found = std::lower_bound(frames->begin(), frames->end(), video_time, [this](const Frame& frame, auto pts) {
      return video_pts(frame.pts_ns).value_or(G_MAXUINT64) < pts;
    });
    // Encoder/mux timebases commonly round a 59.94 fps PTS to microseconds.
    // A 1 ms window accommodates that rounding, but never pairs neighboring
    // frames or silently holds a camera box over missing observations.
    if (found != frames->end()) {
      const auto pts = video_pts(found->pts_ns);
      if (pts && *pts >= video_time && *pts - video_time <= kPtsTolerance)
        return static_cast<std::size_t>(found - frames->begin());
    }
    if (found != frames->begin()) {
      --found;
      const auto pts = video_pts(found->pts_ns);
      if (pts && video_time >= *pts && video_time - *pts <= kPtsTolerance)
        return static_cast<std::size_t>(found - frames->begin());
    }
    return std::nullopt;
  }

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
    const bool video = g_str_has_prefix(gst_structure_get_name(gst_caps_get_structure(caps, 0)), "video/x-raw");
    const bool gpu = gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM");
    gst_caps_unref(caps);
    if (!video)
      return;
    if (!gpu) {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->error = "The panorama decoder did not produce GPU video. An NVIDIA-decodable H.264/H.265 file is required.";
      return;
    }
    GstPad* target_pad = gst_element_get_static_pad(self->converter, "sink");
    if (gst_pad_link(pad, target_pad) != GST_PAD_LINK_OK) {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->error = "Could not link the NVIDIA panorama decoder.";
    }
    gst_object_unref(target_pad);
  }

  static gint autoplug_select(GstElement*, GstPad*, GstCaps*, GstElementFactory* factory, gpointer) noexcept {
    const gchar* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    const gchar* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    // GstAutoplugSelectResult is private to playback: TRY=0, SKIP=2.
    return klass && g_strrstr(klass, "Decoder/Video") && g_strcmp0(name, "nvv4l2decoder") != 0 ? 2 : 0;
  }

  static GstPadProbeReturn attach_camera(GstPad*, GstPadProbeInfo* info, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    try {
      std::unique_lock<std::mutex> lock(self->mutex);
      if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        if (self->media.stitching && GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_CAPS) {
          GstCaps* caps = nullptr;
          gst_event_parse_caps(GST_PAD_PROBE_INFO_EVENT(info), &caps);
          int width = 0, height = 0;
          const auto* structure = gst_caps_get_structure(caps, 0);
          if (!gst_structure_get_int(structure, "width", &width) ||
              !gst_structure_get_int(structure, "height", &height) || width <= 0 || height <= 0 ||
              static_cast<unsigned>(width) != self->canvas_width ||
              static_cast<unsigned>(height) != self->canvas_height) {
            self->error = "The stitched output dimensions differ from the recorded canvas";
            return GST_PAD_PROBE_DROP;
          }
        }
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_SEGMENT) {
          if (self->media.stitching) {
            GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
            const GstSegment* source = nullptr;
            gst_event_parse_segment(event, &source);
            GstSegment segment = *source;
            segment.start += self->raw_start;
            segment.position += self->raw_start;
            segment.time += self->raw_start;
            if (GST_CLOCK_TIME_IS_VALID(segment.stop))
              segment.stop += self->raw_start;
            GstEvent* mapped = gst_event_new_segment(&segment);
            gst_event_set_seqnum(mapped, gst_event_get_seqnum(event));
            gst_event_unref(event);
            GST_PAD_PROBE_INFO_DATA(info) = mapped;
          }
          self->segment_generation = self->generation;
          hm::gpu_preview::set_renderer_generation(self->sink, self->generation);
        }
        return GST_PAD_PROBE_OK;
      }
      GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (!buffer || (!self->media.stitching && !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)))) {
        self->error = "Decoded panorama frame has no timestamp.";
        return GST_PAD_PROBE_DROP;
      }
      // Make only the buffer metadata writable; the NVMM pixel allocation is
      // still shared and never mapped/read back. A one-source graph needs no
      // nvstreammux, whose source-event handler consumes upstream seeks.
      buffer = gst_buffer_make_writable(buffer);
      GST_PAD_PROBE_INFO_DATA(info) = buffer;
      if (!buffer) {
        self->error = "Could not acquire writable replay metadata.";
        return GST_PAD_PROBE_DROP;
      }
      if (self->media.stitching) {
        // The production playlist starts a new zero-based epoch after each
        // seek. Use the stitcher's reference-camera PTS, not the mux batch's
        // latest-camera timestamp, and restore the explicitly bound timeline.
        const auto* batch = gst_buffer_get_nvds_batch_meta(buffer);
        if (!batch || batch->num_frames_in_batch != 1 || !batch->frame_meta_list) {
          self->error = "The original cameras did not produce one synchronized stitched frame";
          return GST_PAD_PROBE_DROP;
        }
        const auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
        if (!GST_CLOCK_TIME_IS_VALID(frame->buf_pts) || frame->buf_pts > G_MAXINT64 - self->raw_start) {
          self->error = "The stitched camera frame has an invalid source timestamp";
          return GST_PAD_PROBE_DROP;
        }
        GST_BUFFER_PTS(buffer) = self->raw_start + frame->buf_pts;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
      }
      const auto index = self->frames && self->primed ? self->find_frame(GST_BUFFER_PTS(buffer)) : std::nullopt;
      if (self->frames && self->primed && !index) {
        const auto first = self->video_pts(self->frames->front().pts_ns);
        const auto end = self->video_pts(self->end_pts_ns);
        // The exclusive endpoint can round just below the telemetry PTS in
        // the decoder timebase (including a one-nanosecond difference). Apply
        // the same tolerance used to pair selected frames before diagnosing
        // an unmatched observation or holding the surplus endpoint frame.
        const bool at_end = end && (GST_BUFFER_PTS(buffer) >= *end || *end - GST_BUFFER_PTS(buffer) <= GST_MSECOND);
        if (first && end && GST_BUFFER_PTS(buffer) >= *first && !at_end) {
          g_printerr(
              "Experiment timestamp mismatch: video=%" G_GUINT64_FORMAT " first=%" G_GUINT64_FORMAT
              " end=%" G_GUINT64_FORMAT " source-start=%" G_GUINT64_FORMAT "\n",
              GST_BUFFER_PTS(buffer),
              *first,
              *end,
              self->raw_start);
          self->error = "Video timestamps do not match the recorded samples. Check the media PTS binding.";
        }
        if (at_end) {
          // Decode past the policy endpoint so codecs with reordered frames
          // can emit the final selected image. Hold this one surplus buffer
          // until the next seek/close, rather than decoding the rest of the
          // game or dropping the final delayed pictures with a segment stop.
          const auto generation = self->segment_generation;
          self->range_boundary.wait(lock, [&]() { return self->closing || self->generation != generation; });
        }
        return GST_PAD_PROBE_DROP;
      }
      if (index && self->seeking && *index < self->target)
        return GST_PAD_PROBE_DROP;
      NvDsBatchMeta* batch = gst_buffer_get_nvds_batch_meta(buffer);
      if (!batch) {
        batch = nvds_create_batch_meta(1);
        if (batch) {
          // Match nvstreammux's initialization contract so downstream
          // production elements can deep-copy the batch metadata safely.
          batch->base_meta.batch_meta = batch;
          batch->base_meta.copy_func = nvds_batch_meta_copy_func;
          batch->base_meta.release_func = nvds_batch_meta_release_func;
          batch->max_frames_in_batch = 1;
        }
        NvDsFrameMeta* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
        if (!frame) {
          if (batch)
            nvds_destroy_batch_meta(batch);
          self->error = "Could not allocate the single-frame replay metadata.";
          return GST_PAD_PROBE_DROP;
        }
        frame->batch_id = 0;
        frame->source_id = 0;
        frame->frame_num = index ? self->frames->at(*index).sample_id : 0;
        frame->buf_pts = GST_BUFFER_PTS(buffer);
        nvds_add_frame_meta_to_batch(batch, frame);
        NvDsMeta* meta =
            gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
        if (!meta) {
          nvds_destroy_batch_meta(batch);
          self->error = "Could not attach the single-frame replay metadata.";
          return GST_PAD_PROBE_DROP;
        }
        meta->meta_type = NVDS_BATCH_GST_META;
      }
      if (!batch || !batch->frame_meta_list || batch->num_frames_in_batch != 1) {
        self->error = "Panorama preview requires one DeepStream frame per batch.";
        return GST_PAD_PROBE_DROP;
      }
      auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
      frame->source_frame_width = self->canvas_width;
      frame->source_frame_height = self->canvas_height;
      if (!index)
        return GST_PAD_PROBE_OK; // Initial decoder preroll before the first seek.
      g_object_set(
          self->cropper,
          "fixed-edge-rotation-angle-left",
          static_cast<double>(self->frames->at(*index).edge_rotation_left),
          "fixed-edge-rotation-angle-right",
          static_cast<double>(self->frames->at(*index).edge_rotation_right),
          nullptr);
      const auto& camera = self->frames->at(*index).follower;
      if (camera) {
        NvDsObjectMeta* object = nvds_acquire_obj_meta_from_pool(batch);
        if (!object) {
          self->error = "Could not allocate replay camera metadata.";
          return GST_PAD_PROBE_DROP;
        }
        object->class_id = 99; // Production playcropper's Program camera class.
        object->object_id = UNTRACKED_OBJECT_ID;
        object->confidence = 1.0f;
        object->rect_params.left = camera->left;
        object->rect_params.top = camera->top;
        object->rect_params.width = camera->width();
        object->rect_params.height = camera->height();
        nvds_add_obj_meta_to_frame(frame, object, nullptr);
      }
      return GST_PAD_PROBE_OK;
    } catch (const std::exception& exception) {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->error = std::string("Could not pair replay frame: ") + exception.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->error = "Could not pair replay frame.";
    }
    return GST_PAD_PROBE_DROP;
  }
};

CameraExperimentPreview::CameraExperimentPreview() : impl_(std::make_unique<Impl>()) {}
CameraExperimentPreview::~CameraExperimentPreview() {
  Close();
}

bool CameraExperimentPreview::Open(
    const hm::playtracker_replay::MediaBinding& media,
    unsigned canvas_width,
    unsigned canvas_height,
    std::uint64_t window_id,
    double left_rotation,
    double right_rotation,
    std::string* error) {
  Close();
  if (!canvas_width || !canvas_height || !media.width || !media.height || !window_id ||
      media.video_origin_pts_ns > G_MAXINT64 || media.telemetry_origin_pts_ns > G_MAXINT64)
    return fail(error, "Video binding has invalid dimensions, timestamps, or a missing native window.");
  const auto geometry =
      hm::playtracker_replay::ValidatePanoramaGeometry(media.width, media.height, canvas_width, canvas_height);
  if (!geometry.ok())
    return fail(error, geometry.ToString());
  if (media.stitching)
    g_setenv("USE_NEW_NVSTREAMMUX", "yes", TRUE);
  gst_init(nullptr, nullptr);
  gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  if (!hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements())
    return fail(error, "Camera experiment video preview requires NVIDIA graphics and an X11 display.");
  load_cropper_plugin();
  auto& s = *impl_;
  s.media = media;
  s.canvas_width = canvas_width;
  s.canvas_height = canvas_height;
  s.window_id = window_id;
  s.left_rotation = left_rotation;
  s.right_rotation = right_rotation;
  // Original-camera graph construction/teardown and chapter discovery run
  // off the Qt thread. The selected start becomes known in SetTrajectory.
  return media.stitching ? true : s.build_graph(error);
}

bool CameraExperimentPreview::Impl::build_graph(std::string* error) {
  auto& s = *this;
  s.graph = gst_pipeline_new("camera-experiment");
  s.decoder = media.stitching ? nullptr : gst_element_factory_make("uridecodebin", "panorama-decoder");
  s.converter = gst_element_factory_make("nvvideoconvert", "panorama-rgba");
  GstElement* capsfilter = gst_element_factory_make("capsfilter", "panorama-caps");
  s.cropper = gst_element_factory_make("playcropper", "experiment-cropper");
  s.sink = gst_element_factory_make("hmgpupreviewsink", "experiment-renderer");
  std::vector<GstElement*> elements{s.converter, capsfilter, s.cropper, s.sink};
  if (!media.stitching)
    elements.push_back(s.decoder);
  for (GstElement* element : elements) {
    if (element)
      gst_bin_add(GST_BIN(s.graph), element);
  }
  if (std::any_of(elements.begin(), elements.end(), [](auto* element) { return !element; })) {
    clear_graph();
    return fail(error, "Could not create NVIDIA decoder, playcropper, or GPU renderer. Check HStream runtime plugins.");
  }
  if (media.stitching) {
    s.raw = std::make_unique<CameraExperimentSource>();
    if (!s.raw->Build(s.graph, *media.stitching, s.raw_start, error) || !gst_element_link(s.raw->output(), s.converter))
      return false;
  } else {
    gchar* uri = gst_filename_to_uri(media.path.c_str(), nullptr);
    if (!uri) {
      clear_graph();
      return fail(error, "Panorama path must identify a local video file.");
    }
    g_object_set(s.decoder, "uri", uri, nullptr);
    g_free(uri);
    g_signal_connect(s.decoder, "pad-added", G_CALLBACK(Impl::pad_added), &s);
    g_signal_connect(s.decoder, "autoplug-select", G_CALLBACK(Impl::autoplug_select), &s);
  }
  g_object_set(s.converter, "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "output-buffers", 2, nullptr);
  GstCaps* caps = rgba_caps();
  g_object_set(capsfilter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(
      s.cropper,
      "plugin-type",
      "playcropper",
      "output-width",
      1280U,
      "output-height",
      720U,
      "num-batch-buffers",
      1U,
      "num-output-buffers",
      2U,
      "plugin-private-config",
      "show-scoreboard=0;show=0;no-crop=0;transform-object-meta=0",
      "fixed-edge-rotation-angle-left",
      left_rotation,
      "fixed-edge-rotation-angle-right",
      right_rotation,
      nullptr);
  if (media.stitching && !media.stitching->high_bit_depth) {
    // Match production: grade 8-bit sources once, after the camera crop.
    g_object_set(
        s.cropper,
        "exposure",
        media.stitching->exposure,
        "shadow-lift",
        media.stitching->shadow_lift,
        "shadow-lift-black-point",
        media.stitching->shadow_lift_black_point ? TRUE : FALSE,
        nullptr);
  }
  g_object_set(
      s.sink,
      "window-id",
      window_id,
      "channel",
      "experiment",
      "sync",
      TRUE,
      "async",
      media.stitching ? FALSE : TRUE,
      "qos",
      FALSE,
      "enable-last-sample",
      FALSE,
      nullptr);
  hm::gpu_preview::set_source_geometry(s.sink, 1280, 720);
  const bool linked = gst_element_link_many(s.converter, capsfilter, s.cropper, s.sink, nullptr);
  if (!linked) {
    clear_graph();
    return fail(error, "Could not link the GPU panorama replay graph.");
  }
  GstPad* probe = gst_element_get_static_pad(capsfilter, "src");
  gst_pad_add_probe(
      probe,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      Impl::attach_camera,
      &s,
      nullptr);
  gst_object_unref(probe);
  s.bus = gst_element_get_bus(s.graph);
  return true;
}

bool CameraExperimentPreview::SetTrajectory(
    std::shared_ptr<const std::vector<Frame>> frames,
    std::uint64_t end_pts_ns,
    std::string* error,
    bool play,
    std::size_t index) {
  if ((!impl_->media.stitching && !impl_->graph) || !frames || frames->empty() || frames->back().pts_ns >= end_pts_ns)
    return fail(error, "No prepared trajectory is available for preview.");
  Pause();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->frames = std::move(frames);
    impl_->end_pts_ns = end_pts_ns;
    impl_->presented.reset();
    if (!impl_->video_pts(impl_->frames->front().pts_ns) || !impl_->video_pts(end_pts_ns))
      return fail(error, "The media PTS binding maps the selected range outside video time.");
  }
  return Seek(index, play, error);
}

bool CameraExperimentPreview::Seek(std::size_t index, bool play, std::string* error) {
  auto& s = *impl_;
  if ((!s.media.stitching && !s.graph) || !s.frames || index >= s.frames->size())
    return fail(error, "No selected frame is available.");
  if (s.media.stitching) {
    std::uint64_t start = 0;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (s.rebuilding.valid() || s.seeking) {
        s.pending_seek = std::make_pair(index, play);
        return true;
      }
      const auto mapped = s.video_pts(s.frames->at(index).pts_ns);
      if (!mapped)
        return fail(error, "The selected frame is outside the source time binding");
      start = *mapped;
      s.target = index;
      s.playing = play;
      s.seeking = true;
      s.presented.reset();
      s.closing = true;
      ++s.generation;
      s.range_boundary.notify_all();
    }
    if (s.raw)
      s.raw->Suspend();
    s.rebuilding = std::async(std::launch::async, [&s, start]() {
      try {
        s.clear_graph();
        {
          std::lock_guard<std::mutex> lock(s.mutex);
          s.closing = false;
          s.raw_start = start;
          s.error.clear();
        }
        std::string error;
        if (!s.build_graph(&error))
          return error.empty() ? std::string("Could not build original-camera preview") : error;
        return std::string();
      } catch (const std::exception& error) {
        return std::string(error.what());
      }
    });
    return true;
  }
  std::uint64_t start = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.initial_seek_pending) {
      s.target = index;
      s.playing = play;
      return true;
    }
    if (s.seeking) {
      // Flush/segment processing in the decoder and cropper is asynchronous.
      // Coalesce rapid transport/comparison requests and begin the newest one
      // only after the current target has actually reached the renderer.
      s.pending_seek = std::make_pair(index, play);
      return true;
    }
    const auto mapped_start = s.video_pts(s.frames->at(index).pts_ns);
    const auto mapped_end = s.video_pts(s.end_pts_ns);
    if (!mapped_start || !mapped_end)
      return fail(error, "The selected frame lies outside the media PTS binding.");
    start = *mapped_start;
    ++s.generation;
    s.range_boundary.notify_all();
    s.target = index;
    s.seeking = true;
    s.playing = play;
    s.seek_started = std::chrono::steady_clock::now();
    s.error.clear();
    if (!s.primed)
      s.initial_seek_pending = true;
  }
  gst_element_set_state(s.graph, GST_STATE_PAUSED);
  // Decodebin's dynamic source pads must exist before a seek can propagate.
  // Poll completes this initial seek after preroll, without blocking Qt.
  if (s.initial_seek_pending)
    return true;
  if (!gst_element_seek(
          s.graph,
          1.0,
          GST_FORMAT_TIME,
          // Start decoding at a complete keyframe and trim in attach_camera.
          // NVIDIA's accurate-seek path can synthesize PTS from the requested
          // segment start, shifting fractional-rate frames out of alignment.
          static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET,
          // Keep the selected image when the encoded PTS rounds just below
          // its telemetry PTS. The metadata probe drops earlier observations.
          start > GST_MSECOND ? start - GST_MSECOND : 0,
          GST_SEEK_TYPE_NONE,
          0)) {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.seeking = false;
    s.playing = false;
    return fail(error, "The panorama decoder rejected the seek. Use a seekable local H.264/H.265 file.");
  }
  if (play)
    gst_element_set_state(s.graph, GST_STATE_PLAYING);
  return true;
}

void CameraExperimentPreview::Pause() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->playing = false;
    if (impl_->pending_seek)
      impl_->pending_seek->second = false;
  }
  if (!impl_->rebuilding.valid() && impl_->graph) {
    if (impl_->raw) {
      gst_element_set_locked_state(impl_->sink, TRUE);
      gst_element_set_state(impl_->sink, GST_STATE_PAUSED);
    } else {
      gst_element_set_state(impl_->graph, GST_STATE_PAUSED);
    }
  }
}

void CameraExperimentPreview::SetLoop(bool loop) {
  impl_->loop = loop;
}

CameraExperimentPreview::Status CameraExperimentPreview::Poll() {
  auto& s = *impl_;
  if (s.rebuilding.valid()) {
    if (s.rebuilding.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return {s.playing, true, std::nullopt, {}};
    const auto failure = s.rebuilding.get();
    if (!failure.empty()) {
      s.clear_graph();
      std::lock_guard<std::mutex> lock(s.mutex);
      s.seeking = s.playing = false;
      s.initial_seek_pending = false;
      s.error = failure;
      return {false, false, std::nullopt, failure};
    }
    if (s.pending_seek) {
      const auto next = std::exchange(s.pending_seek, std::nullopt);
      s.seeking = false;
      std::string error;
      Seek(next->first, next->second, &error);
      return {s.playing, true, std::nullopt, error};
    }
    s.raw->Resume();
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.primed = true;
      s.initial_seek_pending = true;
      s.seek_started = std::chrono::steady_clock::now();
    }
    // Source recovery after a keyframe seek needs running NVIDIA decoders.
    // A paused renderer prerolls exactly one frame and applies backpressure
    // while the rest of this dedicated graph remains PLAYING.
    if (!s.playing) {
      gst_element_set_locked_state(s.sink, TRUE);
      gst_element_set_state(s.sink, GST_STATE_PAUSED);
    }
    if (gst_element_set_state(s.graph, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      s.error = "Could not start the original-camera preview";
      s.seeking = s.playing = false;
    }
  }
  if (s.raw && s.initial_seek_pending && s.raw->Position()) {
    s.initial_seek_pending = false;
    if (s.playing)
      gst_element_set_state(s.graph, GST_STATE_PLAYING);
  }
  bool ended = false;
  if (s.bus) {
    while (GstMessage* message = gst_bus_pop(s.bus)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gst_message_parse_error(message, &error, nullptr);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.error = error ? error->message : "Panorama preview failed.";
        g_clear_error(&error);
      } else if (
          GST_MESSAGE_TYPE(message) == GST_MESSAGE_SEGMENT_DONE || GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        ended = true;
      }
      gst_message_unref(message);
    }
  }
  if (!s.raw && s.initial_seek_pending &&
      gst_element_get_state(s.graph, nullptr, nullptr, 0) == GST_STATE_CHANGE_SUCCESS) {
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.primed = true;
      s.initial_seek_pending = false;
      s.seeking = false;
    }
    std::string error;
    if (!Seek(s.target, s.playing, &error)) {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.error = std::move(error);
    }
  }
  std::uint64_t generation = 0;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  const bool acknowledged = s.sink && hm::gpu_preview::presented_frame(s.sink, &generation, &pts);
  std::optional<std::pair<std::size_t, bool>> next_seek;
  bool pause_at_end = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (acknowledged && generation == s.generation) {
      s.presented = s.find_frame(pts);
      if (s.presented && (!s.seeking || *s.presented == s.target || s.playing)) {
        if (s.seeking)
          g_print(
              "HSTREAM_EXPERIMENT_PRESENTED generation=%" G_GUINT64_FORMAT " pts=%" G_GUINT64_FORMAT " frame=%zu\n",
              generation,
              pts,
              *s.presented);
        s.seeking = false;
      }
    }
    if (s.seeking && std::chrono::steady_clock::now() - s.seek_started > std::chrono::seconds(15)) {
      g_printerr(
          "Experiment seek timeout: target=%zu presented=%" G_GUINT64_FORMAT " expected=%" G_GUINT64_FORMAT "\n",
          s.target,
          pts,
          s.video_pts(s.frames->at(s.target).pts_ns).value_or(GST_CLOCK_TIME_NONE));
      s.error = "No matching frame was presented after the seek. Check the sources, range, and PTS binding.";
      s.seeking = false;
    }
    if (!s.seeking && s.pending_seek && s.error.empty())
      next_seek = std::exchange(s.pending_seek, std::nullopt);
    if (s.playing && !s.seeking && !next_seek && s.presented && s.frames && *s.presented + 1 == s.frames->size()) {
      if (s.loop)
        next_seek = std::make_pair(0U, true);
      else
        pause_at_end = true;
    } else if (ended && s.playing && !next_seek) {
      s.error = "The video ended before the final selected sample. Check the media binding and range.";
    }
  }
  if (pause_at_end)
    Pause();
  if (next_seek) {
    std::string error;
    if (!Seek(next_seek->first, next_seek->second, &error)) {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.error = std::move(error);
    }
  }
  std::lock_guard<std::mutex> lock(s.mutex);
  return {s.playing, s.seeking, s.presented, s.error};
}

bool CameraExperimentPreview::Capture(const std::string& path, std::string* error) {
  if (impl_->rebuilding.valid() || !impl_->sink)
    return fail(error, "Wait for the selected frame to finish loading before capturing it.");
  std::vector<std::uint8_t> rgba;
  unsigned width = 0;
  unsigned height = 0;
  if (!hm::gpu_preview::capture_presented_frame(impl_->sink, &rgba, &width, &height, error))
    return false;
  // Explicit operator/test screenshot only; never called by normal playback.
  QImage image(rgba.data(), width, height, width * 4, QImage::Format_RGBA8888);
  return image.save(QString::fromStdString(path)) || fail(error, "Could not save the preview screenshot.");
}

void CameraExperimentPreview::Impl::clear_graph() {
  if (raw) {
    raw->Cancel();
    if (sink)
      gst_element_set_locked_state(sink, FALSE);
  }
  if (graph) {
    gst_element_set_state(graph, GST_STATE_NULL);
    if (sink)
      hm::gpu_preview::quiesce(sink, generation);
    if (bus)
      gst_object_unref(bus);
    gst_object_unref(graph);
  }
  graph = decoder = converter = cropper = sink = nullptr;
  bus = nullptr;
  raw.reset();
}

void CameraExperimentPreview::Close() {
  if (impl_->rebuilding.valid())
    impl_->rebuilding.wait();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closing = true;
    impl_->range_boundary.notify_all();
  }
  if (impl_->raw)
    impl_->raw->Suspend();
  impl_->clear_graph();
  impl_ = std::make_unique<Impl>();
}
