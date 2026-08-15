#include "hstream/src/apps/apps-common/deepstream_sources.h"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

class BarrierFixture {
 public:
  BarrierFixture() {
    g_mutex_init(&parent_.uri_playlist_barrier_mutex);
    g_cond_init(&parent_.uri_playlist_barrier_cond);
    parent_.num_bins = 2;
    parent_.uri_playlist_next_frame_sequence = 0;
    parent_.uri_playlist_paired_video_end = GST_CLOCK_TIME_NONE;
    parent_.uri_playlist_exact_pairing_enabled = TRUE;

    for (guint source_id = 0; source_id < 2; ++source_id) {
      NvDsSrcBin& source = parent_.sub_bins[source_id];
      source.parent_bin = &parent_;
      source.source_id = source_id;
      source.bin = gst_bin_new(nullptr);
      source.src_elem = gst_element_factory_make("fakesrc", nullptr);
      source.num_uri_list = 1;
      source.uri_list = g_new0(gchar*, 2);
      source.uri_list[0] = g_strdup("file:///barrier-test.mp4");
      source.uri_list_frame_ready_sequence = G_MAXUINT64;
      source.uri_list_released_video_end = GST_CLOCK_TIME_NONE;
      source.uri_list_mux_delivered_sequence = G_MAXUINT64;
    }
  }

  ~BarrierFixture() {
    cancel_uri_playlist_frame_barrier(&parent_);
    for (guint source_id = 0; source_id < 2; ++source_id) {
      NvDsSrcBin& source = parent_.sub_bins[source_id];
      if (source.bin) {
        gst_object_unref(source.bin);
      }
      if (source.src_elem) {
        gst_object_unref(source.src_elem);
      }
      g_strfreev(source.uri_list);
    }
    g_cond_clear(&parent_.uri_playlist_barrier_cond);
    g_mutex_clear(&parent_.uri_playlist_barrier_mutex);
  }

  NvDsSrcParentBin* parent() {
    return &parent_;
  }
  NvDsSrcBin* source(guint source_id) {
    return &parent_.sub_bins[source_id];
  }

  bool waitUntilReady(guint source_id, guint64 sequence, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      g_mutex_lock(&parent_.uri_playlist_barrier_mutex);
      const bool ready = parent_.sub_bins[source_id].uri_list_frame_ready_sequence == sequence;
      g_mutex_unlock(&parent_.uri_playlist_barrier_mutex);
      if (ready) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

 private:
  NvDsSrcParentBin parent_{};
};

bool delayed_peer_outlives_old_timeout() {
  BarrierFixture fixture;
  std::atomic<int> first_result{-1};
  std::thread first(
      [&] { first_result = wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND) ? 1 : 0; });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(2))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    first.join();
    std::cerr << "First camera did not enter the frame barrier\n";
    return false;
  }

  // The removed watchdog aborted at 30 seconds. A genuinely delayed peer must
  // still complete the exact pair after that interval without skipping either
  // frame or marking the playlist terminal.
  std::this_thread::sleep_for(std::chrono::seconds(31));
  const bool second_result = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
  first.join();

  NvDsSrcParentBin* parent = fixture.parent();
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  const bool state_ok = parent->uri_playlist_next_frame_sequence == 1 && !parent->uri_playlist_terminal &&
      !parent->uri_playlist_barrier_failed && !parent->uri_playlist_delivery_aborted;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (first_result != 1 || !second_result || !state_ok) {
    std::cerr << "Delayed peer did not commit one intact camera pair after the old timeout\n";
    return false;
  }
  return true;
}

bool cancellation_wakes_waiter_promptly() {
  BarrierFixture fixture;
  std::atomic<int> wait_result{-1};
  std::thread waiter(
      [&] { wait_result = wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND) ? 1 : 0; });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(2))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    waiter.join();
    std::cerr << "Camera did not enter the cancellable frame barrier\n";
    return false;
  }

  const auto cancelled_at = std::chrono::steady_clock::now();
  cancel_uri_playlist_frame_barrier(fixture.parent());
  waiter.join();
  const auto cancellation_latency = std::chrono::steady_clock::now() - cancelled_at;
  if (wait_result != 0 || cancellation_latency > std::chrono::seconds(1)) {
    std::cerr << "Barrier cancellation did not release the delayed decoder promptly\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  if (!delayed_peer_outlives_old_timeout() || !cancellation_wakes_waiter_promptly()) {
    return 1;
  }
  return 0;
}
