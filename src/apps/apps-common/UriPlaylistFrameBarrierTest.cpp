#include "hstream/src/apps/apps-common/deepstream_sources.h"

#include <gst/gst.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

constexpr const char* kStallBudgetVariable = "HM_URI_PLAYLIST_BARRIER_STALL_SECONDS";

// Shrinks the production stall budget so the wedged-peer cases finish in
// seconds instead of minutes.
class ScopedStallBudget {
 public:
  explicit ScopedStallBudget(const char* seconds) {
    g_setenv(kStallBudgetVariable, seconds, TRUE);
  }
  ScopedStallBudget(const ScopedStallBudget&) = delete;
  ScopedStallBudget& operator=(const ScopedStallBudget&) = delete;
  ~ScopedStallBudget() {
    g_unsetenv(kStallBudgetVariable);
  }
};

class BarrierFixture {
 public:
  BarrierFixture() {
    g_mutex_init(&parent_.uri_playlist_barrier_mutex);
    g_cond_init(&parent_.uri_playlist_barrier_cond);
    parent_.uri_playlist_barrier_initialized = TRUE;
    parent_.num_bins = 2;
    parent_.uri_playlist_next_frame_sequence = 0;
    parent_.uri_playlist_paired_video_end = GST_CLOCK_TIME_NONE;
    parent_.uri_playlist_exact_pairing_enabled = TRUE;

    bus_ = gst_bus_new();
    for (guint source_id = 0; source_id < 2; ++source_id) {
      NvDsSrcBin& source = parent_.sub_bins[source_id];
      source.parent_bin = &parent_;
      source.source_id = source_id;
      source.bin = gst_bin_new(nullptr);
      source.src_elem = gst_element_factory_make("fakesrc", nullptr);
      gst_element_set_bus(source.src_elem, bus_);
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
    gst_object_unref(bus_);
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

  bool hasStreamError() {
    GstMessage* error = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR);
    if (!error) {
      return false;
    }
    GError* parsed = nullptr;
    gst_message_parse_error(error, &parsed, nullptr);
    const bool failed = parsed && parsed->domain == GST_STREAM_ERROR && parsed->code == GST_STREAM_ERROR_FAILED;
    g_clear_error(&parsed);
    gst_message_unref(error);
    return failed;
  }

 private:
  GstBus* bus_ = nullptr;
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

// Reproduces the field deadlock. Camera 1 crossed a GoPro chapter boundary, its
// replacement decodebin never finished preroll, and camera 0 sat in the barrier
// holding decoded frame 25098 while the 30 second diagnostic re-armed forever.
// Nothing reached the bus, so the run froze for half an hour with no error and
// no exit. A peer that makes no progress at all has to be reported.
bool wedged_peer_is_reported_instead_of_hanging() {
  ScopedStallBudget budget("2");
  BarrierFixture fixture;
  const auto started = std::chrono::steady_clock::now();
  auto pending = std::async(std::launch::async, [&fixture] {
    return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND) ? 1 : 0;
  });
  // The peer never arrives, which is exactly how a wedged chapter switch looks
  // from this side of the barrier.
  if (pending.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
    std::cerr << "Wedged peer left the camera barrier waiting indefinitely\n";
    cancel_uri_playlist_frame_barrier(fixture.parent());
    pending.wait();
    return false;
  }
  const auto latency = std::chrono::steady_clock::now() - started;

  NvDsSrcParentBin* parent = fixture.parent();
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  const bool state_ok = parent->uri_playlist_next_frame_sequence == 0 && parent->uri_playlist_terminal &&
      parent->uri_playlist_barrier_failed && parent->uri_playlist_delivery_aborted;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (pending.get() != 0 || !state_ok || !fixture.hasStreamError()) {
    std::cerr << "Wedged peer did not fail the frame barrier\n";
    return false;
  }
  // Giving up before the budget elapses would mean a merely slow peer could be
  // abandoned too.
  if (latency < std::chrono::seconds(2)) {
    std::cerr << "Frame barrier abandoned the peer before the stall budget elapsed\n";
    return false;
  }
  return true;
}

// A wedged playlist can still be noisy: teardown, delivery and cancellation
// paths all broadcast the shared condition, and these waits retry on every
// signal. Deriving the next wake from "now" on each retry would let that noise
// postpone the stall check indefinitely, so the detector keeps an absolute
// deadline and evaluates progress once per interval however often it is woken.
bool signal_noise_does_not_mask_a_wedged_peer() {
  ScopedStallBudget budget("2");
  BarrierFixture fixture;
  std::atomic<bool> stop_signalling{false};
  std::thread noise([&] {
    NvDsSrcParentBin* parent = fixture.parent();
    while (!stop_signalling) {
      g_mutex_lock(&parent->uri_playlist_barrier_mutex);
      g_cond_broadcast(&parent->uri_playlist_barrier_cond);
      g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
  auto pending = std::async(std::launch::async, [&fixture] {
    return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND) ? 1 : 0;
  });
  const bool finished = pending.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
  stop_signalling = true;
  noise.join();
  if (!finished) {
    std::cerr << "Condition signals masked a wedged peer indefinitely\n";
    cancel_uri_playlist_frame_barrier(fixture.parent());
    pending.wait();
    return false;
  }

  NvDsSrcParentBin* parent = fixture.parent();
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  const bool state_ok = parent->uri_playlist_next_frame_sequence == 0 && parent->uri_playlist_terminal &&
      parent->uri_playlist_barrier_failed;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (pending.get() != 0 || !state_ok) {
    std::cerr << "A wedged peer under condition noise did not fail the frame barrier\n";
    return false;
  }
  return true;
}

// The counterpart invariant: a camera that is still decoding its new chapter is
// slow, not wedged, and must be waited on however long it takes. Keep the
// playlist visibly moving for several stall budgets without ever signalling the
// barrier, then let the pair complete.
bool peer_progress_defers_stall_detection() {
  ScopedStallBudget budget("2");
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

  // Deliberately no g_cond_broadcast here: the waiter has to notice the progress
  // on its own timed wake, which is what distinguishes a live peer from a dead
  // one when nothing is signalling the barrier.
  std::atomic<bool> stop_ticking{false};
  std::thread ticker([&] {
    NvDsSrcParentBin* parent = fixture.parent();
    while (!stop_ticking) {
      g_mutex_lock(&parent->uri_playlist_barrier_mutex);
      ++parent->sub_bins[1].uri_list_decoded_frame_count;
      g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });
  std::this_thread::sleep_for(std::chrono::seconds(8));
  stop_ticking = true;
  ticker.join();

  const bool second_result = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
  first.join();

  NvDsSrcParentBin* parent = fixture.parent();
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  const bool state_ok = parent->uri_playlist_next_frame_sequence == 1 && !parent->uri_playlist_terminal &&
      !parent->uri_playlist_barrier_failed && !parent->uri_playlist_delivery_aborted;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (first_result != 1 || !second_result || !state_ok) {
    std::cerr << "A peer that kept making progress was treated as wedged\n";
    return false;
  }
  return true;
}

bool native_pause_excludes_stall_time() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  set_uri_playlist_waits_paused(fixture.parent(), TRUE);
  // Exercise overlapping native/process pauses, which must not be counted twice.
  set_uri_playlist_process_paused(TRUE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  set_uri_playlist_process_paused(FALSE);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  set_uri_playlist_waits_paused(fixture.parent(), FALSE);
  const bool paired = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
  return stayed_pending && paired && pending.get();
}

bool process_pause_excludes_sigstop_time() {
  // Prepare before SIGSTOP. After SIGCONT, let the waiter run before the resume
  // command; no expired wall-clock deadline may fail its otherwise healthy pair.
  const pid_t child = fork();
  if (child == 0) {
    ScopedStallBudget budget("1");
    BarrierFixture fixture;
    auto pending = std::async(
        std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
    if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
      _exit(1);
    }
    set_uri_playlist_process_paused(TRUE);
    raise(SIGSTOP);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    set_uri_playlist_process_paused(FALSE);
    const bool paired = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
    _exit(stayed_pending && paired && pending.get() ? 0 : 1);
  }
  if (child < 0) {
    return false;
  }
  int status = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  pid_t result = 0;
  while (result == 0 && std::chrono::steady_clock::now() < deadline) {
    result = waitpid(child, &status, WUNTRACED | WNOHANG);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (result != child || !WIFSTOPPED(status)) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return false;
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  kill(child, SIGCONT);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return false;
}

bool resumed_stall_still_reports_failure() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  set_uri_playlist_waits_paused(fixture.parent(), TRUE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  set_uri_playlist_waits_paused(fixture.parent(), FALSE);
  if (pending.wait_for(std::chrono::seconds(4)) != std::future_status::ready) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    std::cerr << "Resume did not restore stall detection\n";
    return false;
  }
  return stayed_pending && !pending.get() && fixture.parent()->uri_playlist_barrier_failed;
}

// A stitching calibration parks the decoders for minutes, and the operator can
// hit Pause and Resume in the middle of it. Resume must not restart a clock the
// calibration still owns, or the budget expires while the solve is still running
// and the run dies blaming the cameras.
bool suspension_reasons_do_not_release_each_other() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  const bool initially_idle = uri_playlist_calibration_idle_time_us(fixture.parent()) == G_MAXINT64;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  update_uri_playlist_calibration_phase(fixture.parent(), TRUE);
  // Ending an inner calibration phase must leave the outer owner active.
  update_uri_playlist_calibration_phase(fixture.parent(), TRUE);
  update_uri_playlist_calibration_phase(fixture.parent(), FALSE);
  const bool outer_active = uri_playlist_calibration_idle_time_us(fixture.parent()) == 0;
  // A complete pause/resume cycle nested inside the calibration suspension.
  set_uri_playlist_waits_paused(fixture.parent(), TRUE);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  set_uri_playlist_waits_paused(fixture.parent(), FALSE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  if (!stayed_pending) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    std::cerr << "Resuming the pipeline resumed a clock the calibration still held suspended\n";
    return false;
  }
  update_uri_playlist_calibration_phase(fixture.parent(), FALSE);
  const bool paired = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
  return paired && pending.get() && initially_idle && outer_active;
}

// The budget must still be spendable once calibration lets go; suspension
// postpones the watchdog, it does not retire it for the rest of the run.
bool released_calibration_suspension_restores_stall_detection() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  update_uri_playlist_calibration_phase(fixture.parent(), TRUE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  update_uri_playlist_calibration_phase(fixture.parent(), FALSE);
  if (pending.wait_for(std::chrono::seconds(4)) != std::future_status::ready) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    std::cerr << "Releasing the calibration suspension did not restore stall detection\n";
    return false;
  }
  const gint64 idle_us = uri_playlist_calibration_idle_time_us(fixture.parent());
  return stayed_pending && !pending.get() && fixture.parent()->uri_playlist_barrier_failed &&
      idle_us > 0 && idle_us < 5 * G_TIME_SPAN_SECOND;
}

// Scoreboard selection can hold the first Program output after PLAYING. Its
// nested owners must survive calibration completion and a native pause/resume;
// ending selection must likewise leave any remaining suspension intact.
bool operator_selection_preserves_overlapping_suspensions() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  update_uri_playlist_operator_selection_phase(fixture.parent(), TRUE);
  update_uri_playlist_operator_selection_phase(fixture.parent(), TRUE);
  update_uri_playlist_calibration_phase(fixture.parent(), TRUE);
  set_uri_playlist_waits_paused(fixture.parent(), TRUE);
  update_uri_playlist_operator_selection_phase(fixture.parent(), FALSE);
  update_uri_playlist_calibration_phase(fixture.parent(), FALSE);
  set_uri_playlist_waits_paused(fixture.parent(), FALSE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool selection_held = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

  update_uri_playlist_calibration_phase(fixture.parent(), TRUE);
  set_uri_playlist_waits_paused(fixture.parent(), TRUE);
  update_uri_playlist_operator_selection_phase(fixture.parent(), FALSE);
  // A duplicate end must not underflow ownership and disable the next run's clock.
  update_uri_playlist_operator_selection_phase(fixture.parent(), FALSE);
  const bool other_reasons_held = fixture.parent()->uri_playlist_operator_selection_depth == 0 &&
      fixture.parent()->uri_playlist_wait_suspend_mask ==
          (URI_PLAYLIST_WAIT_SUSPEND_CALIBRATION | URI_PLAYLIST_WAIT_SUSPEND_PIPELINE_PAUSE);
  update_uri_playlist_calibration_phase(fixture.parent(), FALSE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool pause_held = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  set_uri_playlist_waits_paused(fixture.parent(), FALSE);
  const bool paired = wait_at_uri_playlist_frame_barrier(fixture.source(1), 0, GST_SECOND);
  return paired && pending.get() && selection_held && other_reasons_held && pause_held &&
      !uri_playlist_waits_suspended(fixture.parent());
}

bool released_operator_selection_restores_stall_detection() {
  ScopedStallBudget budget("1");
  BarrierFixture fixture;
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  update_uri_playlist_operator_selection_phase(fixture.parent(), TRUE);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const bool stayed_pending = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  update_uri_playlist_operator_selection_phase(fixture.parent(), FALSE);
  if (pending.wait_for(std::chrono::seconds(4)) != std::future_status::ready) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    std::cerr << "Ending operator selection did not restore stall detection\n";
    return false;
  }
  return stayed_pending && !pending.get() && fixture.parent()->uri_playlist_barrier_failed && fixture.hasStreamError();
}

bool operator_selection_does_not_block_cancellation() {
  BarrierFixture fixture;
  update_uri_playlist_operator_selection_phase(fixture.parent(), TRUE);
  auto pending = std::async(
      std::launch::async, [&] { return wait_at_uri_playlist_frame_barrier(fixture.source(0), 0, GST_SECOND); });
  if (!fixture.waitUntilReady(0, 0, std::chrono::seconds(1))) {
    cancel_uri_playlist_frame_barrier(fixture.parent());
    return false;
  }
  cancel_uri_playlist_frame_barrier(fixture.parent());
  const bool woke = pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  update_uri_playlist_operator_selection_phase(fixture.parent(), FALSE);
  return woke && !pending.get() && !fixture.parent()->uri_playlist_barrier_failed &&
      !uri_playlist_waits_suspended(fixture.parent()) && !fixture.hasStreamError();
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

bool mismatched_sequences_fail_without_deadlock() {
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

  const auto mismatch_started = std::chrono::steady_clock::now();
  const bool second_result = wait_at_uri_playlist_frame_barrier(fixture.source(1), 1, 2 * GST_SECOND);
  first.join();
  const auto mismatch_latency = std::chrono::steady_clock::now() - mismatch_started;

  NvDsSrcParentBin* parent = fixture.parent();
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  const bool state_ok = parent->uri_playlist_next_frame_sequence == 0 && parent->uri_playlist_terminal &&
      parent->uri_playlist_barrier_failed && parent->uri_playlist_delivery_aborted;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (first_result != 0 || second_result || !state_ok || mismatch_latency > std::chrono::seconds(1)) {
    std::cerr << "Mismatched camera sequences did not fail both waiters promptly\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  // delayed_peer_outlives_old_timeout runs against the production stall budget,
  // so make sure the surrounding environment cannot shorten it.
  g_unsetenv(kStallBudgetVariable);
  if (!delayed_peer_outlives_old_timeout() || !wedged_peer_is_reported_instead_of_hanging() ||
      !signal_noise_does_not_mask_a_wedged_peer() || !peer_progress_defers_stall_detection() ||
      !native_pause_excludes_stall_time() || !process_pause_excludes_sigstop_time() ||
      !resumed_stall_still_reports_failure() || !suspension_reasons_do_not_release_each_other() ||
      !released_calibration_suspension_restores_stall_detection() ||
      !operator_selection_preserves_overlapping_suspensions() || !released_operator_selection_restores_stall_detection() ||
      !operator_selection_does_not_block_cancellation() || !cancellation_wakes_waiter_promptly() ||
      !mismatched_sequences_fail_without_deadlock()) {
    return 1;
  }
  return 0;
}
