#include "Int8SamplePlan.h"

#include <iostream>

int main() {
  const auto times = hm::pipeline::Int8SampleTimes(64000000000ULL, 64);
  if (!times.ok() || times->size() != 64 || times->front() != 500000000ULL || times->back() != 63500000000ULL ||
      hm::pipeline::Int8SampleTimes(100, 64).ok() || hm::pipeline::Int8SampleTimes(64000000000ULL, 1).ok() ||
      hm::pipeline::Int8SampleTimes(UINT64_MAX, 256).ok()) {
    std::cerr << "INT8 samples must cover the recording with bounded, distinct interior timestamps\n";
    return 1;
  }
  return 0;
}
