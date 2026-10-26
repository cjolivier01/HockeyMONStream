#include "WebPreviewObserver.h"
#include "WebRtcEndpoint.h"
#include "deepstream_sinks.h"

#include <gst/rtp/gstrtpbuffer.h>
#include <libsoup/soup.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

// Tests the actual Soup handlers/context drain using synthetic identities.
// There is no video decoder here, and receipts do not establish visible pixels.
GST_DEBUG_CATEGORY(NVDS_APP);
namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::abort();
  }
}
bool pump(const std::function<bool()>& ready, unsigned milliseconds = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  do {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  return ready();
}
guint free_port() {
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  check(socket_fd >= 0, "Could not reserve test port");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Could not bind test port");
  socklen_t length = sizeof(address);
  check(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Could not inspect test port");
  close(socket_fd);
  return ntohs(address.sin_port);
}
struct Client {
  SoupWebsocketConnection* connection{nullptr};
  bool complete{false};
  std::vector<std::string> messages;
  ~Client() {
    if (connection) {
      g_signal_handlers_disconnect_by_data(connection, this);
      g_object_unref(connection);
    }
  }
  void Connect(SoupSession* session, guint port) {
    const auto uri = "http://127.0.0.1:" + std::to_string(port) + "/ws";
    auto* message = soup_message_new("GET", uri.c_str());
    soup_session_websocket_connect_async(
        session,
        message,
        nullptr,
        nullptr,
#if SOUP_MAJOR_VERSION >= 3
        G_PRIORITY_DEFAULT,
#endif
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          auto* self = static_cast<Client*>(data);
          GError* error = nullptr;
          self->connection = soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
          if (error) {
            std::cerr << error->message << '\n';
            g_error_free(error);
          }
          self->complete = true;
          if (self->connection)
            g_signal_connect(
                self->connection,
                "message",
                G_CALLBACK(+[](SoupWebsocketConnection*, gint, GBytes* bytes, gpointer data) {
                  gsize size = 0;
                  auto* text = static_cast<const char*>(g_bytes_get_data(bytes, &size));
                  static_cast<Client*>(data)->messages.emplace_back(text, size);
                }),
                self);
        },
        this);
    g_object_unref(message);
    check(pump([&] { return complete; }) && connection, "WebSocket connection failed");
  }
  void Send(const std::string& message) {
    soup_websocket_connection_send_text(connection, message.c_str());
  }
};
void deliver(hm::preview::TransportMonitor& monitor, std::uint64_t sequence) {
  hm::preview::FrameIdentityMeta identity{};
  identity.channel = const_cast<char*>("program");
  identity.operation = 1;
  identity.sequence = sequence;
  identity.source_pts = 3600 * GST_SECOND + sequence * GST_MSECOND;
  identity.presentation_pts = sequence * GST_MSECOND;
  check(monitor.Admit(&identity).accepted, "Synthetic identity admission failed");
  const guint8 bytes[]{0, 0, 0, 1, 0x65, 0x80};
  auto* encoded = gst_buffer_new_allocate(nullptr, sizeof(bytes), nullptr);
  gst_buffer_fill(encoded, 0, bytes, sizeof(bytes));
  GST_BUFFER_PTS(encoded) = identity.presentation_pts;
  check(monitor.Encoded(encoded), "Synthetic IDR admission failed");
  gst_buffer_unref(encoded);
  auto* packet = gst_rtp_buffer_new_allocate(2, 0, 0);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gst_rtp_buffer_map(packet, GST_MAP_WRITE, &rtp);
  gst_rtp_buffer_set_timestamp(&rtp, sequence * 90);
  gst_rtp_buffer_set_ssrc(&rtp, 456);
  gst_rtp_buffer_set_marker(&rtp, true);
  gst_rtp_buffer_unmap(&rtp);
  GST_BUFFER_PTS(packet) = identity.presentation_pts;
  check(monitor.Rtp(packet), "Synthetic marker admission failed");
  gst_buffer_unref(packet);
}
std::string envelope(const char* type, std::uint64_t peer) {
  return std::string("{\"type\":\"") + type +
      "\",\"version\":1,\"run\":\"signal-test\",\"stage\":0,\"topology\":\"1\",\"peer\":\"" + std::to_string(peer) +
      "\"";
}
std::string receipt(std::uint64_t sequence, std::uint64_t peer) {
  return envelope("preview-presented", peer) + ",\"channel\":\"program\",\"operation\":\"1\",\"sequence\":\"" +
      std::to_string(sequence) + "\",\"rtpTimestamp\":" + std::to_string(sequence * 90) +
      ",\"ssrc\":456,\"observation\":\"video-frame-callback\"}";
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  auto monitor = std::make_shared<hm::preview::TransportMonitor>(4, 8, 8);
  auto observer = std::make_shared<hm::preview::WebPreviewObserver>(
      monitor, hm::preview::WebPreviewObserverConfig{{"signal-test", 0, 1}, 5, 8});
  auto preview = std::make_shared<hm::preview::WebPreviewSignaling>();
  preview->observer = observer;
  preview->initial_peer_generation = 40;
  check(monitor->SetPeer(40), "Could not initialize previously draining target");
  auto* pipeline = gst_pipeline_new("observer-test");
  auto* endpoint = hstream::webrtc_endpoint_create("endpoint", nullptr);
  check(endpoint && gst_bin_add(GST_BIN(pipeline), endpoint), "Could not create test endpoint");
  auto* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
  auto* input = hstream::webrtc_endpoint_add_track(endpoint, caps);
  check(input != nullptr, "Could not add test track");
  gst_object_unref(input);
  gst_caps_unref(caps);
  std::uint64_t peer = 40, last_sequence = 0;
  unsigned replacements = 0, detachments = 0;
  auto retire = [&](std::uint64_t generation) {
    check(generation > peer, "Signaling reused a peer generation");
    if (last_sequence) {
      hm::preview::PacketFrame old{};
      old.peer = peer;
      old.operation = 1;
      old.sequence = last_sequence;
      old.channel = "program";
      old.rtp_timestamp = last_sequence * 90;
      old.ssrc = 456;
      check(!monitor->Presented(old), "Endpoint mutation preceded receipt retirement");
    }
    check(monitor->SetPeer(generation), "Monitor rejected delegated peer generation");
    peer = generation;
  };
  preview->replace_peer = [&](std::uint64_t generation) {
    retire(generation);
    ++replacements;
    return hstream::webrtc_endpoint_reset_peer(endpoint);
  };
  preview->detach_peer = [&](std::uint64_t generation) {
    retire(generation);
    ++detachments;
    hstream::webrtc_endpoint_detach_peer(endpoint);
    return true;
  };
  NvDsSinkEncoderConfig config{};
  config.webrtc_port = free_port();
  check(start_webrtc_signaling_for_preview(endpoint, &config, preview), "Observer signaling startup failed");
  auto* session = soup_session_new();
  Client first;
  first.Connect(session, config.webrtc_port);
  check(
      replacements == 1 && detachments == 1 && peer == 42 && monitor->Select("program", 1),
      "Signaling bypassed delegated target lifecycle");
  deliver(*monitor, ++last_sequence);
  check(pump([&] { return !first.messages.empty(); }), "Context source did not drain frame metadata");
  check(
      first.messages.size() == 1 && first.messages.front().find("preview-frames") != std::string::npos,
      "Unexpected metadata message");
  for (unsigned index = 0; index < 20; ++index) {
    deliver(*monitor, ++last_sequence);
    pump([] { return false; }, 6);
  }
  check(
      first.messages.size() == 1 && observer->dropped_events() >= 40,
      "Unacknowledged client grew the outgoing metadata queue");
  first.Send(envelope("preview-frames-ack", peer) + ",\"batch\":\"1\"}");
  pump([] { return false; }, 20);
  deliver(*monitor, ++last_sequence);
  check(pump([&] { return first.messages.size() == 2; }), "Acknowledgment did not restore one batch of credit");
  first.Send(receipt(last_sequence, peer));
  check(pump([&] { return monitor->counters().presented == 1; }), "Actual WebSocket handler rejected exact receipt");
  deliver(*monitor, ++last_sequence);
  const auto old_peer = peer;
  Client second;
  second.Connect(session, config.webrtc_port);
  check(
      replacements == 2 && detachments == 2 && peer == 44 && !observer->TakePresented(),
      "Replacement did not retire the old context/receipt");
  second.Send(receipt(last_sequence, old_peer));
  pump([] { return false; }, 20);
  check(monitor->counters().presented == 1, "Replacement accepted old-peer presentation");
  deliver(*monitor, ++last_sequence);
  check(pump([&] { return !second.messages.empty(); }), "Replacement observer did not resume");
  second.Send(receipt(last_sequence, peer));
  check(pump([&] { return monitor->counters().presented == 2; }), "Replacement exact receipt failed");
  prepare_webrtc_sinks_for_stop(pipeline);
  check(detachments == 3 && !observer->TakePresented(), "Stop did not retire peer/receipts");
  const auto dropped = observer->dropped_events();
  deliver(*monitor, ++last_sequence);
  pump([] { return false; }, 30);
  check(
      observer->dropped_events() == dropped && !monitor->TakeEvents().empty(),
      "Stopped signaling retained its frame-drain source");
  soup_session_abort(session);
  g_object_unref(session);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
