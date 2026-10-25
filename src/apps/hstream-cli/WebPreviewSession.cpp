#include "WebPreviewSession.h"

#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/apps/apps-common/PlayerAnalyticsRouting.h"
#include "hstream/src/apps/apps-common/PreviewOverlayPainter.h"
#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"

#include <gstnvdsmeta.h>

#include <atomic>
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
    const gpu_preview::PreviewOverlayOptions options{
        route->first,
        source ? source_width : program_width,
        source ? source_height : program_height,
        route->second->width.load(std::memory_order_acquire),
        route->second->height.load(std::memory_order_acquire),
        false,
        false,
        analytics_layers,
        joint_confidence};
    const auto scene = gpu_preview::BuildPreviewOverlayScene(metadata, options, commands, nullptr);
    auto* pixels = static_cast<uint8_t*>(output.rgba) + output.video.top * output.pitch + output.video.left * 4;
    const auto result = painter.Paint(
        {pixels, output.pitch, output.video.width, output.video.height},
        scene,
        commands,
        {route->first, options.negotiated_width, options.negotiated_height, false},
        output.stream);
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
    const std::string& retained_selection) {
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
                [state](bool active, uint64_t operation) {
                  gpu_preview::set_isolation_active(state->ingress, active, operation);
                },
                [state](bool active, uint64_t operation) {
                  gpu_preview::set_isolation_active(state->drain, active, operation);
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

absl::Status WebPreviewSession::Attach() {
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
  const auto selected = routes_.select_current(options_.initial_channel, 1, {});
  if (selected.status != RouteOwner::Status::kActivated && selected.status != RouteOwner::Status::kDeactivated)
    return absl::FailedPreconditionError("Initial browser route is unavailable: " + selected.reason);
  signaling_ = target_->Signaling({routes_.session(), options_.poll_interval_ms, options_.events_per_batch});
  if (!signaling_ || !start_webrtc_signaling_for_preview(target_->endpoint(), sink_config_, signaling_))
    return absl::InternalError("Could not start selectable preview signaling");
  return absl::OkStatus();
}

void WebPreviewSession::Stop() {
  if (stopped_)
    return;
  stopped_ = true;
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
std::string WebPreviewSession::Capability() const {
  return WebPreviewCapability(options_, routes_);
}
std::string WebPreviewSession::Command(const RouteOwner::Request& request) {
  const auto result =
      stopped_ ? RouteOwner::Result{RouteOwner::Status::kFailed, "preview-stopped"} : routes_.select(request, {});
  return WebPreviewRouteReply(request, result);
}
} // namespace hm::preview
