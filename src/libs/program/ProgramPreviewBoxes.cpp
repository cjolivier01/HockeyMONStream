#include "hstream/src/libs/program/ProgramPreviewBoxes.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace hm::program {
void ProgramPreviewBoxes::Update(uint64_t pts, const std::vector<tracking::Observation>& tracks) {
  tracks_.clear();
  std::array<uint64_t, player_analytics::kMaximumTracks> ids{};
  for (const auto& track : tracks) {
    const auto& b = track.box;
    if ((b.class_id != 0 && b.class_id != goalie_ && b.class_id != referee_) ||
        track.id == player_analytics::kUntrackedId || !std::isfinite(b.left) || !std::isfinite(b.top) ||
        !std::isfinite(b.right) || !std::isfinite(b.bottom) || b.left >= b.right || b.top >= b.bottom)
      continue;
    if (tracks_.size() == ids.size())
      break;
    ids[tracks_.size()] = track.id;
    tracks_.push_back(track);
  }
  colors_.Update(0, pts, ids.data(), tracks_.size());
}
const draw_display::analytics::CommandList& ProgramPreviewBoxes::Build(
    const playcropper::FrameTransformGeometry& t,
    unsigned width,
    unsigned height) {
  commands_.Clear();
  if (!width || !height || !(t.crop_box.width() > 0) || !(t.crop_box.height() > 0))
    return commands_;
  const float radians = t.angle * (3.14159265358979323846F / 180.0F);
  const float cosine = std::cos(radians), sine = std::sin(radians);
  const auto map = [&](float x, float y) {
    const float dx = x - t.source_rect.left - t.anchor_point.x;
    const float dy = y - t.source_rect.top - t.anchor_point.y;
    return hm::Point{
        (t.anchor_point.x + dx * cosine - dy * sine - t.crop_box.left) * width / t.crop_box.width(),
        (t.anchor_point.y + dx * sine + dy * cosine - t.crop_box.top) * height / t.crop_box.height()};
  };
  // Same output-relative box stroke as the shared NVIDIA player compositor.
  const float stroke = 2 * std::clamp(height / 1080.0F, 0.5F, 8.0F);
  for (const auto& track : tracks_) {
    const auto lease = colors_.Find(track.id);
    if (!lease || lease->slot >= player_analytics::kTrackPalette.size())
      continue;
    const auto& c = player_analytics::kTrackPalette[lease->slot];
    const auto& b = track.box;
    const std::array<hm::Point, 4> corners{
        map(b.left, b.top), map(b.right, b.top), map(b.right, b.bottom), map(b.left, b.bottom)};
    const char* role = b.class_id == goalie_ ? "GOALIE" : b.class_id == referee_ ? "REF" : "";
    if (*role)
      commands_.AddText(corners[0].x, corners[0].y - 10 * stroke, 8 * stroke, role, {1, 1, 1, 1});
    for (size_t i = 0; i < corners.size(); ++i) {
      const auto& a = corners[i];
      const auto& z = corners[(i + 1) % corners.size()];
      commands_.AddLine(a.x, a.y, z.x, z.y, stroke, {c.red, c.green, c.blue, c.alpha});
    }
  }
  return commands_;
}
draw_display::RinkMapFrame ProgramPreviewBoxes::RinkFrame(
    const std::string& generation,
    unsigned width,
    unsigned height,
    uint64_t pts,
    uint64_t sequence) const {
  draw_display::RinkMapFrame frame;
  frame.generation = generation;
  frame.stitched_width = width;
  frame.stitched_height = height;
  frame.coordinate_width = width;
  frame.coordinate_height = height;
  frame.pts = pts;
  frame.sequence = sequence;
  frame.have_tracks = true;
  for (const auto& track : tracks_) {
    const auto lease = colors_.Find(track.id);
    if (!lease || lease->slot >= player_analytics::kTrackPalette.size())
      continue;
    const auto& color = player_analytics::kTrackPalette[lease->slot];
    const auto& b = track.box;
    frame.players.push_back(
        {b.left,
         b.top,
         b.right - b.left,
         b.bottom - b.top,
         {color.red, color.green, color.blue, color.alpha},
         track.id,
         false});
  }
  return frame;
}
void ProgramPreviewBoxes::Reset() {
  colors_.Reset();
  tracks_.clear();
  commands_.Clear();
}
} // namespace hm::program
