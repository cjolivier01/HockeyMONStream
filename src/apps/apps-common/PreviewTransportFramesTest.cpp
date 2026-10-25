#include "hstream/src/apps/apps-common/PreviewTransportFrames.h"

#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
hm::preview::FrameIdentityMeta frame(const char* channel, std::uint64_t operation, std::uint64_t sequence) {
  hm::preview::FrameIdentityMeta value{};
  value.channel = const_cast<char*>(channel);
  value.operation = operation;
  value.sequence = sequence;
  value.source_pts = (1000 - sequence) * GST_SECOND; // Source time deliberately moves backward.
  value.presentation_pts = sequence * GST_MSECOND;
  return value;
}
} // namespace

int main() {
  hm::preview::TransportFrames ledger(3);
  bool passed = expect(!ledger.Admit(frame("program", 1, 1)).accepted, "Admitted an unbound peer");
  const std::uint8_t idr[]{0, 0, 0, 1, 0x67, 0x12, 0, 0, 1, 0x65, 0x99};
  const std::uint8_t ordinary_intra[]{0, 0, 1, 0x41, 0x12, 0, 0, 0, 1, 0x06, 0x99};
  passed &= expect(
      hm::preview::H264AccessUnitHasIdr(idr, sizeof(idr)) &&
          !hm::preview::H264AccessUnitHasIdr(ordinary_intra, sizeof(ordinary_intra)) &&
          !hm::preview::H264AccessUnitHasIdr(idr, 4) && !hm::preview::H264AccessUnitHasIdr(nullptr, 1),
      "Access-unit IDR admission accepted a non-IDR or truncated packet");
  passed &= expect(ledger.SetPeer(1) && !ledger.SetPeer(1) && !ledger.SetPeer(0), "Peer generations were not fenced");
  auto admission = ledger.Admit(frame("program", 1, 1));
  passed &= expect(admission.accepted && admission.force_keyframe, "First peer frame did not request an IDR");
  passed &= expect(!ledger.Encoded(GST_MSECOND, false), "Initial delta frame was admitted");
  passed &= expect(!ledger.Rtp(GST_MSECOND, 123, 456, true).accepted, "Rejected delta frame reached RTP");
  admission = ledger.Admit(frame("program", 1, 2));
  passed &= expect(admission.accepted && admission.force_keyframe, "Missing IDR was not retried");
  auto encoded = ledger.Encoded(2 * GST_MSECOND, true);
  passed &= expect(
      encoded && encoded->peer == 1 && encoded->source_pts == 998 * GST_SECOND,
      "Encoded identity lost the independent source timeline");
  passed &= expect(!ledger.Encoded(2 * GST_MSECOND, true), "Duplicate encoded notification was admitted");
  auto packet = ledger.Rtp(2 * GST_MSECOND, 0xfffffffe, 456, false);
  passed &= expect(
      packet.accepted && !packet.completed && ledger.pending() == 1,
      "RTP identity completed before the access-unit marker");
  packet = ledger.Rtp(2 * GST_MSECOND, 0xfffffffe, 456, true);
  passed &= expect(
      packet.accepted && packet.completed && packet.completed->rtp_timestamp == 0xfffffffe &&
          packet.completed->ssrc == 456 && packet.completed->operation == 1 && ledger.pending() == 0,
      "RTP marker did not publish exact identity");
  passed &=
      expect(!ledger.Rtp(2 * GST_MSECOND, 0xfffffffe, 456, true).accepted, "Duplicate packet marker was admitted");
  admission = ledger.Admit(frame("program", 1, 3));
  passed &= expect(admission.accepted && !admission.force_keyframe, "Stable route requested an unnecessary IDR");
  admission = ledger.Admit(frame("source0", 2, 4));
  passed &= expect(
      admission.accepted && admission.force_keyframe && !ledger.Encoded(3 * GST_MSECOND, true),
      "New route retained an old route's delayed encoding");
  passed &= expect(
      !ledger.Admit(frame("program", 1, 5)).accepted && !ledger.Admit(frame("program", 2, 5)).accepted &&
          !ledger.Admit(frame("source0", 2, 4)).accepted,
      "Stale/conflicting route identity was admitted");
  passed &= expect(ledger.Encoded(4 * GST_MSECOND, true).has_value(), "New route IDR was lost");
  passed &= expect(
      ledger.SetPeer(2) && !ledger.Rtp(4 * GST_MSECOND, 100, 456, true).accepted,
      "Replaced peer retained delayed packets");
  admission = ledger.Admit(frame("source0", 2, 5));
  passed &= expect(admission.accepted && admission.force_keyframe, "New peer did not require an IDR");
  for (std::uint64_t i = 6; i <= 10; ++i)
    passed &= expect(ledger.Admit(frame("source0", 2, i)).accepted, "Bounded backlog rejected a newer frame");
  passed &= expect(ledger.pending() == 3 && !ledger.Encoded(5 * GST_MSECOND, true), "Backlog exceeded its bound");
  passed &= expect(ledger.Encoded(10 * GST_MSECOND, true).has_value(), "Newest bounded frame was lost");
  packet = ledger.Rtp(10 * GST_MSECOND, 2, 789, true);
  passed &= expect(
      packet.accepted && packet.completed && packet.completed->peer == 2 && packet.completed->rtp_timestamp == 2 &&
          ledger.pending() == 0,
      "RTP timestamp rollover or retired missing frames corrupted identity");
  hm::preview::TransportFrames invalid(0);
  invalid.SetPeer(1);
  passed &= expect(!invalid.Admit(frame("program", 1, 1)).accepted, "Missing capacity silently chose a default");

  hm::preview::TransportFrames bounded(1);
  bounded.SetPeer(1);
  bounded.Admit(frame("program", 1, 1));
  passed &= expect(bounded.Encoded(GST_MSECOND, true).has_value(), "Initial IDR was not encoded");
  admission = bounded.Admit(frame("program", 1, 2));
  passed &= expect(
      admission.accepted && admission.force_keyframe && !bounded.Rtp(GST_MSECOND, 100, 456, true).accepted &&
          !bounded.Encoded(2 * GST_MSECOND, false),
      "Overflow before the initial IDR marker admitted an undecodable delta");
  admission = bounded.Admit(frame("program", 1, 3));
  passed &= expect(
      admission.force_keyframe && bounded.Encoded(3 * GST_MSECOND, true).has_value() &&
          bounded.Rtp(3 * GST_MSECOND, 300, 456, true).accepted,
      "Recovery IDR did not reach the RTP boundary");
  admission = bounded.Admit(frame("program", 1, 4));
  passed &= expect(
      !admission.force_keyframe && bounded.Encoded(4 * GST_MSECOND, false).has_value(),
      "Delivered IDR did not enable its following delta");
  admission = bounded.Admit(frame("program", 1, 5));
  passed &= expect(
      admission.force_keyframe && !bounded.Rtp(4 * GST_MSECOND, 400, 456, true).accepted &&
          !bounded.Encoded(5 * GST_MSECOND, false),
      "Overflow after a delivered IDR admitted a delta referencing a dropped picture");
  admission = bounded.Admit(frame("program", 1, 6));
  passed &= expect(
      admission.force_keyframe && bounded.Encoded(6 * GST_MSECOND, true).has_value() &&
          bounded.Rtp(6 * GST_MSECOND, 600, 456, true).accepted,
      "Reference-loss recovery did not admit a new IDR");
  hm::preview::TransportFrames missing_packet(8);
  missing_packet.SetPeer(1);
  missing_packet.Admit(frame("program", 1, 1));
  missing_packet.Encoded(GST_MSECOND, true);
  missing_packet.Rtp(GST_MSECOND, 100, 456, true);
  for (std::uint64_t i = 2; i <= 3; ++i) {
    missing_packet.Admit(frame("program", 1, i));
    passed &= expect(missing_packet.Encoded(i * GST_MSECOND, false).has_value(), "Delta was not encoded");
  }
  passed &= expect(
      !missing_packet.Rtp(3 * GST_MSECOND, 300, 456, true).accepted &&
          missing_packet.Admit(frame("program", 1, 4)).force_keyframe &&
          missing_packet.Encoded(4 * GST_MSECOND, true).has_value() &&
          missing_packet.Rtp(4 * GST_MSECOND, 400, 456, true).accepted,
      "Missing earlier encoded access unit did not require an IDR at the RTP boundary");
  return passed ? 0 : 1;
}
