// Pins get_or_create_font_cache()'s ownership contract so it cannot be changed
// silently.
//
// It retains only a static weak_ptr, so the cache is destroyed as soon as the
// last caller drops its strong reference, and constructing one forks fc-list
// (measured at ~9 ms on a desktop with ~2k fonts installed). That combination
// is why a caller must hold the cache in a member: taking it into a local
// rebuilds it on every call. This test cannot see caller-side misuse, but it
// does fail if the contract those callers depend on is altered -- for example
// by promoting the weak_ptr to a shared_ptr, which would leak a glyph atlas
// and CUDA allocations into static destruction.
//
// Assertions here observe process-global state, so keep this binary to a single
// source file; a second test in the same process would make it order-dependent.

#include "hstream/src/libs/draw_display/Fonts.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  using hm::draw_display::get_or_create_font_cache;

  // Nothing holds a cache yet, so a non-creating request must not build one.
  check(get_or_create_font_cache(/*create_if_needed=*/false) == nullptr, "no cache exists before the first request");

  {
    std::shared_ptr<hm::draw_display::FontCache> held = get_or_create_font_cache();
    check(held != nullptr, "get_or_create_font_cache() creates a cache on demand");

    // While a strong reference is held, every caller shares that instance
    // instead of paying for another fc-list fork.
    check(get_or_create_font_cache().get() == held.get(), "callers share the cache while a strong reference is held");
    check(
        get_or_create_font_cache(/*create_if_needed=*/false).get() == held.get(),
        "a non-creating request finds the live cache");
  }

  // The last strong reference is gone, so the static weak_ptr is expired again.
  // Callers must therefore keep the cache in a member, not in a local.
  check(
      get_or_create_font_cache(/*create_if_needed=*/false) == nullptr,
      "the cache is released once every strong reference is dropped");

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("FontsTest passed\n");
  return EXIT_SUCCESS;
}
