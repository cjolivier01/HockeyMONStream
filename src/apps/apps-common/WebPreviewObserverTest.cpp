#include "hstream/src/apps/apps-common/WebPreviewObserver.h"

#include <gst/rtp/gstrtpbuffer.h>

#include <iostream>
#include <stdexcept>

namespace {
using namespace hm::preview;
using Message = WebPreviewObserver::Message;
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
bool deliver(TransportMonitor& monitor, std::uint64_t sequence, std::uint64_t operation = 1) {
  FrameIdentityMeta identity{};
  identity.channel = const_cast<char*>("program");
  identity.operation = operation;
  identity.sequence = sequence;
  identity.presentation_pts = sequence * GST_MSECOND;
  identity.source_pts = 3600 * GST_SECOND + identity.presentation_pts;
  if (!monitor.Admit(&identity).accepted)
    return false;
  const guint8 bytes[]{0, 0, 0, 1, 0x65, 0x80};
  auto* encoded = gst_buffer_new_allocate(nullptr, sizeof(bytes), nullptr);
  gst_buffer_fill(encoded, 0, bytes, sizeof(bytes));
  GST_BUFFER_PTS(encoded) = identity.presentation_pts;
  bool result = monitor.Encoded(encoded);
  gst_buffer_unref(encoded);
  auto* packet = gst_rtp_buffer_new_allocate(2, 0, 0);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gst_rtp_buffer_map(packet, GST_MAP_WRITE, &rtp);
  gst_rtp_buffer_set_timestamp(&rtp, static_cast<std::uint32_t>(sequence * 90));
  gst_rtp_buffer_set_ssrc(&rtp, 456);
  gst_rtp_buffer_set_marker(&rtp, true);
  gst_rtp_buffer_unmap(&rtp);
  GST_BUFFER_PTS(packet) = identity.presentation_pts;
  result &= monitor.Rtp(packet);
  gst_buffer_unref(packet);
  return result;
}
Message handle(WebPreviewObserver& observer, const std::string& json) {
  auto* parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json.c_str(), json.size(), nullptr)) {
    g_object_unref(parser);
    return Message::kRejected;
  }
  auto result = observer.Handle(json_node_get_object(json_parser_get_root(parser)));
  g_object_unref(parser);
  return result;
}
std::string base(const char* type, std::uint64_t peer = 1) {
  return std::string("{\"type\":\"") + type +
      "\",\"version\":1,\"run\":\"test-run\",\"stage\":0,\"topology\":\"9007199254740993\",\"peer\":\"" +
      std::to_string(peer) + "\"";
}
std::string receipt(std::uint64_t sequence, std::uint64_t peer = 1, std::uint64_t operation = 1) {
  return base("preview-presented", peer) + ",\"channel\":\"program\",\"operation\":\"" + std::to_string(operation) +
      "\",\"sequence\":\"" + std::to_string(sequence) +
      "\",\"observation\":\"video-frame-callback\",\"rtpTimestamp\":" +
      std::to_string(static_cast<std::uint32_t>(sequence * 90)) + ",\"ssrc\":456}";
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool passed = true;
  auto monitor = std::make_shared<TransportMonitor>(4, 8, 3);
  WebPreviewObserverConfig config{{"test-run", 0, 9007199254740993ULL}, 10, 2};
  try {
    WebPreviewObserver invalid(monitor, {{"", 0, 0}, 0, 0});
    passed &= expect(false, "Unbounded/unidentified observer admitted");
  } catch (const std::invalid_argument&) {
  }
  WebPreviewObserver observer(monitor, config);
  passed &= expect(
      observer.Retire(1) && monitor->SetPeer(1) && observer.Activate(1) && monitor->Select("program", 1),
      "Initial observer binding failed");
  passed &= expect(deliver(*monitor, 1), "Initial encoded frame failed");
  auto batch = observer.Poll();
  passed &= expect(
      batch && batch->find("9007199254740993") != std::string::npos && batch->find("packetized") != std::string::npos &&
          batch->find("encoded") != std::string::npos,
      "Batch lost full-width session or sender boundaries");
  passed &= expect(monitor->counters().presented == 0, "Sender events falsely declared presentation");
  auto wrong = receipt(1);
  wrong.replace(wrong.find("456"), 3, "457");
  passed &= expect(
      handle(observer, wrong) == Message::kRejected && handle(observer, receipt(1, 2)) == Message::kRejected,
      "Wrong SSRC/peer receipt accepted");
  passed &= expect(
      handle(observer, receipt(1)) == Message::kPresented && handle(observer, receipt(1)) == Message::kRejected,
      "Marker retirement lost receipt history or accepted replay");
  auto presented = observer.TakePresented();
  passed &= expect(
      presented && presented->source_pts == 3600 * GST_SECOND + GST_MSECOND && !observer.TakePresented(),
      "Presented receipt lost native source identity or was consumed twice");
  for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
    passed &= expect(deliver(*monitor, sequence) && !observer.Poll(), "Slow metadata reader grew outgoing backlog");
  }
  passed &= expect(
      observer.dropped_events() == 16 && monitor->counters().history_evicted == 5 &&
          handle(observer, receipt(2)) == Message::kRejected && handle(observer, receipt(9)) == Message::kPresented,
      "Bounded history did not evict old frames or retain latest receipt");
  passed &= expect(
      handle(observer, base("preview-frames-ack") + ",\"batch\":\"2\"}") == Message::kRejected &&
          handle(observer, base("preview-frames-ack") + ",\"batch\":\"1\"}") == Message::kAcknowledged &&
          handle(observer, base("preview-frames-ack") + ",\"batch\":\"1\"}") == Message::kRejected,
      "Batch acknowledgment admitted stale/duplicate credit");
  passed &= expect(deliver(*monitor, 10) && deliver(*monitor, 11) && deliver(*monitor, 12), "Burst failed");
  batch = observer.Poll();
  passed &= expect(
      batch && batch->find("\"sequence\":\"12\"") != std::string::npos &&
          batch->find("\"sequence\":\"10\"") == std::string::npos,
      "Frame batch budget did not retain latest events");
  passed &= expect(
      observer.Retire(2) && !observer.Retire(1) && !observer.Poll() &&
          handle(observer, receipt(12)) == Message::kRejected && !observer.TakePresented(),
      "Peer retirement preserved events or receipt before endpoint replacement");
  passed &= expect(
      monitor->SetPeer(2) && observer.Activate(2) && !observer.Activate(1) && deliver(*monitor, 13),
      "Replacement observer could not admit fresh IDR");
  monitor->Recover();
  passed &= expect(handle(observer, receipt(13, 2)) == Message::kRejected, "Reconnect retained pre-recovery history");
  passed &= expect(
      deliver(*monitor, 14) && monitor->Select("", 2) && handle(observer, receipt(14, 2)) == Message::kRejected,
      "Render off retained receipt history");
  passed &= expect(monitor->Select("program", 3) && deliver(*monitor, 15, 3), "New route operation failed");
  for (auto field : {"run", "topology", "stage", "operation", "sequence", "channel", "observation"}) {
    auto invalid = receipt(15, 2, 3);
    const auto start = invalid.find(std::string("\"") + field + "\":") + strlen(field) + 3;
    const auto end = invalid.find_first_of(",}", start);
    invalid.replace(start, end - start, "null");
    passed &= expect(handle(observer, invalid) == Message::kRejected, "Malformed receipt field accepted");
  }
  auto extra = receipt(15, 2, 3);
  extra.insert(extra.size() - 1, ",\"extra\":true");
  passed &= expect(
      handle(observer, extra) == Message::kRejected && handle(observer, receipt(15, 2, 3)) == Message::kPresented,
      "Strict receipt schema or final presentation failed");
  // RTP wrap has no identity ambiguity: sequence and full generation are exact.
  monitor->Recover();
  passed &= expect(!observer.TakePresented(), "Recovery retained a previously accepted pending observation");
  const std::uint64_t wrapped = 47721859;
  passed &= expect(
      deliver(*monitor, wrapped, 3) && handle(observer, receipt(wrapped, 2, 3)) == Message::kPresented,
      "RTP timestamp wrap lost exact identity");
  return passed ? 0 : 1;
}
