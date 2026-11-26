#include "hstream/src/libs/scoreboard/CameraClock.h"

#include <gst/gst.h>
#include <mutex>

namespace hm::scoreboard {
namespace {
struct State {
  std::mutex mutex;
  bool paused = false;
  CameraClock::time_point pause_start;
  std::chrono::steady_clock::duration excluded{};
};
State& SharedState() {
  // The CLI and dynamically loaded cropper can each link this library. Store
  // the clock state on GStreamer's process-wide singleton so their pause and
  // decoder budgets share one state even without exported executable symbols.
  static State* state = [] {
    static GstClock* clock = gst_system_clock_obtain(); // Retained for process lifetime.
    const auto key = g_quark_from_static_string("hstream-scoreboard-operation-clock-v1");
    auto* candidate = new State;
    if (!g_object_replace_qdata(
            G_OBJECT(clock),
            key,
            nullptr,
            candidate,
            [](gpointer value) { delete static_cast<State*>(value); },
            nullptr))
      delete candidate;
    return static_cast<State*>(g_object_get_qdata(G_OBJECT(clock), key));
  }();
  return *state;
}
} // namespace
CameraClock::time_point CameraClock::now() {
  auto& state = SharedState();
  std::lock_guard<std::mutex> lock(state.mutex);
  return (state.paused ? state.pause_start : std::chrono::steady_clock::now()) - state.excluded;
}
void CameraClock::SetPaused(bool value) {
  auto& state = SharedState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (value == state.paused)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (value)
    state.pause_start = now;
  else
    state.excluded += now - state.pause_start;
  state.paused = value;
}
} // namespace hm::scoreboard
