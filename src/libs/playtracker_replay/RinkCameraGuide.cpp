#include "hstream/src/libs/playtracker_replay/RinkCameraGuide.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hm::playtracker_replay {
namespace {
bool valid(const hm::BBox& b) {
  return std::isfinite(b.left) && std::isfinite(b.top) && std::isfinite(b.right) && std::isfinite(b.bottom) &&
      b.right > b.left && b.bottom > b.top;
}
double median(std::array<double, 256>& values, size_t count) {
  std::sort(values.begin(), values.begin() + count);
  return count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
}
double blend(double seconds, double time_constant) {
  return -std::expm1(-seconds / time_constant);
}
} // namespace

RinkCameraGuide::RinkCameraGuide(RinkCameraSetup setup, double rotation, unsigned width, unsigned height)
    : setup_(std::move(setup)), rotation_(rotation), width_(width), height_(height) {
  if (!setup_.map)
    return;
  const double x = setup_.map->layout.length / 2 - setup_.map->layout.goal_inset;
  for (int i = 0; i < 2; ++i) {
    auto point = setup_.map->Project({i ? x : -x, 0}, rotation_, width_, height_);
    if (point)
      goals_[i] = cv::Point2d(point->x * width_, point->y * height_);
  }
}

hm::BBox RinkCameraGuide::Step(
    uint64_t pts_ns,
    const hm::BBox& baseline,
    const hm::BBox& arena,
    const std::optional<hm::BBox>& cluster,
    const Track* tracks,
    size_t count,
    RinkCameraInfo* info) {
  *info = {};
  if (!setup_.map || !width_ || !height_ || !valid(baseline) || !valid(arena))
    return baseline;
  if (previous_ns_ && pts_ns == *previous_ns_) {
    *info = previous_info_;
    return previous_box_;
  }
  const bool reset = !previous_ns_ || pts_ns < *previous_ns_ || pts_ns - *previous_ns_ > 300000000;
  const double dt = reset ? 0 : (pts_ns - *previous_ns_) / 1e9;
  if (reset) {
    history_ = {};
    offset_ = {};
    scale_ = 1;
  }
  previous_ns_ = pts_ns;
  std::array<double, 256> xs{}, heights{}, vx{}, vy{}, px{}, py{};
  size_t located = 0, moving = 0;
  for (size_t i = 0; tracks && i < std::min(count, size_t(256)); ++i) {
    const auto& track = tracks[i];
    if (track.ignored || track.id == std::numeric_limits<uint64_t>::max() || !valid(track.box))
      continue;
    const cv::Point2d pixel((track.box.left + track.box.right) / 2.0, track.box.bottom);
    // Follow the native policy's chosen play cluster; this does not prune or
    // relabel any observation supplied to the native tracker.
    if (cluster &&
        (pixel.x < cluster->left || pixel.x > cluster->right || pixel.y < cluster->top || pixel.y > cluster->bottom))
      continue;
    const auto point = setup_.map->Locate(pixel.x / width_, pixel.y / height_, rotation_, width_, height_);
    if (!point)
      continue;
    xs[located] = point->x;
    heights[located++] = track.box.height();
    const size_t bucket = ((track.id ^ (track.id >> 32)) * 11400714819323198485ULL >> 57) * 4;
    History* item = &history_[bucket];
    for (size_t slot = bucket; slot < bucket + 4; ++slot) {
      auto& candidate = history_[slot];
      if (candidate.used && candidate.id == track.id) {
        item = &candidate;
        break;
      }
      if (!candidate.used || (item->used && candidate.seen_ns < item->seen_ns))
        item = &candidate;
    }
    if (!item->used || item->id != track.id || pts_ns - item->seen_ns > 300000000) {
      *item = {track.id, pts_ns, pts_ns, *point, pixel, {}, {}, true, false};
      continue;
    }
    item->seen_ns = pts_ns;
    const double elapsed = (pts_ns - item->reference_ns) / 1e9;
    if (elapsed >= 0.12) {
      const auto velocity = (*point - item->point) / elapsed;
      const auto pixel_velocity = (pixel - item->pixel) / elapsed;
      // Box-foot jumps and ID reassociations must not predict a rush.
      if (elapsed <= 0.4 && cv::norm(velocity) <= 14) {
        const double a = item->moving_sample ? blend(elapsed, 0.35) : 1;
        item->velocity += (velocity - item->velocity) * a;
        item->pixel_velocity += (pixel_velocity - item->pixel_velocity) * a;
        item->moving_sample = true;
      } else {
        item->moving_sample = false;
      }
      item->point = *point;
      item->pixel = pixel;
      item->reference_ns = pts_ns;
    }
    if (item->moving_sample) {
      vx[moving] = item->velocity.x;
      vy[moving] = item->velocity.y;
      px[moving] = item->pixel_velocity.x;
      py[moving++] = item->pixel_velocity.y;
    }
  }
  info->located = located;
  cv::Point2d desired_offset;
  double desired_scale = 1;
  const double aspect = baseline.width() / baseline.height();
  if (!reset && located >= 3 && moving >= 3 && cluster && valid(*cluster)) {
    info->active = true;
    const double speed = std::hypot(median(vx, moving), median(vy, moving));
    info->speed_mps = speed;
    const double rush = std::clamp((speed - 1.5) / 4, 0.0, 1.0);
    if (setup_.settings.anticipate) {
      const double lead_seconds = std::min(0.6, 4 / std::max(speed, 0.1));
      desired_offset.x =
          std::clamp(median(px, moving) * lead_seconds * rush, -baseline.width() * 0.16, baseline.width() * 0.16);
      desired_offset.y =
          std::clamp(median(py, moving) * lead_seconds * rush, -baseline.height() * 0.06, baseline.height() * 0.06);
    }
    if (setup_.settings.adaptive_zoom) {
      desired_scale = 0.94 + 0.24 * rush;
      desired_scale = std::max(
          desired_scale,
          std::min(1.18, std::max(cluster->width() / baseline.width(), cluster->height() / baseline.height()) * 1.08));
    }
    if (setup_.settings.frame_goals) {
      const double centre_x = median(xs, located);
      const int side = centre_x > 0 ? 1 : 0;
      const auto& goal = goals_[side];
      const double zone = setup_.map->layout.blue_line_distance + 2;
      const double weight = std::clamp((std::abs(centre_x) - zone) / 8, 0.0, 1.0);
      if (goal && weight > 0) {
        info->goal = side ? 1 : -1;
        const double height = median(heights, located);
        const double left = goal->x - height * 0.3, right = goal->x + height * 0.3;
        const double top = goal->y - height * 1.2, bottom = goal->y + height * 0.2;
        const cv::Point2d base((baseline.left + baseline.right) / 2, (baseline.top + baseline.bottom) / 2);
        const cv::Point2d centre = base + desired_offset;
        const double required_width =
            2 * std::max({centre.x - left, right - centre.x, (centre.y - top) * aspect, (bottom - centre.y) * aspect});
        const double goal_scale = std::clamp(required_width / baseline.width(), desired_scale, 1.35);
        desired_scale += (goal_scale - desired_scale) * weight;
        // Leave an already framed goal alone. Recentring on a changing player
        // cluster here would bypass the native policy's deliberate pan restraint.
        const double half_width = baseline.width() * desired_scale / 2;
        if (right - half_width <= left + half_width) {
          const double cx = std::clamp(base.x + desired_offset.x, right - half_width, left + half_width);
          desired_offset.x += (cx - base.x - desired_offset.x) * weight;
        }
        const double half_height = half_width / aspect;
        if (bottom - half_height <= top + half_height) {
          const double cy = std::clamp(base.y + desired_offset.y, bottom - half_height, top + half_height);
          desired_offset.y += (cy - base.y - desired_offset.y) * weight;
        }
      }
    }
  }
  desired_offset.x = std::clamp(desired_offset.x, -baseline.width() * 0.2, baseline.width() * 0.2);
  desired_offset.y = std::clamp(desired_offset.y, -baseline.height() * 0.1, baseline.height() * 0.1);
  offset_ += (desired_offset - offset_) * blend(dt, 0.45);
  scale_ += (desired_scale - scale_) * blend(dt, desired_scale > scale_ ? 0.35 : 1.5);
  const double w = std::min({baseline.width() * scale_, double(arena.width()), arena.height() * aspect});
  const double h = w / aspect;
  const double cx = (baseline.left + baseline.right) / 2 + offset_.x;
  const double cy = (baseline.top + baseline.bottom) / 2 + offset_.y;
  const double left = std::clamp(cx - w / 2, double(arena.left), arena.right - w);
  const double top = std::clamp(cy - h / 2, double(arena.top), arena.bottom - h);
  // An inactive guide with no residual must be exactly the native trajectory.
  previous_box_ =
      reset || (scale_ == 1 && offset_ == cv::Point2d()) ? baseline : hm::BBox(left, top, left + w, top + h);
  info->lead_pixels = (previous_box_.left + previous_box_.right - baseline.left - baseline.right) / 2;
  info->zoom_ratio = previous_box_.width() / baseline.width();
  previous_info_ = *info;
  return previous_box_;
}
} // namespace hm::playtracker_replay
