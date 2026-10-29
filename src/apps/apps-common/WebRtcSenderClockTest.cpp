#include "WebRtcEndpoint.h"

#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array<guint32, 2> kSsrc{0x13572468, 0x24681357};
constexpr std::array<guint32, 2> kOffset{0x12340000, 0x23450000};
constexpr std::array<guint, 2> kRate{90000, 48000};

void check(bool condition, const char* message) {
  if (!condition) {
    g_printerr("WebRTC sender clock: %s\n", message);
    std::abort();
  }
}
GstClockTime running_time(GstElement* pipeline) {
  GstClock* clock = gst_element_get_clock(pipeline);
  check(clock, "missing pipeline clock");
  const auto now = gst_clock_get_time(clock);
  gst_object_unref(clock);
  const auto base = gst_element_get_base_time(pipeline);
  check(now >= base, "invalid pipeline running time");
  return now - base;
}
struct Report {
  guint32 ssrc, rtp;
  guint64 ntp;
};
struct Packet {
  guint32 ssrc, rtp;
  guint16 sequence;
  GstClockTime pts;
};
struct Capture {
  std::mutex mutex;
  std::vector<Report> reports;
  std::vector<Packet> packets;
};
void read_packet(GstBuffer* buffer, Capture& capture, bool rtcp) {
  std::lock_guard<std::mutex> lock(capture.mutex);
  if (rtcp) {
    GstRTCPBuffer mapped = GST_RTCP_BUFFER_INIT;
    check(gst_rtcp_buffer_map(buffer, GST_MAP_READ, &mapped), "invalid outgoing RTCP");
    GstRTCPPacket packet;
    if (gst_rtcp_buffer_get_first_packet(&mapped, &packet)) {
      do {
        if (gst_rtcp_packet_get_type(&packet) == GST_RTCP_TYPE_SR) {
          Report report{};
          guint32 packets, octets;
          gst_rtcp_packet_sr_get_sender_info(&packet, &report.ssrc, &report.ntp, &report.rtp, &packets, &octets);
          capture.reports.push_back(report);
        }
      } while (gst_rtcp_packet_move_to_next(&packet));
    }
    gst_rtcp_buffer_unmap(&mapped);
  } else {
    GstRTPBuffer mapped = GST_RTP_BUFFER_INIT;
    check(gst_rtp_buffer_map(buffer, GST_MAP_READ, &mapped), "invalid outgoing RTP");
    capture.packets.push_back(
        {gst_rtp_buffer_get_ssrc(&mapped),
         gst_rtp_buffer_get_timestamp(&mapped),
         gst_rtp_buffer_get_seq(&mapped),
         GST_BUFFER_PTS(buffer)});
    gst_rtp_buffer_unmap(&mapped);
  }
}
struct Probe {
  Capture* capture;
  bool rtcp;
};
GstPadProbeReturn capture_packets(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto& probe = *static_cast<Probe*>(data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
    read_packet(GST_PAD_PROBE_INFO_BUFFER(info), *probe.capture, probe.rtcp);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    auto* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    for (guint i = 0; i < gst_buffer_list_length(list); ++i)
      read_packet(gst_buffer_list_get(list, i), *probe.capture, probe.rtcp);
  }
  return GST_PAD_PROBE_OK;
}
GstWebRTCSessionDescription* create_description(GstElement* peer, const char* action, const char* field) {
  GstPromise* promise = gst_promise_new();
  g_signal_emit_by_name(peer, action, nullptr, promise);
  check(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "SDP creation did not reply");
  GstWebRTCSessionDescription* result = nullptr;
  check(
      gst_structure_get(gst_promise_get_reply(promise), field, GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &result, nullptr),
      "SDP creation failed");
  gst_promise_unref(promise);
  return result;
}
void set_description(GstElement* peer, const char* action, GstWebRTCSessionDescription* description) {
  GstPromise* promise = gst_promise_new();
  g_signal_emit_by_name(peer, action, description, promise);
  check(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "SDP installation did not reply");
  const auto* reply = gst_promise_get_reply(promise);
  check(!reply || !gst_structure_has_field(reply, "error"), "SDP installation failed");
  gst_promise_unref(promise);
}
void negotiate(GstElement* endpoint, GstElement* peer, GstElement* receiver) {
  auto candidate = +[](GstElement*, guint index, gchar* text, gpointer other) {
    g_signal_emit_by_name(other, "add-ice-candidate", index, text);
  };
  g_signal_connect(peer, "on-ice-candidate", G_CALLBACK(candidate), receiver);
  g_signal_connect(receiver, "on-ice-candidate", G_CALLBACK(candidate), peer);
  bool ready = false;
  for (int i = 0; i < 200 && !ready; ++i) {
    ready = hstream::webrtc_endpoint_prepare_offer(endpoint);
    if (!ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(ready, "actual audio/video caps did not become ready");
  auto* offer = create_description(peer, "create-offer", "offer");
  check(gst_sdp_message_medias_len(offer->sdp) == 2, "offer lost an audio/video track");
  set_description(receiver, "set-remote-description", offer);
  set_description(peer, "set-local-description", offer);
  auto* answer = create_description(receiver, "create-answer", "answer");
  set_description(peer, "set-remote-description", answer);
  set_description(receiver, "set-local-description", answer);
  gst_webrtc_session_description_free(offer);
  gst_webrtc_session_description_free(answer);
  bool connected = false;
  std::uint64_t revision = 0;
  for (int i = 0; i < 1000 && !connected; ++i) {
    check(hstream::webrtc_endpoint_connection_snapshot(endpoint, peer, &revision, &connected), "peer retired early");
    if (!connected)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(connected, "loopback peer did not connect");
}
void run() {
  GstElement* pipeline = gst_pipeline_new("sender-clock-test");
  GstElement* endpoint = hstream::webrtc_endpoint_create("sender", nullptr);
  gst_bin_add(GST_BIN(pipeline), endpoint);
  std::array<GstElement*, 2> sources{};
  std::array<GstPad*, 2> inputs{};
  const std::array<const char*, 2> caps_text{
      "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1",
      "application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=(string)2"};
  for (size_t i = 0; i < sources.size(); ++i) {
    sources[i] = gst_element_factory_make("appsrc", nullptr);
    auto* caps = gst_caps_from_string(caps_text[i]);
    gst_caps_set_simple(caps, "ssrc", G_TYPE_UINT, kSsrc[i], nullptr);
    inputs[i] = hstream::webrtc_endpoint_add_track(endpoint, caps);
    check(sources[i] && inputs[i], "could not create source/track");
    g_object_set(
        sources[i],
        "caps",
        caps,
        "is-live",
        TRUE,
        "format",
        GST_FORMAT_TIME,
        "min-latency",
        gint64{0},
        "max-latency",
        gint64{0},
        nullptr);
    gst_caps_unref(caps);
    gst_bin_add(GST_BIN(pipeline), sources[i]);
    const auto name = "sink_" + std::to_string(i);
    check(gst_element_link_pads(sources[i], "src", endpoint, name.c_str()), "could not link track");
  }
  check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "pipeline failed to play");
  std::array<guint16, 2> sequence{0, 0};
  std::map<std::pair<guint32, guint16>, Packet> sent;
  const auto push = [&] {
    const auto pts = running_time(pipeline);
    for (size_t i = 0; i < sources.size(); ++i) {
      GstBuffer* buffer = gst_rtp_buffer_new_allocate(3, 0, 0);
      GstRTPBuffer mapped = GST_RTP_BUFFER_INIT;
      check(gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &mapped), "could not create RTP");
      const guint32 timestamp = kOffset[i] + gst_util_uint64_scale(pts, kRate[i], GST_SECOND);
      gst_rtp_buffer_set_payload_type(&mapped, 96 + i);
      gst_rtp_buffer_set_ssrc(&mapped, kSsrc[i]);
      gst_rtp_buffer_set_seq(&mapped, ++sequence[i]);
      gst_rtp_buffer_set_timestamp(&mapped, timestamp);
      gst_rtp_buffer_set_marker(&mapped, TRUE);
      auto* payload = static_cast<guint8*>(gst_rtp_buffer_get_payload(&mapped));
      payload[0] = i ? 0xf8 : 0x65;
      payload[1] = i ? 0xff : 0x80;
      payload[2] = i ? 0xfe : 0x00;
      gst_rtp_buffer_unmap(&mapped);
      GST_BUFFER_PTS(buffer) = pts;
      GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
      sent[{kSsrc[i], sequence[i]}] = {kSsrc[i], timestamp, sequence[i], pts};
      check(gst_app_src_push_buffer(GST_APP_SRC(sources[i]), buffer) == GST_FLOW_OK, "could not push RTP");
    }
  };
  push(); // Establish caps through the initial nonblocking drains.
  for (int generation = 0; generation < 2; ++generation) {
    Capture capture;
    GstElement* receiver = gst_element_factory_make("webrtcbin", nullptr);
    check(receiver, "missing loopback receiver");
    g_object_set(receiver, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
    gst_bin_add(GST_BIN(pipeline), receiver);
    g_signal_connect(
        receiver,
        "pad-added",
        G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer parent) {
          auto* sink = gst_element_factory_make("fakesink", nullptr);
          g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
          gst_bin_add(GST_BIN(parent), sink);
          auto* input = gst_element_get_static_pad(sink, "sink");
          check(gst_pad_link(pad, input) == GST_PAD_LINK_OK, "receiver could not drain RTP");
          gst_object_unref(input);
          check(gst_element_sync_state_with_parent(sink), "receiver drain state failed");
        }),
        pipeline);
    check(gst_element_sync_state_with_parent(receiver), "receiver state failed");
    GstElement* peer = hstream::webrtc_endpoint_reset_peer(endpoint);
    check(peer, "could not create replacement sender peer");
    negotiate(endpoint, peer, receiver);
    // Dynamic sinks normally trigger the application's LATENCY bus handler.
    // This standalone graph has no main loop; distribute its zero input latency
    // explicitly so RTPSession can establish packet running times for SRs.
    check(gst_bin_recalculate_latency(GST_BIN(pipeline)), "could not configure test pipeline latency");
    auto* manager = gst_bin_get_by_name(GST_BIN(peer), "rtpbin");
    check(manager, "missing sender RTP manager");
    GObject* internal = nullptr;
    g_signal_emit_by_name(manager, "get-internal-session", 0, &internal);
    check(internal, "missing bundled audio/video RTP session");
    // Bound this CPU-only packet test without waiting for ordinary 5 s RTCP
    // intervals. Payloads drain without any decoder, GPU or audio device.
    g_object_set(internal, "rtcp-min-interval", guint64{50 * GST_MSECOND}, "bandwidth", 1000000.0, nullptr);
    g_object_unref(internal);
    // The valves retained caps during negotiation. Their first opened push
    // forwards those sticky events and creates the manager's RTP output.
    push();
    GstPad* rtp = nullptr;
    for (int attempt = 0; attempt < 200 && !rtp; ++attempt) {
      rtp = gst_element_get_static_pad(manager, "send_rtp_src_0");
      if (!rtp)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto* rtcp = gst_element_get_static_pad(manager, "send_rtcp_src_0");
    check(rtp && rtcp, "missing generated packet outputs");
    Probe rtp_probe{&capture, false}, rtcp_probe{&capture, true};
    const auto types = GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST);
    const auto rtp_id = gst_pad_add_probe(rtp, types, capture_packets, &rtp_probe, nullptr);
    const auto rtcp_id = gst_pad_add_probe(rtcp, types, capture_packets, &rtcp_probe, nullptr);
    const auto counts = [&] {
      std::array<size_t, 2> result{};
      std::lock_guard<std::mutex> lock(capture.mutex);
      for (const auto& report : capture.reports)
        for (size_t i = 0; i < kSsrc.size(); ++i)
          result[i] += report.ssrc == kSsrc[i];
      return result;
    };
    const auto collect = [&](std::array<size_t, 2> target) {
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      for (;;) {
        const auto have = counts();
        if (have[0] >= target[0] && have[1] >= target[1])
          break;
        check(Clock::now() < deadline, "both audio/video sender reports did not arrive");
        push();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    };
    collect({3, 3});
    check(gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE, "pause failed");
    check(
        gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_FAILURE,
        "pause did not settle");
    const auto before = counts();
    const auto base_before = gst_element_get_base_time(pipeline);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    check(counts() == before, "sender reports continued while paused");
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "resume failed");
    check(gst_element_get_base_time(pipeline) >= base_before + 350 * GST_MSECOND, "test did not shift the pause base");
    collect({before[0] + 3, before[1] + 3});
    gst_pad_remove_probe(rtp, rtp_id);
    gst_pad_remove_probe(rtcp, rtcp_id);
    gst_object_unref(rtp);
    gst_object_unref(rtcp);
    gst_object_unref(manager);
    std::array<std::vector<Report>, 2> reports;
    std::array<size_t, 2> packet_counts{};
    std::vector<Packet> captured_packets;
    std::vector<Report> captured_reports;
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      captured_packets = capture.packets;
      captured_reports = capture.reports;
    }
    for (const auto& packet : captured_packets) {
      const auto found = sent.find({packet.ssrc, packet.sequence});
      check(
          found != sent.end() && found->second.rtp == packet.rtp && found->second.pts == packet.pts,
          "endpoint changed actual RTP identity or presentation timestamp");
      for (size_t i = 0; i < kSsrc.size(); ++i)
        packet_counts[i] += packet.ssrc == kSsrc[i];
    }
    for (const auto& report : captured_reports)
      for (size_t i = 0; i < kSsrc.size(); ++i)
        if (report.ssrc == kSsrc[i])
          reports[i].push_back(report);
    for (size_t i = 0; i < kSsrc.size(); ++i) {
      check(packet_counts[i] > 0 && reports[i].size() >= 6, "missing audio/video packet evidence");
      const auto first = reports[i].front();
      for (const auto& report : reports[i]) {
        check(report.ntp && report.ntp >= first.ntp, "sender NTP was invalid or moved backwards");
        const long double ntp_ms = (report.ntp - first.ntp) * (1000.0L / (1ULL << 32));
        const long double rtp_ms = static_cast<gint32>(report.rtp - first.rtp) * (1000.0L / kRate[i]);
        check(std::abs(ntp_ms - rtp_ms) < 1.0L, "Pause changed the RTP/sender-report clock mapping");
      }
    }
    std::printf(
        "sender generation %d: video/audio packets %zu/%zu, reports %zu/%zu; pause mapping preserved\n",
        generation,
        packet_counts[0],
        packet_counts[1],
        reports[0].size(),
        reports[1].size());
    g_signal_handlers_disconnect_by_data(peer, receiver);
    g_signal_handlers_disconnect_by_data(receiver, peer);
    hstream::webrtc_endpoint_detach_peer(endpoint);
    gst_element_set_state(receiver, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(pipeline), receiver);
    gst_object_unref(peer);
    for (size_t i = 0; i < inputs.size(); ++i) {
      auto* current = gst_element_get_static_pad(endpoint, ("sink_" + std::to_string(i)).c_str());
      check(current == inputs[i], "peer replacement changed a stable audio/video input");
      gst_object_unref(current);
    }
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  for (auto* input : inputs)
    gst_object_unref(input);
  gst_object_unref(pipeline);
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  run();
}
