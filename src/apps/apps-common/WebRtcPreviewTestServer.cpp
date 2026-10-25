#include "PreviewRtpGate.h"
#include "WebPreviewObserver.h"
#include "WebRtcEndpoint.h"
#include "deepstream_sinks.h"

#include <gst/gst.h>
#include <gst/video/video-event.h>

#include <charconv>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

namespace {
using namespace hm::preview;

template <class Integer>
bool parse_number(const std::string& text, Integer* value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// This CPU fixture mirrors the target's managed-readiness barrier. It does not
// register a production route or substitute software encoding for the target.
struct ReadyDestination {
  GWeakRef endpoint, peer;
  std::uint64_t revision;
  ReadyDestination(GstElement* endpoint, GObject* peer, std::uint64_t revision) : revision(revision) {
    g_weak_ref_init(&this->endpoint, endpoint);
    g_weak_ref_init(&this->peer, peer);
  }
  ~ReadyDestination() {
    g_weak_ref_clear(&endpoint);
    g_weak_ref_clear(&peer);
  }
  bool Apply(bool ready) {
    auto* destination = GST_ELEMENT(g_weak_ref_get(&endpoint));
    auto* current = GST_ELEMENT(g_weak_ref_get(&peer));
    const bool result =
        destination && current && hstream::webrtc_endpoint_apply_readiness(destination, current, revision, ready);
    if (current)
      gst_object_unref(current);
    if (destination)
      gst_object_unref(destination);
    return result;
  }
};
struct PeerGate {
  GstElement* gate;
  GWeakRef endpoint;
  std::uint64_t generation;
  PeerGate(GstElement* gate, GstElement* endpoint, std::uint64_t generation)
      : gate(GST_ELEMENT(gst_object_ref(gate))), generation(generation) {
    g_weak_ref_init(&this->endpoint, endpoint);
  }
  ~PeerGate() {
    g_weak_ref_clear(&endpoint);
    gst_object_unref(gate);
  }
};
void peer_connection(GObject* peer, GParamSpec*, gpointer data) {
  auto* binding = static_cast<PeerGate*>(data);
  auto* endpoint = GST_ELEMENT(g_weak_ref_get(&binding->endpoint));
  if (!endpoint)
    return;
  std::uint64_t revision;
  bool connected;
  if (hstream::webrtc_endpoint_connection_snapshot(endpoint, GST_ELEMENT(peer), &revision, &connected)) {
    auto destination = std::make_shared<ReadyDestination>(endpoint, peer, revision);
    rtp_gate_set_ready(
        binding->gate, binding->generation, connected, [destination](bool ready) { return destination->Apply(ready); });
  }
  gst_object_unref(endpoint);
}

struct PresentationFixture {
  std::shared_ptr<TransportMonitor> monitor = std::make_shared<TransportMonitor>(64, 128, 256);
  std::shared_ptr<WebPreviewSignaling> signaling;
  RouteOwner::Session session;
  GstElement *gate{nullptr}, *endpoint{nullptr}; // Owned by the stopped-before-destruction pipeline.
  std::mutex identity_mutex;
  std::string channel{"program"};
  std::uint64_t operation{1}, sequence{0};

  bool Detach(std::uint64_t peer) {
    if (!rtp_gate_begin_transition(gate, peer))
      return false;
    hstream::webrtc_endpoint_detach_peer(endpoint);
    return rtp_gate_finish_transition(gate, peer, [] { return true; }) &&
        rtp_gate_set_ready(gate, peer, true, [](bool) { return true; });
  }
  GstElement* Replace(std::uint64_t generation) {
    if (!rtp_gate_begin_transition(gate, generation))
      return nullptr;
    hstream::webrtc_endpoint_detach_peer(endpoint);
    GstElement* peer = nullptr;
    if (!rtp_gate_finish_transition(gate, generation, [&] {
          peer = hstream::webrtc_endpoint_reset_peer(endpoint);
          if (!peer)
            return false;
          g_signal_connect_data(
              peer,
              "notify::connection-state",
              G_CALLBACK(peer_connection),
              new PeerGate(gate, endpoint, generation),
              [](gpointer data, GClosure*) { delete static_cast<PeerGate*>(data); },
              GConnectFlags(0));
          return true;
        })) {
      if (peer)
        gst_object_unref(peer);
      return nullptr;
    }
    PeerGate current(gate, endpoint, generation);
    peer_connection(G_OBJECT(peer), nullptr, &current);
    return peer;
  }
  static GstPadProbeReturn Admit(GstPad* pad, GstPadProbeInfo* info, gpointer data) noexcept {
    auto& self = *static_cast<PresentationFixture*>(data);
    try {
      FrameIdentityMeta identity{};
      std::string channel;
      TransportFrames::Admission admission;
      auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (!buffer)
        return GST_PAD_PROBE_DROP;
      {
        std::lock_guard<std::mutex> lock(self.identity_mutex);
        channel = self.channel;
        identity.channel = channel.data();
        identity.operation = self.operation;
        identity.sequence = ++self.sequence;
        identity.source_pts = GST_BUFFER_PTS(buffer);
        identity.presentation_pts = GST_BUFFER_PTS(buffer);
        admission = self.monitor->Admit(&identity);
      }
      if (!admission.accepted)
        return GST_PAD_PROBE_DROP;
      if (admission.force_keyframe) {
        const bool accepted = gst_pad_send_event(
            pad,
            gst_video_event_new_downstream_force_key_unit(
                identity.presentation_pts,
                GST_CLOCK_TIME_NONE,
                identity.presentation_pts,
                TRUE,
                static_cast<guint>(identity.sequence)));
        self.monitor->KeyframeRequest(accepted);
        if (!accepted)
          return GST_PAD_PROBE_DROP;
      }
      return GST_PAD_PROBE_OK;
    } catch (...) {
      self.monitor->ProbeFailure();
      return GST_PAD_PROBE_DROP;
    }
  }
  static GstPadProbeReturn Encoded(GstPad*, GstPadProbeInfo* info, gpointer data) noexcept {
    auto& self = *static_cast<PresentationFixture*>(data);
    try {
      return self.monitor->Encoded(GST_PAD_PROBE_INFO_BUFFER(info)) ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
    } catch (...) {
      self.monitor->ProbeFailure();
      return GST_PAD_PROBE_DROP;
    }
  }
  void ReportPresented() {
    const auto receipt = signaling->observer->TakePresented();
    if (!receipt)
      return;
    auto* builder = json_builder_new();
    json_builder_begin_object(builder);
    const auto text = [&](const char* name, const std::string& value) {
      json_builder_set_member_name(builder, name);
      json_builder_add_string_value(builder, value.c_str());
    };
    const auto number = [&](const char* name, gint64 value) {
      json_builder_set_member_name(builder, name);
      json_builder_add_int_value(builder, value);
    };
    number("version", 1);
    text("run", session.run);
    number("stage", session.stage);
    text("topology", std::to_string(session.topology));
    text("peer", std::to_string(receipt->peer));
    text("channel", receipt->channel);
    text("operation", std::to_string(receipt->operation));
    text("sequence", std::to_string(receipt->sequence));
    text("sourcePts", std::to_string(receipt->source_pts));
    text("presentationPts", std::to_string(receipt->presentation_pts));
    number("rtpTimestamp", receipt->rtp_timestamp);
    number("ssrc", receipt->ssrc);
    json_builder_end_object(builder);
    auto* root = json_builder_get_root(builder);
    auto* generator = json_generator_new();
    json_generator_set_root(generator, root);
    auto* serialized = json_generator_to_data(generator, nullptr);
    g_print("WEBRTC_TEST_PRESENTED %s\n", serialized);
    g_free(serialized);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
  }
  bool Command(const std::string& command) {
    if (command == "quit" || command == "q")
      return false;
    std::lock_guard<std::mutex> lock(identity_mutex);
    if (command == "recover") {
      monitor->Recover();
      g_print("WEBRTC_TEST_RECOVERED\n");
    } else if (command == "off" || command == "route program" || command == "route stitched") {
      channel = command == "off" ? "" : command.substr(6);
      monitor->Select(channel, ++operation);
      g_print(
          "WEBRTC_TEST_ROUTE {\"channel\":\"%s\",\"operation\":\"%llu\"}\n",
          channel.c_str(),
          static_cast<unsigned long long>(operation));
    } else {
      g_printerr("Unknown presentation fixture command\n");
    }
    return true;
  }
};
} // namespace

// CPU-only signaling fixture for browser decode/reconnect acceptance.
GST_DEBUG_CATEGORY(NVDS_APP);
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool presentation = false;
  bool have_run = false, have_stage = false, have_topology = false;
  guint port = 18080;
  PresentationFixture fixture;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--presentation")
      presentation = true;
    else if (argument == "--run" && index + 1 < argc) {
      fixture.session.run = argv[++index];
      have_run = true;
    } else if (argument == "--stage" && index + 1 < argc)
      have_stage = parse_number(std::string(argv[++index]), &fixture.session.stage);
    else if (argument == "--topology" && index + 1 < argc)
      have_topology = parse_number(std::string(argv[++index]), &fixture.session.topology);
    else if (index == 1 && parse_number(argument, &port) && port && port <= 65535) {
    } else {
      g_printerr("Usage: %s [PORT] [--presentation --run UUID --stage INTEGER --topology DECIMAL]\n", argv[0]);
      return 1;
    }
  }
  if (presentation && (!have_run || !have_stage || !have_topology || !fixture.session.topology)) {
    g_printerr("Presentation fixture requires an explicit valid run/stage/topology descriptor\n");
    return 1;
  }
  GError* error = nullptr;
  const char* presentation_graph =
      "videotestsrc is-live=true ! video/x-raw,width=320,height=180,framerate=15/1 ! "
      "videoconvert ! openh264enc name=encoder complexity=low gop-size=15 ! h264parse name=encoded ! "
      "video/x-h264,stream-format=byte-stream,alignment=au ! rtph264pay config-interval=-1 pt=96 ! "
      "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1 ! "
      "identity name=video";
  GstElement* pipeline = gst_parse_launch(
      presentation
          ? presentation_graph
          : "videotestsrc is-live=true ! video/x-raw,width=320,height=180,framerate=15/1 ! "
            "videoconvert ! openh264enc complexity=low gop-size=15 ! h264parse ! rtph264pay config-interval=-1 pt=96 ! "
            "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1 ! identity name=video "
            "audiotestsrc is-live=true ! audioconvert ! opusenc ! rtpopuspay pt=97 ! identity name=audio",
      &error);
  if (!pipeline || error) {
    g_printerr("%s\n", error ? error->message : "pipeline unavailable");
    return 1;
  }
  GstElement* endpoint = hstream::webrtc_endpoint_create("preview", nullptr);
  if (!endpoint || !gst_bin_add(GST_BIN(pipeline), endpoint))
    return 2;
  if (presentation) {
    fixture.endpoint = endpoint;
    fixture.gate = rtp_gate_create("presentation-rtp-gate", fixture.monitor);
    if (!fixture.gate || !gst_bin_add(GST_BIN(pipeline), fixture.gate) ||
        !hstream::webrtc_endpoint_manage_readiness(endpoint) || !fixture.monitor->Select("program", 1) ||
        !fixture.Detach(1))
      return 2;
    for (const auto& probe : {std::make_pair("encoder", "sink"), std::make_pair("encoded", "src")}) {
      auto* element = gst_bin_get_by_name(GST_BIN(pipeline), probe.first);
      auto* pad = element ? gst_element_get_static_pad(element, probe.second) : nullptr;
      if (!pad)
        return 2;
      gst_pad_add_probe(
          pad,
          GST_PAD_PROBE_TYPE_BUFFER,
          std::string(probe.first) == "encoder" ? PresentationFixture::Admit : PresentationFixture::Encoded,
          &fixture,
          nullptr);
      gst_object_unref(pad);
      gst_object_unref(element);
    }
  }
  for (const char* name : {"video", "audio"}) {
    if (presentation && std::string(name) == "audio")
      continue;
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), name);
    GstPad* src = gst_element_get_static_pad(source, "src");
    GstCaps* caps = gst_caps_from_string(
        std::string(name) == "video"
            ? "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1,profile-level-id=(string)42e01f"
            : "application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=(string)2");
    GstPad* sink = hstream::webrtc_endpoint_add_track(endpoint, caps);
    if (!sink)
      return 2;
    if (presentation) {
      auto* input = gst_element_get_static_pad(fixture.gate, "sink");
      auto* output = gst_element_get_static_pad(fixture.gate, "src");
      const bool linked = gst_pad_link(src, input) == GST_PAD_LINK_OK && gst_pad_link(output, sink) == GST_PAD_LINK_OK;
      gst_object_unref(input);
      gst_object_unref(output);
      if (!linked)
        return 2;
    } else if (gst_pad_link(src, sink) != GST_PAD_LINK_OK)
      return 2;
    gst_object_unref(sink);
    gst_caps_unref(caps);
    gst_object_unref(src);
    gst_object_unref(source);
  }
  NvDsSinkEncoderConfig config{};
  config.webrtc_port = port;
  if (presentation) {
    fixture.signaling = std::make_shared<WebPreviewSignaling>();
    try {
      fixture.signaling->observer =
          std::make_shared<WebPreviewObserver>(fixture.monitor, WebPreviewObserverConfig{fixture.session, 10, 64});
    } catch (const std::exception& exception) {
      g_printerr("%s\n", exception.what());
      return 3;
    }
    fixture.signaling->initial_peer_generation = 1;
    fixture.signaling->replace_peer = [&](std::uint64_t peer) { return fixture.Replace(peer); };
    fixture.signaling->detach_peer = [&](std::uint64_t peer) { return fixture.Detach(peer); };
    if (!start_webrtc_signaling_for_preview(endpoint, &config, fixture.signaling))
      return 3;
  } else if (!start_webrtc_signaling_for_sink(endpoint, &config))
    return 3;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return 4;
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  GstBus* bus = gst_element_get_bus(pipeline);
  const guint bus_watch = gst_bus_add_watch(
      bus,
      [](GstBus*, GstMessage* message, gpointer) -> gboolean {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
          GError* error = nullptr;
          gchar* debug = nullptr;
          if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            gst_message_parse_error(message, &error, &debug);
          else
            gst_message_parse_warning(message, &error, &debug);
          g_printerr(
              "%s %s: %s (%s)\n",
              GST_MESSAGE_TYPE_NAME(message),
              GST_OBJECT_NAME(message->src),
              error ? error->message : "",
              debug ? debug : "");
          g_clear_error(&error);
          g_free(debug);
        }
        return G_SOURCE_CONTINUE;
      },
      nullptr);
  gst_object_unref(bus);
  GIOChannel* input = g_io_channel_unix_new(0);
  struct InputContext {
    GMainLoop* loop;
    PresentationFixture* fixture;
    bool finished{false};
  } input_context{loop, presentation ? &fixture : nullptr};
  g_io_channel_set_flags(input, static_cast<GIOFlags>(g_io_channel_get_flags(input) | G_IO_FLAG_NONBLOCK), nullptr);
  const guint input_watch = g_io_add_watch(
      input,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      [](GIOChannel* input, GIOCondition condition, gpointer data) -> gboolean {
        auto& context = *static_cast<InputContext*>(data);
        bool keep = context.fixture && !(condition & (G_IO_HUP | G_IO_ERR));
        if (keep) {
          gchar* line = nullptr;
          gsize terminator = 0;
          const auto status = g_io_channel_read_line(input, &line, nullptr, &terminator, nullptr);
          if (status == G_IO_STATUS_NORMAL) {
            keep = context.fixture->Command(std::string(line, terminator));
            g_free(line);
          } else if (status != G_IO_STATUS_AGAIN)
            keep = false;
        }
        if (!keep) {
          context.finished = true;
          g_main_loop_quit(context.loop);
          return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
      },
      &input_context);
  const guint receipt_watch = presentation ? g_timeout_add(
                                                 10,
                                                 [](gpointer data) -> gboolean {
                                                   static_cast<PresentationFixture*>(data)->ReportPresented();
                                                   return G_SOURCE_CONTINUE;
                                                 },
                                                 &fixture)
                                           : 0;
  g_print("WEBRTC_TEST_READY port=%u\n", config.webrtc_port);
  g_main_loop_run(loop);
  if (receipt_watch)
    g_source_remove(receipt_watch);
  if (!input_context.finished)
    g_source_remove(input_watch);
  g_source_remove(bus_watch);
  prepare_webrtc_sinks_for_stop(pipeline);
  gst_element_send_event(pipeline, gst_event_new_eos());
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (presentation)
    rtp_gate_stop(fixture.gate);
  gst_object_unref(pipeline);
  g_main_loop_unref(loop);
  g_io_channel_unref(input);
  return 0;
}
