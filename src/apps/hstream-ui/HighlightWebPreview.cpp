#include "HighlightWebPreview.h"
#include "hstream/src/apps/apps-common/WebPreviewTarget.h"

#include "hstream/src/apps/apps-common/WebRtcEndpoint.h"
#include "hstream/src/apps/apps-common/WebRtcSignaling.h"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace hm::ui {
struct HighlightWebPreview::Impl {
  std::unique_ptr<hm::preview::WebPreviewTarget> target;
  std::shared_ptr<hm::preview::WebPreviewObserver> observer;
  GstElement* graph{nullptr}; // The reel's stopped-before-destruction owner.
  GMainContext* context{g_main_context_new()};
  GMainLoop* loop{g_main_loop_new(context, FALSE)};
  std::thread signaling;
  bool started{false};
  std::mutex progress_mutex;
  std::chrono::steady_clock::time_point progress;
  uint64_t presented{0};
  uint64_t acknowledged{0};
  unsigned connect_timeout_ms, disconnect_timeout_ms;
  ~Impl() {
    g_main_loop_unref(loop);
    g_main_context_unref(context);
  }
};

HighlightWebPreview::HighlightWebPreview() : impl_(std::make_unique<Impl>()) {}
HighlightWebPreview::~HighlightWebPreview() {
  Stop();
}

std::unique_ptr<HighlightWebPreview> HighlightWebPreview::Create(
    const HighlightWebPreviewConfig& config,
    GstElement* graph,
    GstElement* video,
    GstElement* audio,
    std::string* error) {
  auto fail = [&](const char* message) -> std::unique_ptr<HighlightWebPreview> {
    if (error)
      *error = message;
    return {};
  };
  if (!hm::preview::ValidWebPreviewRun(config.run) || config.port < 1024 || config.port > 65535 || !graph || !video ||
      !audio)
    return fail("Invalid authored preview identity, port or graph");
  for (const char* name : {"dtlsenc", "dtlsdec", "srtpenc", "srtpdec", "nicesrc", "nicesink"}) {
    auto* factory = gst_element_factory_find(name);
    if (!factory) {
      if (error)
        *error = std::string("Authored preview requires GStreamer plugin ") + name;
      return {};
    }
    gst_object_unref(factory);
  }
  const auto& o = config.options;
  auto self = std::unique_ptr<HighlightWebPreview>(new HighlightWebPreview);
  auto& state = *self->impl_;
  state.graph = graph;
  state.connect_timeout_ms = o.authored_connect_timeout_ms;
  state.disconnect_timeout_ms = o.authored_disconnect_timeout_ms;
  state.progress = std::chrono::steady_clock::now();
  hm::preview::WebPreviewConfig target;
  target.name = "highlight_browser_preview";
  target.stream_id = config.run + ":highlight:1";
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
  target.stun_server = config.stun_server;
  state.target = hm::preview::WebPreviewTarget::Create(target, {}, error);
  if (!state.target)
    return {};
  if (!gst_bin_add(GST_BIN(graph), state.target->bin()))
    return fail("Could not add authored preview target");
  auto* input = state.target->AddRoute("program");
  auto* source = gst_element_get_static_pad(video, "src");
  const bool linked = input && source && gst_pad_link(source, input) == GST_PAD_LINK_OK &&
      state.target->Select(input, 1) && state.target->DetachPeer(1);
  if (source)
    gst_object_unref(source);
  if (input)
    gst_object_unref(input);
  if (!linked)
    return fail("Could not attach authored preview video");

  GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
  GstElement* encoder = gst_element_factory_make("opusenc", nullptr);
  GstElement* payloader = gst_element_factory_make("rtpopuspay", nullptr);
  if (!convert || !encoder || !payloader) {
    for (auto* item : {convert, encoder, payloader})
      if (item)
        gst_object_unref(item);
    return fail("Authored preview requires Opus audio encoding");
  }
  gst_bin_add_many(GST_BIN(graph), convert, encoder, payloader, nullptr);
  g_object_set(encoder, "bitrate", o.audio_bitrate, nullptr);
  g_object_set(payloader, "pt", 97u, nullptr); // Shared WebRTC audio payload assignment.
  GstCaps* audio_caps = gst_caps_from_string(
      "application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=(string)2");
  auto* audio_input = hstream::webrtc_endpoint_add_track(state.target->endpoint(), audio_caps);
  auto* audio_ghost = audio_input ? gst_ghost_pad_new("highlight_audio", audio_input) : nullptr;
  if (audio_input)
    gst_object_unref(audio_input);
  if (audio_ghost && !gst_element_add_pad(state.target->bin(), audio_ghost)) {
    gst_object_unref(audio_ghost);
    audio_ghost = nullptr;
  }
  auto* audio_source = gst_element_get_static_pad(payloader, "src");
  const bool audio_linked = gst_element_link_many(audio, convert, encoder, payloader, nullptr) && audio_ghost &&
      audio_source && gst_pad_link(audio_source, audio_ghost) == GST_PAD_LINK_OK;
  if (audio_source)
    gst_object_unref(audio_source);
  gst_caps_unref(audio_caps);
  if (!audio_linked)
    return fail("Could not attach authored preview audio");

  auto signaling = state.target->Signaling({{config.run, 0, 1}, o.poll_interval_ms, o.events_per_batch});
  if (!signaling)
    return fail("Could not prepare authored preview signaling");
  state.observer = signaling->observer;
  std::promise<bool> ready;
  auto result = ready.get_future();
  state.signaling = std::thread([&state, port = config.port, signaling, ready = std::move(ready)]() mutable {
    g_main_context_push_thread_default(state.context);
    const bool started = hm::webrtc::Start(state.target->endpoint(), port, signaling);
    ready.set_value(started);
    if (started)
      g_main_loop_run(state.loop);
    g_main_context_pop_thread_default(state.context);
  });
  state.started = result.get();
  if (!state.started)
    return fail("Could not listen for authored preview signaling");
  return self;
}

void HighlightWebPreview::Stop() {
  auto& state = *impl_;
  if (!state.signaling.joinable())
    return;
  if (!state.started) {
    state.signaling.join();
    return;
  }
  // Use a source instead of invoke(): invoke may run immediately on the caller
  // while the new context has not yet acquired its first iteration.
  GSource* source = g_idle_source_new();
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        auto& state = *static_cast<Impl*>(data);
        hm::webrtc::Stop(state.graph);
        g_main_loop_quit(state.loop);
        return G_SOURCE_REMOVE;
      },
      &state,
      nullptr);
  g_source_attach(source, state.context);
  g_source_unref(source);
  state.signaling.join();
}

hm::preview::TransportCounters HighlightWebPreview::counters() const {
  return impl_->target->counters();
}
bool HighlightWebPreview::CheckPresentation(std::string* error) {
  auto& state = *impl_;
  std::lock_guard<std::mutex> lock(state.progress_mutex);
  const auto now = std::chrono::steady_clock::now();
  const auto count = counters().presented;
  const auto acknowledged = state.observer->acknowledged_batches();
  // Browsers may suspend video-frame callbacks in background tabs while
  // continuing audio and signaling. Keep initial playback gated by an actual
  // presented frame, then accept current-peer batch ACKs only as liveness.
  if (count != state.presented || (state.presented && acknowledged != state.acknowledged)) {
    state.presented = count;
    state.progress = now;
  }
  state.acknowledged = acknowledged;
  if (now - state.progress <=
      std::chrono::milliseconds(state.presented ? state.disconnect_timeout_ms : state.connect_timeout_ms))
    return true;
  if (error)
    *error = state.presented ? "Authored preview stopped receiving browser acknowledgments"
                             : "Authored preview browser did not connect before its deadline";
  return false;
}
} // namespace hm::ui
