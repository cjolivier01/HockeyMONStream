#include "TensorRtGpuIdentity.h"

#include <cstdint>
#include <iostream>

int main() {
  constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  if (!hm::inference::UseLowMemoryProfile(8 * kGiB)) {
    std::cerr << "An 8 GiB GPU must select the low-memory profile\n";
    return 1;
  }
  if (hm::inference::UseLowMemoryProfile(8 * kGiB + 1)) {
    std::cerr << "A GPU above 8 GiB must select the normal profile\n";
    return 1;
  }
  return 0;
}
