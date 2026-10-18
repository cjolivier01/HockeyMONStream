#include "hstream/src/libs/draw_display/RinkTrackSmoothing.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    hm::draw_display::RinkTrackSmoothing filter;
    constexpr uint64_t step = 33333333;
    double raw_energy = 0, filtered_energy = 0;
    for (uint64_t i = 0; i < 180; ++i) {
      const cv::Point2d noisy(i % 2 ? .45 : -.45, i % 3 ? .3 : -.6);
      const auto smoothed = filter.Update(1, i * step, 7, noisy);
      if (i > 30) {
        raw_energy += noisy.dot(noisy);
        filtered_energy += smoothed.dot(smoothed);
      }
    }
    check(filtered_energy < raw_energy * .25, "Stationary dot jitter should fall by more than half in RMS");
    filter.Reset();
    double maximum_lag = 0;
    for (uint64_t i = 0; i < 150; ++i) {
      const cv::Point2d moving(double(i) * step * 1e-9 * 6, 0);
      const auto smoothed = filter.Update(1, i * step, 7, moving);
      maximum_lag = std::max(maximum_lag, cv::norm(smoothed - moving));
    }
    check(maximum_lag < .8, "A skating player must not be held far behind their measured position");
    filter.Reset();
    for (uint64_t i = 0; i < 10; ++i)
      filter.Update(1, i * step, 7, {0, 0});
    check(cv::norm(filter.Update(1, 10 * step, 7, {10, 7})) < .01, "A single loose-box spike must not move the dot");
    check(cv::norm(filter.Update(1, 10 * step, 7, {2, 3})) < .01, "Repeated painting must not advance smoothing");
    const cv::Point2d current(12, 4);
    check(filter.Update(1, 30 * step, 7, current) == current, "A missing-track gap must reset history");
    check(filter.Update(1, 1, 7, {5, 6}) == cv::Point2d(5, 6), "Backward source time must reset history");
    check(filter.Update(2, 2, 7, {7, 8}) == cv::Point2d(7, 8), "A different track must not inherit a dot");
    check(filter.Update(1, 2, 8, {8, 9}) == cv::Point2d(8, 9), "A new color lease must reset a reused track");
    const auto invalid = std::numeric_limits<uint64_t>::max();
    check(filter.Update(invalid, 2, 8, current) == current, "Untracked players must not share history");
    check(filter.Update(1, invalid, 8, current) == current, "Unknown timestamps cannot drive smoothing");
    for (uint64_t id = 0; id < 10000; ++id)
      check(filter.Update(id, id * step, 0, current) == current, "Bounded cache churn cannot blend identities");
    std::cout << "RinkTrackSmoothing tests passed; stationary RMS ratio=" << std::sqrt(filtered_energy / raw_energy)
              << ", maximum lag at 6 m/s=" << maximum_lag << " m\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
