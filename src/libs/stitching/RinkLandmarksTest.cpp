#include "hstream/src/libs/stitching/RinkLandmarks.h"

#include <iostream>
#include <limits>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main() {
  using Model = hm::stitching::RinkLandmarks;
  std::vector<float> classes(Model::kQueries * Model::kClasses, -20);
  std::vector<float> masks(Model::kQueries * Model::kMaskSize * Model::kMaskSize, -20);
  for (int query = 0; query < Model::kQueries; ++query)
    classes[query * Model::kClasses + 11] = 20;
  auto run = [&] {
    return Model::Postprocess(classes.data(), classes.size(), masks.data(), masks.size(), {1008, 504}, {2000, 1000});
  };
  auto result = run();
  bool ok = expect(result.ok() && result->empty(), "Background queries must not become landmarks");
  // A centre line, a goal area and the field. Only the painted line can be
  // converted to geometric samples; a net centroid is above the ice.
  for (int query = 0; query < 3; ++query) {
    classes[query * Model::kClasses + 11] = -20;
    classes[query * Model::kClasses + (query == 0 ? 2 : query == 1 ? 6 : 5)] = 20;
    for (int y = 10; y < 115; ++y)
      for (int x = 70 + query * 20; x < 74 + query * 20; ++x)
        masks[query * Model::kMaskSize * Model::kMaskSize + y * Model::kMaskSize + x] = 20;
  }
  result = run();
  ok &= expect(result.ok() && result->size() == 2, "Field masks must be omitted without losing lines or goals");
  if (result.ok())
    for (const auto& detection : *result) {
      ok &= expect(
          detection.outline.size() <= 128 && detection.score > .99, "Contours must be bounded and scores preserved");
      ok &= expect(
          detection.class_index == 2 ? detection.line_samples.size() >= 2 : detection.line_samples.empty(),
          "Only painted lines may create line observations");
      for (const auto& p : detection.line_samples)
        ok &= expect(
            p.x > 550 && p.x < 600 && p.y > 0 && p.y < 1000,
            "Crop/padding and pixel-centre scaling must match the source");
    }
  masks[0] = std::numeric_limits<float>::quiet_NaN();
  ok &= expect(!run().ok(), "Nonfinite output must be rejected");
  ok &= expect(
      !Model::Postprocess(classes.data(), 12, masks.data(), masks.size(), {1008, 504}, {2000, 1000}).ok(),
      "Unexpected tensor shapes must be rejected");
  if (ok)
    std::cout << "RinkLandmarks tests passed\n";
  return ok ? 0 : 1;
}
