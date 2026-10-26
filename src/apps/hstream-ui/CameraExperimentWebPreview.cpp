#include "src/apps/hstream-ui/CameraExperimentWebPreview.h"

#include "hstream/src/apps/apps-common/PreviewOverlayPainter.h"
#include "hstream/src/apps/apps-common/WebPreviewTarget.h"
#include "hstream/src/apps/apps-common/WebRtcSignaling.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gstnvdsmeta.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {
class WebOutput final : public CameraExperimentPreviewOutput {
 public:
  explicit WebOutput(hm::ui::HighlightWebPreviewConfig config) : config_(std::move(config)) {}
  ~WebOutput() override {
    RetireSource();
    stopping_ = true;
    changed_.notify_all();
    if (pump_.joinable())
      pump_.join();
    if (signaling_.joinable()) {
      GSource* source = g_idle_source_new();
      g_source_set_callback(
          source,
          [](gpointer pointer) -> gboolean {
            auto& self = *static_cast<WebOutput*>(pointer);
            hm::webrtc::Stop(self.graph_);
            g_main_loop_quit(self.loop_);
            return G_SOURCE_REMOVE;
          },
          this,
          nullptr);
      g_source_attach(source, context_);
      g_source_unref(source);
      signaling_.join();
    }
    if (graph_)
      gst_element_set_state(graph_, GST_STATE_NULL);
    if (route_)
      gst_object_unref(route_);
    target_.reset();
    if (graph_)
      gst_object_unref(graph_);
    g_main_loop_unref(loop_);
    g_main_context_unref(context_);
  }

  bool Start(std::string* error) {
    gst_init(nullptr, nullptr);
    gst_registry_scan_path(gst_registry_get(), "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
    const auto& o = config_.options;
    if (!hm::preview::ValidWebPreviewRun(config_.run) || config_.port < 1024 || config_.port > 65535)
      return Fail(error, "Invalid camera preview session identity or signaling port");
    graph_ = gst_pipeline_new("camera-preview-transport");
    appsrc_ = gst_element_factory_make("appsrc", "camera-preview-input");
    if (!graph_ || !appsrc_)
      return Fail(error, "Camera preview requires GStreamer appsrc");
    gst_bin_add(GST_BIN(graph_), appsrc_);
    g_object_set(
        appsrc_,
        "format",
        GST_FORMAT_TIME,
        "is-live",
        TRUE,
        "block",
        FALSE,
        "max-buffers",
        guint64{1},
        "leaky-type",
        2,
        nullptr);
    hm::preview::WebPreviewConfig target;
    target.name = "camera_browser_preview";
    target.stream_id = config_.run + ":camera:1";
    target.eof_channel = "program";
    target.surface = {o.width, o.height, o.rate_numerator, o.rate_denominator, o.surface_pool_frames, 0};
    target.conversion_pool_frames = o.conversion_pool_frames;
    target.bitrate = o.bitrate;
    target.idr_interval = o.idr_interval;
    target.h264_profile = o.h264_profile;
    target.encoder_tuning = o.encoder_tuning;
    target.rtp_payload = o.rtp_payload;
    target.pending_frames = o.pending_frames;
    target.pending_events = o.pending_events;
    target.presentation_history = o.presentation_history;
    target.stun_server = config_.stun_server;
    target_ = hm::preview::WebPreviewTarget::Create(
        target,
        [this](GstBuffer* frame, const hm::preview::OwnedSurfaceView& surface) { return Paint(frame, surface); },
        error);
    if (!target_ || !gst_bin_add(GST_BIN(graph_), target_->bin()))
      return Fail(error, "Could not construct camera browser target");
    route_ = target_->AddRoute("program");
    auto* source = gst_element_get_static_pad(appsrc_, "src");
    const bool linked = source && route_ && gst_pad_link(source, route_) == GST_PAD_LINK_OK && target_->DetachPeer(1);
    if (source) {
      gst_pad_add_probe(
          source,
          GST_PAD_PROBE_TYPE_BUFFER,
          [](GstPad*, GstPadProbeInfo* info, gpointer pointer) {
            auto& self = *static_cast<WebOutput*>(pointer);
            const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            std::lock_guard<std::mutex> lock(self.mutex_);
            return self.admitting_ && buffer && GST_BUFFER_OFFSET_END(buffer) == self.generation_ &&
                    self.active_generation_ == self.generation_
                ? GST_PAD_PROBE_OK
                : GST_PAD_PROBE_DROP;
          },
          this,
          nullptr);
      gst_object_unref(source);
    }
    if (!linked)
      return Fail(error, "Could not connect camera browser target");
    auto signaling = target_->Signaling({{config_.run, 0, 1}, o.poll_interval_ms, o.events_per_batch});
    if (!signaling)
      return Fail(error, "Could not prepare camera browser signaling");
    observer_ = signaling->observer;
    std::promise<bool> ready;
    auto result = ready.get_future();
    signaling_ = std::thread([this, signaling, ready = std::move(ready)]() mutable {
      g_main_context_push_thread_default(context_);
      const bool started = hm::webrtc::Start(target_->endpoint(), config_.port, signaling);
      ready.set_value(started);
      if (started)
        g_main_loop_run(loop_);
      g_main_context_pop_thread_default(context_);
    });
    if (!result.get()) {
      signaling_.join();
      return Fail(error, "Could not listen for camera preview signaling");
    }
    if (gst_element_set_state(graph_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      return Fail(error, "Could not start camera preview transport");
    pump_ = std::thread([this] { Pump(); });
    return true;
  }

  GstElement* CreateSink(const Settings& settings, std::string* error) override {
    sink_ = gst_element_factory_make("appsink", "camera-preview-held-frame");
    if (!sink_) {
      Fail(error, "Camera preview requires GStreamer appsink");
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      admitting_ = true;
      mask_ = settings.rink_mask;
    }
    gst_element_sync_state_with_parent(appsrc_);
    SetOverlays(settings.players, settings.play, settings.rink);
    g_object_set(
        sink_,
        "sync",
        settings.realtime ? TRUE : FALSE,
        "async",
        settings.asynchronous ? TRUE : FALSE,
        "max-lateness",
        gint64{-1},
        "qos",
        FALSE,
        "max-buffers",
        1u,
        "drop",
        TRUE,
        "enable-last-sample",
        FALSE,
        "wait-on-eos",
        FALSE,
        nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_preroll = [](GstAppSink* sink, gpointer pointer) {
      return static_cast<WebOutput*>(pointer)->Accept(gst_app_sink_pull_preroll(sink));
    };
    callbacks.new_sample = [](GstAppSink* sink, gpointer pointer) {
      return static_cast<WebOutput*>(pointer)->Accept(gst_app_sink_pull_sample(sink));
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink_), &callbacks, this, nullptr);
    return sink_;
  }
  void SetGeneration(std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_generation_ == generation)
      return;
    source_generation_ = generation;
    // Preview Close/Open starts its local generation sequence over. The live
    // transport keeps its peer, so its operation sequence must never restart.
    if (generation)
      ++generation_;
    ReplaceHeld(nullptr);
    presented_generation_ = 0;
    presented_pts_ = GST_CLOCK_TIME_NONE;
    changed_.notify_all();
  }
  bool Presented(std::uint64_t* generation, GstClockTime* pts) override {
    if (const auto frame = observer_->TakePresented()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (admitting_ && frame->operation == generation_) {
        presented_generation_ = source_generation_;
        presented_pts_ = frame->source_pts;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!presented_generation_ || !GST_CLOCK_TIME_IS_VALID(presented_pts_))
      return false;
    *generation = presented_generation_;
    *pts = presented_pts_;
    return true;
  }
  std::uint64_t PresentationOperation() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }
  void SetOverlays(bool players, bool play, bool rink) override {
    layers_ = (players ? 1u : 0u) | (play ? 2u : 0u) | (rink ? 4u : 0u);
  }
  void Redraw() override {} // Held GPU frames are continuously repainted.
  bool Capture(const std::string&, std::string* error) override {
    return Fail(error, "Capture camera preview screenshots from the browser's presented video frame");
  }
  void RetireSource() override {
    std::lock_guard<std::mutex> transport_lock(transport_mutex_);
    std::uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      admitting_ = false;
      ReplaceHeld(nullptr);
      presented_generation_ = 0;
      presented_pts_ = GST_CLOCK_TIME_NONE;
      generation = generation_;
      active_generation_ = 0;
      changed_.notify_all();
    }
    // Closing selection waits for an admitted GPU copy to complete. Stopping
    // only appsrc clears its bounded queue and source-buffer references while
    // leaving the WebRTC target and current browser peer alive.
    if (target_ && generation)
      target_->Select(nullptr, generation);
    if (appsrc_)
      gst_element_set_state(appsrc_, GST_STATE_READY);
  }
  void ReleaseSink() override {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = nullptr;
    source_generation_ = 0;
  }

 private:
  static bool Fail(std::string* error, const char* message) {
    if (error)
      *error = message;
    return false;
  }
  void ReplaceHeld(GstSample* sample) {
    if (sample)
      gst_sample_ref(sample);
    if (held_)
      gst_sample_unref(held_);
    held_ = sample;
  }
  GstFlowReturn Accept(GstSample* sample) {
    if (!sample)
      return GST_FLOW_FLUSHING;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (admitting_ && source_generation_ && gst_sample_get_buffer(sample))
        ReplaceHeld(sample);
    }
    gst_sample_unref(sample);
    changed_.notify_all();
    return GST_FLOW_OK;
  }
  bool Paint(GstBuffer* selected, const hm::preview::OwnedSurfaceView& output) {
    GstBuffer* original = hm::preview::source_frame(selected);
    // The transport header retains the exact source buffer by parent metadata;
    // its own header carries only the queue-fencing generation.
    auto* parent = original ? gst_buffer_get_parent_buffer_meta(original) : nullptr;
    if (parent)
      original = parent->buffer;
    const auto* batch = original ? gst_buffer_get_nvds_batch_meta(original) : nullptr;
    const auto* metadata =
        batch && batch->frame_meta_list ? static_cast<const NvDsFrameMeta*>(batch->frame_meta_list->data) : nullptr;
    const auto flags = layers_.load();
    auto scene = hm::gpu_preview::BuildPreviewOverlayScene(
        metadata, {"program", 1280, 720, 1280, 720, bool(flags & 1u), bool(flags & 2u), 0, 0}, commands_, nullptr);
    std::shared_ptr<const hm::gpu_preview::RinkMaskImage> mask;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      mask = mask_;
    }
    // The replay session owns this immutable historical mask. Never consult the
    // current game's mask or calibration generation while replaying a trial.
    std::shared_ptr<const hm::gpu_preview::RinkMaskImage> paint_mask;
    if (mask && scene.program_transform) {
      const auto& transform = *scene.program_transform;
      const auto recorded_width = hm::gpu_preview::ExactPreviewDimension(transform.metadata_width);
      const auto recorded_height = hm::gpu_preview::ExactPreviewDimension(transform.metadata_height);
      const auto input_width = hm::gpu_preview::ExactPreviewDimension(transform.input_width);
      const auto input_height = hm::gpu_preview::ExactPreviewDimension(transform.input_height);
      if (recorded_width && recorded_height && input_width && input_height &&
          hm::gpu_preview::rink_mask_dimensions_match(*mask, *recorded_width, *recorded_height)) {
        if (mask == mapped_source_ && *input_width == mapped_width_ && *input_height == mapped_height_)
          paint_mask = mapped_mask_;
        else {
          // A saved panorama may be uniformly scaled relative to telemetry.
          // Its validated replay transform supplies the corresponding input
          // canvas; the historical alpha texture keeps normalized coordinates.
          auto mapped = std::make_shared<hm::gpu_preview::RinkMaskImage>(*mask);
          mapped->canvas_width = *input_width;
          mapped->canvas_height = *input_height;
          mapped_source_ = mask;
          mapped_width_ = *input_width;
          mapped_height_ = *input_height;
          paint_mask = mapped_mask_ = std::move(mapped);
        }
      }
    }
    scene.stitched_output_generation = "recorded-camera-session";
    painter_.SetRinkMask(std::move(paint_mask), scene.stitched_output_generation);
    auto* pixels = static_cast<std::uint8_t*>(output.rgba) + output.video.top * output.pitch + output.video.left * 4;
    const auto painted = painter_.Paint(
        {pixels, output.pitch, output.video.width, output.video.height},
        scene,
        commands_,
        {"program", 1280, 720, bool(flags & 4u)},
        output.stream);
    return painted.status == hm::gpu_preview::PreviewPaintStatus::kOk ||
        painted.status == hm::gpu_preview::PreviewPaintStatus::kOverlaySuppressed;
  }
  void Pump() {
    const auto& o = config_.options;
    const auto interval =
        std::chrono::nanoseconds(gst_util_uint64_scale(GST_SECOND, o.rate_denominator, o.rate_numerator));
    auto next = std::chrono::steady_clock::now();
    while (!stopping_) {
      GstSample* sample = nullptr;
      std::uint64_t generation = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_until(lock, next, [this] { return stopping_.load(); });
        if (stopping_)
          break;
        if (held_ && admitting_) {
          sample = gst_sample_ref(held_);
          generation = generation_;
        }
      }
      next = std::chrono::steady_clock::now() + interval;
      if (!sample)
        continue;
      std::lock_guard<std::mutex> transport_lock(transport_mutex_);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!admitting_ || generation != generation_) {
          gst_sample_unref(sample);
          continue;
        }
      }
      if (generation != active_generation_) {
        if (!target_->Select(route_, generation)) {
          gst_sample_unref(sample);
          continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        active_generation_ = generation;
      }
      GstBuffer* original = gst_sample_get_buffer(sample);
      GstBuffer* header = gst_buffer_new();
      gst_buffer_copy_into(header, original, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
      for (guint index = 0; index < gst_buffer_n_memory(original); ++index)
        gst_buffer_append_memory(header, gst_memory_ref(gst_buffer_peek_memory(original, index)));
      gst_buffer_add_parent_buffer_meta(header, original);
      GST_BUFFER_OFFSET_END(header) = generation;
      gst_app_src_set_caps(GST_APP_SRC(appsrc_), gst_sample_get_caps(sample));
      gst_app_src_push_buffer(GST_APP_SRC(appsrc_), header);
      gst_sample_unref(sample);
    }
  }

  hm::ui::HighlightWebPreviewConfig config_;
  GstElement *graph_{nullptr}, *appsrc_{nullptr}, *sink_{nullptr};
  GstPad* route_{nullptr};
  std::unique_ptr<hm::preview::WebPreviewTarget> target_;
  std::shared_ptr<hm::preview::WebPreviewObserver> observer_;
  GMainContext* context_{g_main_context_new()};
  GMainLoop* loop_{g_main_loop_new(context_, FALSE)};
  std::thread signaling_, pump_;
  mutable std::mutex mutex_;
  std::mutex transport_mutex_;
  std::condition_variable changed_;
  GstSample* held_{nullptr};
  bool admitting_{false};
  std::uint64_t generation_{0}, source_generation_{0}, active_generation_{0}, presented_generation_{0};
  GstClockTime presented_pts_{GST_CLOCK_TIME_NONE};
  std::atomic<bool> stopping_{false};
  std::atomic<unsigned> layers_{0};
  std::shared_ptr<const hm::gpu_preview::RinkMaskImage> mask_;
  std::shared_ptr<const hm::gpu_preview::RinkMaskImage> mapped_source_, mapped_mask_;
  unsigned mapped_width_{0}, mapped_height_{0};
  hm::gpu_preview::PreviewOverlayPainter painter_;
  hm::draw_display::analytics::CommandList commands_;
};
} // namespace

std::shared_ptr<CameraExperimentPreviewOutput> MakeCameraExperimentWebPreviewOutput(
    const hm::ui::HighlightWebPreviewConfig& config,
    std::string* error) {
  auto output = std::make_shared<WebOutput>(config);
  return output->Start(error) ? output : nullptr;
}
