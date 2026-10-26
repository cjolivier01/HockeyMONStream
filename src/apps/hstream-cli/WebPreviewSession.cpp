#include "WebPreviewSession.h"
#include "WebPreviewRuntime.h"
#include "hstream/src/libs/stitching/FieldMaskArtifact.h"

#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/apps/apps-common/PlayerAnalyticsRouting.h"
#include "hstream/src/apps/apps-common/PreviewOverlayPainter.h"
#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"

#include <gstnvdsmeta.h>

#include <atomic>
#include <chrono>
#include <utility>

namespace hm::preview {
struct WebPreviewSession::RouteState {
  GstElement *ingress{nullptr}, *drain{nullptr};
  GstPad* input{nullptr};
  uint64_t operation{0};
  std::atomic<unsigned> width{0}, height{0};
  ~RouteState() {
    if (input)
      gst_object_unref(input);
  }
};

struct WebPreviewSession::PaintState {
  gpu_preview::PreviewOverlayPainter painter;
  draw_display::analytics::CommandList commands;
  std::map<std::string, std::shared_ptr<RouteState>> channels;
  unsigned program_width, program_height, source_width, source_height;
  unsigned analytics_layers;
  float joint_confidence;
  // One immutable snapshot binds the three flags and their acknowledged epoch.
  std::shared_ptr<const pipeline_internal::PreviewOverlayCommand> overlays =
      std::make_shared<const pipeline_internal::PreviewOverlayCommand>();
  RouteOwner::Session session;
  WebPreviewOverlayArtifactMailbox artifacts;
  void ReportArtifact(const FrameIdentityMeta& identity, uint64_t generation, bool ready, const char* reason) {
    // The mailbox never waits on GPU or disk work and retains one status only.
    artifacts.Publish(
        {session,
         identity.operation,
         generation,
         identity.channel,
         ready ? "ready" : "unavailable",
         ready ? "" : reason});
  }
  std::string game_dir, invalidation, mask_generation;
  unsigned mask_width{0}, mask_height{0}, mask_failures{0};
  bool mask_loaded{false};
  std::chrono::steady_clock::time_point mask_retry;

  void EnsureMask(const gpu_preview::PreviewOverlayScene& scene, unsigned flags) {
    if (!(flags & 4U) || !scene.diagnostic_coordinates_valid)
      return;
    const auto width = gpu_preview::ExactPreviewDimension(scene.stitched_surface_width);
    const auto height = gpu_preview::ExactPreviewDimension(scene.stitched_surface_height);
    if (!width || !height || scene.stitched_output_generation.empty())
      return;
    if (mask_generation != scene.stitched_output_generation || mask_width != *width || mask_height != *height) {
      // Invalidate before any replacement load. The painter also checks the
      // immutable image identity against every frame and its actual canvas.
      painter.SetRinkMask({}, {});
      mask_generation = scene.stitched_output_generation;
      mask_width = *width;
      mask_height = *height;
      mask_loaded = false;
      mask_failures = 0;
      mask_retry = {};
    }
    const auto now = std::chrono::steady_clock::now();
    if (mask_loaded || now < mask_retry)
      return;
    std::shared_ptr<const gpu_preview::RinkMaskImage> image;
    const auto status =
        stitching::visit_current_field_mask(game_dir, mask_generation, invalidation, [&](const std::string& path) {
          gpu_preview::RinkMaskLoadOptions options;
          options.downsample_to_texture_budget = true;
          options.maximum_source_dimension =
              std::max(gpu_preview::kMaximumRinkMaskDimension, std::max(*width, *height));
          options.maximum_source_pixels = gpu_preview::kMaximumPreviewRinkMaskSourcePixels;
          options.maximum_resource_bytes = gpu_preview::kMaximumPreviewRinkMaskResourceBytes;
          auto loaded = gpu_preview::load_rink_mask_png(path, {}, {}, options);
          if (!loaded)
            return absl::FailedPreconditionError(loaded.message);
          if (!gpu_preview::rink_mask_dimensions_match(loaded.image, *width, *height))
            return absl::FailedPreconditionError("field mask differs from the live stitched canvas");
          image = std::make_shared<const gpu_preview::RinkMaskImage>(std::move(loaded.image));
          return absl::OkStatus();
        });
    if (!status.ok()) {
      if (!mask_failures)
        g_printerr(
            "HSTREAM_PREVIEW_OVERLAY channel=web status=validation-failed message=%s\n", status.ToString().c_str());
      mask_failures = std::min(mask_failures + 1U, 6U);
      mask_retry = now + std::chrono::seconds(gpu_preview::rink_mask_retry_delay_seconds(mask_failures));
      return;
    }
    painter.SetRinkMask(std::move(image), mask_generation);
    mask_loaded = true;
  }

  bool Paint(GstBuffer* frame, const OwnedSurfaceView& output) {
    const auto* identity = frame_identity(frame);
    if (!identity || !identity->channel || !output.video.width || !output.video.height)
      return false;
    const auto route = channels.find(identity->channel);
    if (route == channels.end())
      return false;
    const bool source = route->first.compare(0, 6, "source") == 0;
    // Source views have no optional processing overlays. Their NVMM pixels
    // already occupy the target's independently owned letterboxed subimage.
    if (source)
      return true;
    auto* native = source_frame(frame);
    auto* batch = native ? gst_buffer_get_nvds_batch_meta(native) : nullptr;
    const auto* metadata =
        batch && batch->frame_meta_list ? static_cast<const NvDsFrameMeta*>(batch->frame_meta_list->data) : nullptr;
    const auto selected = std::atomic_load(&overlays);
    const auto selection = selected->selection;
    const auto flags = (selection.players ? 1U : 0U) | (selection.play ? 2U : 0U) | (selection.rink ? 4U : 0U);
    const gpu_preview::PreviewOverlayOptions options{
        route->first,
        source ? source_width : program_width,
        source ? source_height : program_height,
        route->second->width.load(std::memory_order_acquire),
        route->second->height.load(std::memory_order_acquire),
        (flags & 1U) != 0,
        (flags & 2U) != 0,
        analytics_layers,
        joint_confidence};
    const auto scene = gpu_preview::BuildPreviewOverlayScene(metadata, options, commands, nullptr);
    EnsureMask(scene, flags);
    auto* pixels = static_cast<uint8_t*>(output.rgba) + output.video.top * output.pitch + output.video.left * 4;
    const auto result = painter.Paint(
        {pixels, output.pitch, output.video.width, output.video.height},
        scene,
        commands,
        {route->first, options.negotiated_width, options.negotiated_height, (flags & 4U) != 0},
        output.stream);
    if (selection.rink) {
      const char* reason = !scene.diagnostic_coordinates_valid ? "The preview frame is missing its crop geometry."
          : scene.stitched_output_generation.empty() ? "The preview frame is missing its calibration identity."
          : !mask_loaded                             ? "Rink mask overlay unavailable"
          : result.mask_identity_missing             ? "The saved rink mask does not match this preview frame."
                                                     : "The GPU rink mask overlay is unavailable.";
      ReportArtifact(*identity, selected->generation, result.mask_drawn, reason);
    }
    return result.status == gpu_preview::PreviewPaintStatus::kOk ||
        result.status == gpu_preview::PreviewPaintStatus::kOverlaySuppressed;
  }
};

WebPreviewSession::WebPreviewSession(AppCtx& app, WebPreviewOptions options)
    : app_(app), options_(std::move(options)), paint_(std::make_shared<PaintState>()) {}

absl::StatusOr<std::unique_ptr<WebPreviewSession>> WebPreviewSession::Prepare(
    AppCtx& app,
    const YAML::Node& resolved,
    RouteOwner::Session identity,
    const std::string& retained_selection,
    const std::string& game_dir) {
  auto options = ReadWebPreviewOptions(resolved);
  if (!options.ok())
    return options.status();
  if (!ValidWebPreviewRun(identity.run) || !identity.topology)
    return absl::InvalidArgumentError("Selectable preview requires a valid run and topology identity");
  NvDsSinkEncoderConfig* selected = nullptr;
  for (unsigned i = 0; i < app.config.num_sink_sub_bins; ++i) {
    auto& sink = app.config.sink_bin_sub_bin_config[i];
    if (!sink.enable || sink.type != NV_DS_SINK_WEBRTC)
      continue;
    if (selected || sink.source_id != 0 || sink.link_to_demux)
      return absl::InvalidArgumentError("Selectable preview requires one non-demux WEBRTC sink for source 0");
    selected = &sink.encoder_config;
  }
  if (!selected || !selected->webrtc_port || app.config.tiled_display_config.enable)
    return absl::InvalidArgumentError("Selectable preview requires an explicit WEBRTC port and untiled graph");
  if (!gpu_preview::register_elements())
    return absl::FailedPreconditionError("Native preview gates are unavailable");
  auto session = std::unique_ptr<WebPreviewSession>(new WebPreviewSession(app, *options));
  auto& paint = *session->paint_;
  // The configurator owns the selected game directory; this path is never
  // supplied by a runtime browser command.
  paint.game_dir = game_dir;
  paint.session = identity;
  const char* invalidation = g_getenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  paint.invalidation = invalidation ? invalidation : "";
  paint.program_width = options->program_reference_width;
  paint.program_height = options->program_reference_height;
  paint.source_width = options->source_reference_width;
  paint.source_height = options->source_reference_height;
  paint.analytics_layers = app.config.player_analytics_config.analytics.drawing_layers();
  paint.joint_confidence = app.config.player_analytics_config.analytics.pose.confidence_threshold;
  WebPreviewConfig target;
  target.name = "selectable_web_preview";
  target.stream_id = identity.run + ":" + std::to_string(identity.stage) + ":" + std::to_string(identity.topology);
  target.eof_channel = "program";
  target.surface = {
      options->width,
      options->height,
      options->rate_numerator,
      options->rate_denominator,
      options->surface_pool_frames,
      app.config.hmsticher_config.gpu_id};
  target.conversion_pool_frames = options->conversion_pool_frames;
  target.bitrate = options->bitrate;
  target.idr_interval = options->idr_interval;
  target.h264_profile = options->h264_profile;
  target.encoder_tuning = options->encoder_tuning;
  target.rtp_payload = options->rtp_payload;
  target.pending_frames = options->pending_frames;
  target.pending_events = options->pending_events;
  target.presentation_history = options->presentation_history;
  target.stun_server = selected->webrtc_stun_server ? selected->webrtc_stun_server : "";
  std::string error;
  session->target_ = WebPreviewTarget::Create(
      target,
      [state = session->paint_](GstBuffer* frame, const OwnedSurfaceView& output) {
        return state->Paint(frame, output);
      },
      &error);
  if (!session->target_)
    return absl::FailedPreconditionError("Could not construct selectable preview: " + error);
  // Establish the disconnected draining generation before any media enters.
  // Raw identities require a peer epoch even before the first browser arrives;
  // this also lets the real encoder publish its negotiated RTP caps for offers.
  if (!session->target_->DetachPeer(1))
    return absl::InternalError("Could not initialize the disconnected preview drain");
  session->sink_config_ = selected;
  selected->preview_bin = session->target_->bin();
  selected->preview_endpoint = session->target_->endpoint();
  session->routes_.bind_session(std::move(identity));
  session->routes_.initialize_selection("none", 0);
  session->options_.initial_channel = retained_selection.empty() ? options->initial_channel : retained_selection;
  return session;
}

absl::Status WebPreviewSession::AddRoute(const std::string& channel, RouteTap tap) {
  auto state = std::make_shared<RouteState>();
  state->input = target_->AddRoute(channel);
  if (!state->input)
    return absl::InternalError("Could not allocate shared preview input for " + channel);
  state->ingress = gst_element_factory_make("hmpreviewisolation", (channel + "_web_preview_ingress").c_str());
  state->drain = gst_element_factory_make("hmpreviewisolation", (channel + "_web_preview_drain").c_str());
  auto* queue = tap.kind == RouteTap::Kind::kReplaceDrain
      ? tap.queue
      : gst_element_factory_make("queue", (channel + "_web_preview_queue").c_str());
  std::string error;
  auto attachment = RouteAttachment::Create(tap, {state->ingress, queue, state->drain}, state->input, &error);
  if (!attachment)
    return absl::InternalError("Could not attach " + channel + ": " + error);
  g_object_set(
      queue,
      "max-size-buffers",
      options_.queue_frames,
      "max-size-bytes",
      0U,
      "max-size-time",
      guint64{0},
      "leaky",
      2,
      nullptr);
  for (auto* gate : {state->ingress, state->drain})
    g_object_set(gate, "channel", channel.c_str(), "active", FALSE, "generation", guint64{0}, nullptr);
  gpu_preview::set_isolation_failure_peer(state->drain, state->ingress);
  auto* input = gst_element_get_static_pad(state->ingress, "sink");
  gst_pad_add_probe(
      input,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) noexcept {
        auto& state = **static_cast<std::shared_ptr<RouteState>*>(data);
        auto* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
          GstCaps* caps = nullptr;
          gst_event_parse_caps(event, &caps);
          const auto* structure = caps && gst_caps_get_size(caps) ? gst_caps_get_structure(caps, 0) : nullptr;
          gint width = 0, height = 0;
          if (structure && gst_structure_get_int(structure, "width", &width) &&
              gst_structure_get_int(structure, "height", &height) && width > 0 && height > 0) {
            state.width.store(width, std::memory_order_release);
            state.height.store(height, std::memory_order_release);
          }
        }
        return GST_PAD_PROBE_OK;
      },
      new std::shared_ptr<RouteState>(state),
      +[](gpointer value) { delete static_cast<std::shared_ptr<RouteState>*>(value); });
  gst_object_unref(input);
  auto* target = target_.get();
  if (!attachment->Commit([&] {
        return routes_.register_route(
            channel,
            {
                [state, this](bool active, uint64_t operation) {
                  gpu_preview::set_isolation_active(state->ingress, active && overlays_ready_, operation);
                },
                [state, this](bool active, uint64_t operation) {
                  gpu_preview::set_isolation_active(state->drain, active && overlays_ready_, operation);
                },
                [state, target] {
                  return gpu_preview::isolation_active(state->ingress) && gpu_preview::isolation_active(state->drain) &&
                      target->Active(state->input, state->operation);
                },
                [state, target](uint64_t operation) {
                  state->operation = operation;
                  target->Select(state->input, operation);
                },
                [state, target](uint64_t operation) {
                  state->operation = operation;
                  gpu_preview::set_isolation_generation(state->ingress, operation);
                  gpu_preview::set_isolation_generation(state->drain, operation);
                  return gpu_preview::isolation_active(state->ingress) && gpu_preview::isolation_active(state->drain) &&
                      target->Select(state->input, operation);
                },
                [target](uint64_t operation) { return target->Select(nullptr, operation); },
            });
      }))
    return absl::InternalError("Could not publish shared preview route " + channel);
  channels_.emplace(channel, state);
  paint_->channels.emplace(channel, std::move(state));
  return absl::OkStatus();
}

void WebPreviewSession::SetOverlays(pipeline_internal::PreviewOverlaySelection selection, uint64_t generation) {
  std::atomic_store(
      &paint_->overlays,
      std::make_shared<const pipeline_internal::PreviewOverlayCommand>(
          pipeline_internal::PreviewOverlayCommand{generation, selection}));
}

void WebPreviewSession::RefreshOverlayStatus(const RouteOwner::Session& session) {
  if (!available() || !(session == routes_.session()))
    return;
  paint_->artifacts.Replay(session);
  DrainOverlayStatus();
}

void WebPreviewSession::DrainOverlayStatus() {
  if (stopped_)
    return;
  const auto selected = std::atomic_load(&paint_->overlays);
  const auto event =
      paint_->artifacts.Take(routes_.session(), routes_.operation_generation(), routes_.selected_channel(), *selected);
  if (!event)
    return;
  // Control commands and this timer run on the same main context, so an event
  // cannot overtake the complete route/selection acknowledgment that owns it.
  g_print("HSTREAM_PREVIEW_OVERLAY_ARTIFACT %s\n", WebPreviewOverlayArtifactEvent(*event).c_str());
  std::fflush(stdout);
}

absl::Status WebPreviewSession::Attach(const std::function<bool()>& reconcile_overlays) {
  reconcile_overlays_ = reconcile_overlays;
  auto& output = app_.pipeline.instance_bins[0].sink_bin;
  auto status = AddRoute("program", {RouteTap::Kind::kExistingTee, output.bin, output.tee});
  if (!status.ok())
    return status;
  auto* tracked = gst::SelectTrackedPreviewSource(
      app_.pipeline.dsplaytracker_bin.bin,
      app_.pipeline.common_elements.player_analytics,
      app_.pipeline.common_elements.tracker_bin.bin);
  auto& stitcher = app_.pipeline.hmstitcher_bin;
  if (tracked || stitcher.output_tee) {
    const auto tap = tracked
        ? RouteTap{RouteTap::Kind::kAfterElement, app_.pipeline.pipeline, tracked, nullptr, nullptr, "stitched_web_preview_tee"}
        : RouteTap{RouteTap::Kind::kExistingTee, stitcher.bin, stitcher.output_tee};
    status = AddRoute("stitched", tap);
    if (!status.ok())
      return status;
  }
  auto& sources = app_.pipeline.multi_src_bin;
  if (sources.num_bins > 128)
    return absl::InvalidArgumentError("Selectable preview supports at most 128 sources");
  for (unsigned index = 0; index < sources.num_bins; ++index) {
    auto& source = sources.sub_bins[index];
    if (!source.bin || !source.tee || !source.fakesink_queue || !source.fakesink)
      continue;
    status = AddRoute(
        "source" + std::to_string(index),
        {RouteTap::Kind::kReplaceDrain, source.bin, source.tee, source.fakesink_queue, source.fakesink});
    if (!status.ok())
      return status;
    // The source drain now ends at the shared target, not a per-source sink.
    source.fakesink = nullptr;
  }
  const auto selected = routes_.select_current(
      options_.initial_channel, 1, [this] { overlays_ready_ = reconcile_overlays_ && reconcile_overlays_(); });
  if (selected.status != RouteOwner::Status::kActivated && selected.status != RouteOwner::Status::kDeactivated)
    return absl::FailedPreconditionError("Initial browser route is unavailable: " + selected.reason);
  signaling_ = target_->Signaling({routes_.session(), options_.poll_interval_ms, options_.events_per_batch});
  if (!signaling_ || !start_webrtc_signaling_for_preview(target_->endpoint(), sink_config_, signaling_))
    return absl::InternalError("Could not start selectable preview signaling");
  overlay_status_source_ = g_timeout_add_full(
      G_PRIORITY_DEFAULT,
      options_.poll_interval_ms,
      +[](gpointer data) noexcept -> gboolean {
        try {
          static_cast<WebPreviewSession*>(data)->DrainOverlayStatus();
        } catch (const std::exception& error) {
          g_printerr("Could not publish preview overlay status: %s\n", error.what());
        } catch (...) {
          g_printerr("Could not publish preview overlay status\n");
        }
        return G_SOURCE_CONTINUE;
      },
      this,
      nullptr);
  return absl::OkStatus();
}

void WebPreviewSession::Stop() {
  if (stopped_)
    return;
  stopped_ = true;
  if (overlay_status_source_) {
    g_source_remove(overlay_status_source_);
    overlay_status_source_ = 0;
  }
  if (app_.pipeline.pipeline)
    prepare_webrtc_sinks_for_stop(app_.pipeline.pipeline);
  // Shutdown must close every borrowed gate even after failed rearming or the
  // largest valid operation number. Closing within the current operation is
  // permitted by the target; there is no counter increment to overflow.
  const auto operation = routes_.operation_generation() ? routes_.operation_generation() : 1;
  for (const auto& entry : channels_) {
    gpu_preview::set_isolation_active(entry.second->ingress, false, operation);
    gpu_preview::set_isolation_active(entry.second->drain, false, operation);
  }
  if (target_)
    target_->Select(nullptr, operation);
  if (target_) {
    const auto counters = target_->counters();
    g_print(
        "HSTREAM_PREVIEW_TRANSPORT run=%s topology=%" G_GUINT64_FORMAT " admitted=%" G_GUINT64_FORMAT
        " encoded=%" G_GUINT64_FORMAT " packetized=%" G_GUINT64_FORMAT " presented=%" G_GUINT64_FORMAT
        " rejected-presented=%" G_GUINT64_FORMAT " probe-failures=%" G_GUINT64_FORMAT "\n",
        routes_.session().run.c_str(),
        routes_.session().topology,
        counters.admitted,
        counters.encoded,
        counters.packetized,
        counters.presented,
        counters.rejected_presented,
        counters.probe_failures);
  }
  routes_.forget_routes();
}

WebPreviewSession::~WebPreviewSession() {
  if (overlay_status_source_)
    g_source_remove(overlay_status_source_);
  // No borrowed graph callbacks are used here: the owner's Stop ran before
  // teardown, and partially prepared targets have no registered callbacks.
  routes_.forget_routes();
  signaling_.reset();
  target_.reset();
  if (sink_config_) {
    sink_config_->preview_bin = nullptr;
    sink_config_->preview_endpoint = nullptr;
  }
}

std::string WebPreviewSession::selection() const {
  return routes_.selected_channel().empty() ? "none" : routes_.selected_channel();
}
std::string WebPreviewSession::Capability(const pipeline_internal::PreviewOverlayRuntimeState& overlays, bool seek)
    const {
  return WebPreviewCapability(options_, routes_, overlays, seek);
}
std::string WebPreviewSession::Command(const RouteOwner::Request& request) {
  const auto result = stopped_ ? RouteOwner::Result{RouteOwner::Status::kFailed, "preview-stopped"}
                               : SelectWebPreviewRoute(app_.pipeline.pipeline, routes_, request, [this] {
                                   overlays_ready_ = reconcile_overlays_ && reconcile_overlays_();
                                 });
  return WebPreviewRouteReply(request, result);
}
std::string WebPreviewSession::MutationBlockReason() const {
  return stopped_ ? "preview-stopped" : WebPreviewMutationBlockReason(app_.pipeline.pipeline);
}
} // namespace hm::preview
