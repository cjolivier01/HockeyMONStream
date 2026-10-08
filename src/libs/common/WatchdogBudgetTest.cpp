#include "hstream/src/libs/common/pipeline_utils.h"

#include <iostream>

namespace {

constexpr gint64 kFallback = 8 * G_USEC_PER_SEC;

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  bool ok = true;

  ok &= expect(
      hm::parse_watchdog_budget_us(nullptr, kFallback) == kFallback,
      "An unset budget must fall back to the compiled-in default");
  ok &= expect(
      hm::parse_watchdog_budget_us("", kFallback) == kFallback,
      "An empty budget must fall back rather than parse as zero and disable the watchdog");
  ok &= expect(
      hm::parse_watchdog_budget_us("12", kFallback) == 12 * G_USEC_PER_SEC,
      "A whole number of seconds must scale to microseconds");
  ok &= expect(
      hm::parse_watchdog_budget_us("0", kFallback) == 0,
      "An explicit zero must disable the watchdog, which is how a hang is debugged");

  ok &= expect(
      hm::parse_watchdog_budget_us("-1", kFallback) == kFallback,
      "A negative budget must be rejected, not passed through as an immediate fire");
  ok &= expect(
      hm::parse_watchdog_budget_us("5s", kFallback) == kFallback,
      "Trailing garbage must be rejected rather than silently truncated");
  ok &= expect(hm::parse_watchdog_budget_us("abc", kFallback) == kFallback, "A non-numeric budget must fall back");

  // g_ascii_strtoll saturates at G_MAXINT64 instead of failing, so these clear
  // a naive sign check and then wrap in the microsecond multiply: the first to
  // a negative budget that silently disables the watchdog, the second to
  // exactly 1 s, which kills shutdowns that are merely slow.
  ok &= expect(
      hm::parse_watchdog_budget_us("99999999999999999999", kFallback) == kFallback,
      "A budget that saturates strtoll must be rejected before it can wrap negative");
  ok &= expect(
      hm::parse_watchdog_budget_us("288230376151711745", kFallback) == kFallback,
      "A budget whose microsecond product wraps to a small positive must be rejected");
  ok &= expect(
      hm::parse_watchdog_budget_us("9223372036854", kFallback) == 9223372036854 * G_USEC_PER_SEC,
      "The largest budget that scales without overflow must still be accepted");

  if (!ok) {
    return 1;
  }
  std::cout << "WatchdogBudgetTest passed\n";
  return 0;
}
