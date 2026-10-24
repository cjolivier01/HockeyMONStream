#include "hstream/src/libs/program/ProgramPreviewBoxes.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
bool Near(float a, float b) {
  return std::abs(a - b) < .001F;
}
} // namespace
int main() {
  try {
    hm::program::ProgramPreviewBoxes boxes;
    const hm::playcropper::FrameTransformGeometry crop{{10, 0, 110, 100}, {50, 50}, {0, 20, 100, 80}, 0};
    Check(boxes.Build(crop, 200, 120).empty(), "Absent tracks drew boxes");
    std::vector<hm::tracking::Observation> tracks{
        {7, {20, 30, 40, 60, .9F, 0, 0}},
        {9, {70, 40, 90, 60, .9F, 0, 1}},
        {10, {1, 1, 3, 3, .9F, 1, 2}},
        {11, {1, 1, 1, 3, .9F, 0, 3}}};
    boxes.Update(100, tracks);
    const auto& drawn = boxes.Build(crop, 200, 120);
    Check(drawn.size() == 8, "Nonplayers/degenerate tracks drew boxes");
    const auto first = drawn.data()[0];
    Check(
        Near(first.x0, 20) && Near(first.y0, 20) && Near(first.x1, 60) && Near(first.y1, 20),
        "Crop translation/output scale did not match video");
    const auto color = first.color;
    std::swap(tracks[0], tracks[1]);
    boxes.Update(200, tracks);
    const auto retained = boxes.Build(crop, 200, 120).data()[4].color;
    Check(
        color.red == retained.red && color.green == retained.green && color.blue == retained.blue,
        "Track colors changed with observation order");
    auto rotated = crop;
    rotated.angle = 90;
    const auto turn = boxes.Build(rotated, 200, 120).data()[4];
    Check(
        Near(turn.x0, 140) && Near(turn.y0, -20) && Near(turn.x1, 140) && Near(turn.y1, 20),
        "Forward rotation did not align with inverse video sampling");
    boxes.Update(300, {});
    Check(boxes.Build(crop, 200, 120).empty(), "Lost tracks remained visible");
    boxes.Update(400, tracks);
    boxes.Reset();
    Check(boxes.Build(crop, 200, 120).empty(), "Seek/reset retained old boxes");
    tracks.clear();
    for (uint64_t i = 0; i < 400; ++i)
      tracks.push_back({i, {20, 30, 40, 60, .9F, 0, 0}});
    boxes.Update(500, tracks);
    Check(
        boxes.Build(crop, 200, 120).size() == 4 * hm::player_analytics::kMaximumTracks,
        "Preview did not bound track metadata");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
