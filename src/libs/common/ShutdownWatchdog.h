#pragma once

#include <glib.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace hm {

// Independently bounds synchronous GStreamer/vendor teardown. A blocked main
// context cannot deliver a bus error or check its own deadline. Zero disables.
class ShutdownWatchdog {
 public:
  // The operation string must outlive the watchdog; use a string literal.
  ShutdownWatchdog(const char* operation, gint64 budget_us);
  ShutdownWatchdog(const ShutdownWatchdog&) = delete;
  ShutdownWatchdog& operator=(const ShutdownWatchdog&) = delete;
  ~ShutdownWatchdog();

 private:
  static void fire(const char* operation, long blocked_tid, gint64 budget_us);
  const char* operation_;
  long blocked_tid_;
  std::mutex mutex_;
  std::condition_variable finished_;
  bool done_ = false;
  std::thread worker_;
};

} // namespace hm
