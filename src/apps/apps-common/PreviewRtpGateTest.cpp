#include "hstream/src/apps/apps-common/PreviewRtpGate.h"

#include <gst/rtp/gstrtpbuffer.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
struct Fixture {
  std::shared_ptr<hm::preview::TransportMonitor> monitor = std::make_shared<hm::preview::TransportMonitor>(8, 32);
  GstElement* gate = hm::preview::rtp_gate_create("gate", monitor);
  GstPad *input, *output, *source, *sink;
  std::mutex mutex;
  std::condition_variable ready;
  bool blocked{false}, release{false}, replacement_started{false};
  std::uint64_t downstream_peer{1};
  bool downstream_open{false};
  std::vector<std::pair<GstClockTime, std::uint64_t>> delivered;

  Fixture() {
    check(gate, "Gate construction failed");
    gst_object_ref_sink(gate);
    input = gst_element_get_static_pad(gate, "sink");
    output = gst_element_get_static_pad(gate, "src");
    source = gst_pad_new("source", GST_PAD_SRC);
    sink = gst_pad_new("consumer", GST_PAD_SINK);
    g_object_set_data(G_OBJECT(sink), "fixture", this);
    gst_pad_set_event_function(
        sink, +[](GstPad*, GstObject*, GstEvent* event) -> gboolean {
          gst_event_unref(event);
          return TRUE;
        });
    gst_pad_set_chain_function(
        sink, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
          auto* self = static_cast<Fixture*>(g_object_get_data(G_OBJECT(pad), "fixture"));
          std::lock_guard<std::mutex> lock(self->mutex);
          if (self->downstream_open)
            self->delivered.emplace_back(GST_BUFFER_PTS(buffer), self->downstream_peer);
          gst_buffer_unref(buffer);
          return GST_FLOW_OK;
        });
    gst_pad_set_active(source, TRUE);
    gst_pad_set_active(sink, TRUE);
    check(
        gst_pad_link(source, input) == GST_PAD_LINK_OK && gst_pad_link(output, sink) == GST_PAD_LINK_OK,
        "Fixture pad link failed");
    check(gst_element_set_state(gate, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Gate activation failed");
    gst_pad_push_event(source, gst_event_new_stream_start("rtp-fixture"));
    GstCaps* caps =
        gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
    gst_pad_push_event(source, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_pad_push_event(source, gst_event_new_segment(&segment));
    check(
        hm::preview::rtp_gate_begin_transition(gate, 1) &&
            hm::preview::rtp_gate_finish_transition(gate, 1, [] { return true; }) && set_ready(1, true) &&
            monitor->Select("program", 1),
        "Initial peer binding failed");
  }
  ~Fixture() {
    gst_element_set_state(gate, GST_STATE_NULL);
    hm::preview::rtp_gate_stop(gate);
    gst_pad_unlink(source, input);
    gst_pad_unlink(output, sink);
    for (auto* pad : {source, sink, input, output})
      gst_object_unref(pad);
    gst_object_unref(gate);
  }
  bool request_ready(std::uint64_t peer, bool active) {
    return hm::preview::rtp_gate_set_ready(gate, peer, active, [this, peer](bool open) {
      std::lock_guard<std::mutex> lock(mutex);
      if (peer != downstream_peer)
        return false;
      downstream_open = open;
      return true;
    });
  }
  bool set_ready(std::uint64_t peer, bool active) {
    if (!request_ready(peer, active))
      return false;
    if (!active)
      return true;
    return wait_ready(peer);
  }
  bool wait_ready(std::uint64_t peer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!hm::preview::rtp_gate_is_ready(gate, peer)) {
      if (std::chrono::steady_clock::now() >= deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  bool encode(std::uint64_t sequence, bool idr) {
    hm::preview::FrameIdentityMeta frame{};
    frame.channel = const_cast<char*>("program");
    frame.operation = 1;
    frame.sequence = sequence;
    frame.source_pts = sequence * GST_SECOND;
    frame.presentation_pts = sequence * GST_MSECOND;
    check(monitor->Admit(&frame).accepted, "Raw fixture frame was rejected");
    const guint8 bytes[]{0, 0, 0, 1, static_cast<guint8>(idr ? 0x65 : 0x41), 0x80};
    auto* buffer = gst_buffer_new_allocate(nullptr, sizeof(bytes), nullptr);
    gst_buffer_fill(buffer, 0, bytes, sizeof(bytes));
    GST_BUFFER_PTS(buffer) = frame.presentation_pts;
    const bool result = monitor->Encoded(buffer);
    gst_buffer_unref(buffer);
    return result;
  }
  GstFlowReturn push(std::uint64_t sequence) {
    auto* buffer = gst_rtp_buffer_new_allocate(2, 0, 0);
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &rtp);
    gst_rtp_buffer_set_timestamp(&rtp, sequence * 90);
    gst_rtp_buffer_set_ssrc(&rtp, 1234);
    gst_rtp_buffer_set_marker(&rtp, TRUE);
    gst_rtp_buffer_unmap(&rtp);
    GST_BUFFER_PTS(buffer) = sequence * GST_MSECOND;
    return gst_pad_push(source, buffer);
  }
};
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  Fixture fixture;
  check(fixture.encode(1, true), "Initial IDR encode failed");
  const auto probe = gst_pad_add_probe(
      fixture.output,
      GST_PAD_PROBE_TYPE_BUFFER,
      +[](GstPad*, GstPadProbeInfo*, gpointer data) {
        auto& f = *static_cast<Fixture*>(data);
        std::unique_lock<std::mutex> lock(f.mutex);
        f.blocked = true;
        f.ready.notify_all();
        f.ready.wait(lock, [&] { return f.release; });
        return GST_PAD_PROBE_OK;
      },
      &fixture,
      nullptr);
  auto pushing = std::async(std::launch::async, [&] { return fixture.push(1); });
  {
    std::unique_lock<std::mutex> lock(fixture.mutex);
    check(
        fixture.ready.wait_for(lock, std::chrono::seconds(2), [&] { return fixture.blocked; }),
        "Accepted old-peer packet did not reach the held output boundary");
  }
  // This must not wait for the held compressed push. Real callers now NULL/
  // detach the old transport to release its waits before FinishTransition.
  check(hm::preview::rtp_gate_begin_transition(fixture.gate, 2), "Could not close output in flight");
  auto replacing = std::async(std::launch::async, [&] {
    {
      std::lock_guard<std::mutex> lock(fixture.mutex);
      fixture.replacement_started = true;
      fixture.ready.notify_all();
    }
    return hm::preview::rtp_gate_finish_transition(fixture.gate, 2, [&] {
      std::lock_guard<std::mutex> lock(fixture.mutex);
      fixture.downstream_peer = 2;
      return true;
    });
  });
  {
    std::unique_lock<std::mutex> lock(fixture.mutex);
    fixture.ready.wait(lock, [&] { return fixture.replacement_started; });
  }
  const bool waited = replacing.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    fixture.release = true;
    fixture.ready.notify_all();
  }
  check(
      pushing.get() == GST_FLOW_OK && replacing.get() && waited,
      "Peer replacement crossed an admitted packet still in flight");
  gst_pad_remove_probe(fixture.output, probe);
  check(
      fixture.delivered.size() == 1 && fixture.delivered.front().second == 1,
      "Old-peer RTP packet reached the replacement transport");
  check(!fixture.set_ready(1, true), "Stale connection notification opened new peer");
  check(
      fixture.encode(2, true) && fixture.push(2) == GST_FLOW_OK && fixture.delivered.size() == 1,
      "A negotiating peer admitted media");
  check(fixture.set_ready(2, true), "Connection readiness failed");
  check(
      fixture.push(2) == GST_FLOW_OK && fixture.delivered.size() == 1 && !fixture.encode(3, false),
      "Negotiation-period IDR or its dependent delta established recovery");
  check(
      fixture.encode(4, true) && fixture.push(4) == GST_FLOW_OK && fixture.delivered.size() == 2 &&
          fixture.delivered.back().second == 2,
      "Fresh connected-peer IDR was not delivered");
  check(
      fixture.set_ready(2, false) && fixture.encode(5, true) && fixture.set_ready(2, true) &&
          fixture.push(5) == GST_FLOW_OK && fixture.delivered.size() == 2 && fixture.encode(6, true) &&
          fixture.push(6) == GST_FLOW_OK && fixture.delivered.size() == 3,
      "Reconnection did not renew IDR recovery");
  // The consumer models the endpoint valve, not a decoded browser. Hold a
  // dependent delta after RTP admission, close/reopen the same peer, and prove
  // that the endpoint remains closed until this accepted push has returned.
  check(fixture.encode(7, false), "Established-peer delta was not encoded");
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    fixture.blocked = fixture.release = false;
  }
  const auto ready_probe = gst_pad_add_probe(
      fixture.output,
      GST_PAD_PROBE_TYPE_BUFFER,
      +[](GstPad*, GstPadProbeInfo*, gpointer data) {
        auto& f = *static_cast<Fixture*>(data);
        std::unique_lock<std::mutex> lock(f.mutex);
        f.blocked = true;
        f.ready.notify_all();
        f.ready.wait(lock, [&] { return f.release; });
        return GST_PAD_PROBE_OK;
      },
      &fixture,
      nullptr);
  auto delta = std::async(std::launch::async, [&] { return fixture.push(7); });
  {
    std::unique_lock<std::mutex> lock(fixture.mutex);
    check(
        fixture.ready.wait_for(lock, std::chrono::seconds(2), [&] { return fixture.blocked; }),
        "Delta did not reach the post-admission hold");
  }
  check(fixture.request_ready(2, false) && fixture.request_ready(2, true), "Readiness requests blocked/rejected");
  // Give the deferred worker a turn while the admitted packet still owns its
  // flow barrier. Reopening early must fail even when callback order is stable.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    check(!fixture.downstream_open, "Endpoint reopened while an admitted delta remained in flight");
    fixture.release = true;
    fixture.ready.notify_all();
  }
  check(delta.get() == GST_FLOW_OK && fixture.wait_ready(2), "Deferred readiness did not complete after drain");
  gst_pad_remove_probe(fixture.output, ready_probe);
  check(
      fixture.delivered.size() == 3 && !fixture.encode(8, false) && fixture.encode(9, true) &&
          fixture.push(9) == GST_FLOW_OK && fixture.delivered.size() == 4,
      "A held pre-disconnect delta crossed the reopen before a fresh IDR");
  return 0;
}
