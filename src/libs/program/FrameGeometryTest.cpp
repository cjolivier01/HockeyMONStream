#include "hstream/src/libs/program/FrameGeometry.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Near(float actual, float expected, const char* message) {
  Check(std::abs(actual - expected) < 0.002f, message);
}
void Box(const hm::BBox& actual, const hm::BBox& expected) {
  Near(actual.left, expected.left, "Crop left differs");
  Near(actual.top, expected.top, "Crop top differs");
  Near(actual.right, expected.right, "Crop right differs");
  Near(actual.bottom, expected.bottom, "Crop bottom differs");
}
} // namespace

int main() {
  try {
    using hm::BBox;
    using hm::playcropper::CalculateFrameTransformGeometry;
    using hm::playcropper::ResolveTrackingBox;
    // The archived canvas and encoded panorama have independently rounded
    // dimensions. A missing follower already belongs to the decoded surface.
    for (const auto& dimensions : {std::array<size_t, 2>{13802, 4606}, {8192, 2732}, {4096, 1366}}) {
      const auto crop = ResolveTrackingBox(8192, 2732, dimensions[0], dimensions[1], 1920, 1080, std::nullopt);
      Check(crop.ok(), "Missing follower rejected");
      const float width = 2732.f * 16.f / 9.f;
      Box(*crop, BBox((8192.f - width) / 2, 0, (8192.f + width) / 2, 2732));
      const auto transform = CalculateFrameTransformGeometry(8192, 2732, *crop, false, 8, 11);
      Near(transform.angle, 0, "Centered fallback must not acquire edge rotation");
      Near(transform.crop_box.height(), 2732, "Fallback must retain the full panorama height");
      const auto full = CalculateFrameTransformGeometry(8192, 2732, *crop, true, 8, 11);
      Box(full.crop_box, BBox(0, 0, 8192, 2732));
    }
    const auto scaled = ResolveTrackingBox(8192, 2732, 13802, 4606, 1920, 1080, BBox(1380.2, 460.6, 9661.4, 4145.4));
    Check(scaled.ok(), "Recorded follower rejected");
    Box(*scaled, BBox(819.2, 273.2, 5734.4, 2458.8));
    const auto same = ResolveTrackingBox(8192, 2732, 8192, 2732, 1920, 1080, *scaled);
    Check(same.ok(), "Surface follower rejected");
    Box(*same, *scaled);
    // AMD's camera operates in panorama coordinates. Absence must survive
    // until rendering, where each output chooses its own aspect ratio.
    const auto wide = ResolveTrackingBox(1600, 1200, 1600, 1200, 1920, 1080, std::nullopt);
    const auto tall = ResolveTrackingBox(1600, 1200, 1600, 1200, 1080, 1920, std::nullopt);
    const auto square = ResolveTrackingBox(1600, 1200, 1600, 1200, 1000, 1000, std::nullopt);
    Check(wide.ok() && tall.ok() && square.ok(), "Output-aspect fallback rejected");
    Box(*wide, BBox(0, 150, 1600, 1050));
    Box(*tall, BBox(462.5, 0, 1137.5, 1200));
    Box(*square, BBox(200, 0, 1400, 1200));
    for (size_t zero = 0; zero < 6; ++zero) {
      std::array<size_t, 6> dimensions{8192, 2732, 13802, 4606, 1920, 1080};
      dimensions[zero] = 0;
      for (const auto& box : {std::optional<BBox>{}, std::optional<BBox>{BBox(0, 0, 100, 100)}})
        Check(
            !ResolveTrackingBox(
                 dimensions[0], dimensions[1], dimensions[2], dimensions[3], dimensions[4], dimensions[5], box)
                 .ok(),
            "Zero dimension accepted");
    }
    std::cout << "FrameGeometryTest passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
