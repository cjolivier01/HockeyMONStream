#include "hstream/src/apps/apps-common/PreviewTransportMonitor.h"

#include <gst/rtp/gstrtpbuffer.h>

#include <iostream>
#include <stdexcept>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
hm::preview::FrameIdentityMeta identity(const char* channel, std::uint64_t operation, std::uint64_t sequence) {
  hm::preview::FrameIdentityMeta result{};
  result.channel = const_cast<char*>(channel);
  result.operation = operation;
  result.sequence = sequence;
  result.source_pts = 3600 * GST_SECOND + sequence * GST_MSECOND;
  result.presentation_pts = sequence * GST_MSECOND;
  return result;
}
bool encode(hm::preview::TransportMonitor& monitor, std::uint64_t sequence, bool idr) {
  const guint8 bytes[]{0, 0, 0, 1, static_cast<guint8>(idr ? 0x65 : 0x41), 0x80};
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, sizeof(bytes), nullptr);
  gst_buffer_fill(buffer, 0, bytes, sizeof(bytes));
  GST_BUFFER_PTS(buffer) = sequence * GST_MSECOND;
  const bool result = monitor.Encoded(buffer);
  gst_buffer_unref(buffer);
  return result;
}
GstBuffer* rtp_buffer(std::uint64_t sequence, bool marker) {
  GstBuffer* buffer = gst_rtp_buffer_new_allocate(2, 0, 0);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &rtp);
  gst_rtp_buffer_set_timestamp(&rtp, 90000 + sequence * 90);
  gst_rtp_buffer_set_ssrc(&rtp, 456);
  gst_rtp_buffer_set_marker(&rtp, marker);
  gst_rtp_buffer_unmap(&rtp);
  GST_BUFFER_PTS(buffer) = sequence * GST_MSECOND;
  return buffer;
}
bool packet(hm::preview::TransportMonitor& monitor, std::uint64_t sequence, bool marker = true) {
  GstBuffer* buffer = rtp_buffer(sequence, marker);
  const bool result = monitor.Rtp(buffer);
  gst_buffer_unref(buffer);
  return result;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool passed = true;
  try {
    hm::preview::TransportMonitor invalid(0, 2);
    passed &= expect(false, "Zero transport capacity was admitted");
  } catch (const std::invalid_argument&) {
  }
  hm::preview::TransportMonitor monitor(1, 2);
  passed &= expect(monitor.Select("program", 1) && monitor.SetPeer(1), "Initial binding failed");
  auto frame = identity("program", 1, 1);
  passed &= expect(
      monitor.Admit(&frame).force_keyframe && encode(monitor, 1, true) && packet(monitor, 1, false),
      "Initial IDR admission failed");
  passed &= expect(
      monitor.Select("", 2) && !packet(monitor, 1) && !monitor.Admit(&frame).accepted && monitor.TakeEvents().empty(),
      "Render off retained an old marker, raw frame or event");
  passed &= expect(monitor.Select("source0", 3) && !monitor.Select("program", 1), "Route fencing failed");
  frame = identity("source0", 3, 2);
  passed &= expect(
      monitor.Admit(&frame).force_keyframe && encode(monitor, 2, true) && packet(monitor, 2),
      "Replacement route failed to recover");
  auto events = monitor.TakeEvents();
  passed &= expect(
      events.size() == 2 && events[0].kind == hm::preview::TransportEvent::Kind::kEncoded &&
          events[1].kind == hm::preview::TransportEvent::Kind::kPacketized && events[1].frame.peer == 1 &&
          events[1].frame.operation == 3 && events[1].frame.source_pts == 3600 * GST_SECOND + 2 * GST_MSECOND &&
          events[1].frame.presentation_pts == 2 * GST_MSECOND && events[1].rtp_timestamp == 90180 &&
          events[1].ssrc == 456,
      "Sender events lost exact source/presentation/RTP identity");
  frame = identity("source0", 3, 3);
  passed &= expect(
      monitor.Admit(&frame).accepted && encode(monitor, 3, false) && monitor.SetPeer(2) && !monitor.SetPeer(1) &&
          !packet(monitor, 3) && monitor.TakeEvents().empty(),
      "Peer replacement leaked old output/events");
  frame = identity("source0", 3, 4);
  passed &= expect(monitor.Admit(&frame).force_keyframe && encode(monitor, 4, true), "New peer IDR failed");
  frame = identity("source0", 3, 5);
  passed &= expect(
      monitor.Admit(&frame).force_keyframe && !packet(monitor, 4) && !encode(monitor, 5, false),
      "Backlog eviction admitted a missing reference");
  for (std::uint64_t sequence = 6; sequence < 30; ++sequence) {
    frame = identity("source0", 3, sequence);
    passed &= expect(
        monitor.Admit(&frame).accepted && encode(monitor, sequence, sequence == 6) && packet(monitor, sequence),
        "Recovery/continuous compressed stream failed");
  }
  events = monitor.TakeEvents();
  passed &= expect(
      events.size() == 2 && events.back().frame.sequence == 29 && events.back().frame.peer == 2 &&
          monitor.TakeEvents().empty() && monitor.counters().events_dropped >= 46,
      "Unpolled sender events were not bounded or latest identity was lost");
  monitor.KeyframeRequest(false);
  passed &= expect(
      monitor.counters().keyframe_requests == 1 && monitor.counters().keyframe_failures == 1 &&
          monitor.counters().rejected_raw > 0 && monitor.counters().rejected_encoded > 0 &&
          monitor.counters().rejected_rtp > 0,
      "Transport rejection counters lost failed work");
  frame = identity("source0", 3, 30);
  passed &= expect(
      monitor.Admit(&frame).accepted && encode(monitor, 30, false) && !monitor.Rtp(nullptr),
      "Missing compressed packet was admitted");
  frame = identity("source0", 3, 31);
  passed &= expect(
      monitor.Admit(&frame).force_keyframe && !packet(monitor, 30) && !encode(monitor, 31, false),
      "Unreadable RTP did not invalidate the reference chain");
  frame = identity("source0", 3, 32);
  passed &= expect(
      monitor.Admit(&frame).force_keyframe && encode(monitor, 32, true) && packet(monitor, 32),
      "Unreadable RTP recovery failed");
  monitor.ProbeFailure();
  frame = identity("source0", 3, 33);
  passed &= expect(
      monitor.counters().probe_failures == 1 && monitor.Admit(&frame).force_keyframe,
      "Exceptional probe loss did not require a fresh IDR");
  monitor.TakeEvents();
  passed &= expect(encode(monitor, 33, true), "Mixed-list IDR was not encoded");
  GstBufferList* packets = gst_buffer_list_new();
  gst_buffer_list_add(packets, rtp_buffer(33, true));
  gst_buffer_list_add(packets, rtp_buffer(30, true));
  passed &= expect(
      monitor.FilterRtp(packets) && gst_buffer_list_length(packets) == 1 &&
          GST_BUFFER_PTS(gst_buffer_list_get(packets, 0)) == 33 * GST_MSECOND,
      "A stale packet caused an accepted IDR to disappear from a mixed list");
  gst_buffer_list_unref(packets);
  events = monitor.TakeEvents();
  passed &= expect(
      events.size() == 2 && events.back().frame.sequence == 33 &&
          events.back().kind == hm::preview::TransportEvent::Kind::kPacketized,
      "Mixed-list delivery lost its accepted IDR identity");
  return passed ? 0 : 1;
}
