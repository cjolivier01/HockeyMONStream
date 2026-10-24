#include "hstream/src/libs/tracking/NativeTracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "hockeymon/csrc/bytetrack/KalmanFilter.h"

namespace hm::tracking {
namespace {
namespace kalman = hm::tracker::byte_kalman;
using detection::Proposal;

bool Probability(float value) {
  return std::isfinite(value) && value >= 0 && value <= 1;
}
bool Valid(const Proposal& p) {
  return Probability(p.confidence) && p.class_id >= 0 && std::isfinite(p.left) && std::isfinite(p.right) &&
      std::isfinite(p.top) && std::isfinite(p.bottom) && p.left >= 0 && p.top >= 0 && p.right > p.left &&
      p.bottom > p.top && p.right < (1 << 24) && p.bottom < (1 << 24) &&
      std::isfinite((p.right - p.left) / (p.bottom - p.top));
}
kalman::DETECTBOX Measurement(const Proposal& p) {
  kalman::DETECTBOX measurement;
  measurement << (p.left + p.right) / 2, (p.top + p.bottom) / 2, (p.right - p.left) / (p.bottom - p.top),
      p.bottom - p.top;
  return measurement;
}
float Iou(const Proposal& a, const Proposal& b) {
  const double overlap = std::max(0.f, std::min(a.right, b.right) - std::max(a.left, b.left)) *
      static_cast<double>(std::max(0.f, std::min(a.bottom, b.bottom) - std::max(a.top, b.top)));
  const double total = (a.right - a.left) * static_cast<double>(a.bottom - a.top) +
      (b.right - b.left) * static_cast<double>(b.bottom - b.top) - overlap;
  return total > 0 ? overlap / total : 0;
}

// Rectangular shortest augmenting-path assignment with one dummy column per
// track. A real edge must improve on leaving that track unmatched. Stable row
// and column order resolve equal-cost assignments without unordered containers.
std::vector<int> Assign(const std::vector<std::vector<double>>& cost, double limit) {
  const size_t rows = cost.size(), columns = cost.front().size() + rows;
  std::vector<double> u(rows + 1), v(columns + 1);
  std::vector<size_t> p(columns + 1), way(columns + 1);
  for (size_t i = 1; i <= rows; ++i) {
    p[0] = i;
    size_t j0 = 0;
    std::vector<double> best(columns + 1, std::numeric_limits<double>::infinity());
    std::vector<bool> used(columns + 1);
    do {
      used[j0] = true;
      const size_t i0 = p[j0];
      double delta = std::numeric_limits<double>::infinity();
      size_t j1 = 0;
      for (size_t j = 1; j <= columns; ++j)
        if (!used[j]) {
          const double edge = j <= cost.front().size() ? cost[i0 - 1][j - 1] : limit;
          const double reduced = edge - u[i0] - v[j];
          if (reduced < best[j]) {
            best[j] = reduced;
            way[j] = j0;
          }
          if (best[j] < delta) {
            delta = best[j];
            j1 = j;
          }
        }
      for (size_t j = 0; j <= columns; ++j) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else
          best[j] -= delta;
      }
      j0 = j1;
    } while (p[j0]);
    do {
      const size_t j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0);
  }
  std::vector<int> assignment(rows, -1);
  for (size_t j = 1; j <= cost.front().size(); ++j)
    if (p[j] && cost[p[j] - 1][j - 1] < limit)
      assignment[p[j] - 1] = j - 1;
  return assignment;
}
} // namespace

struct NativeTracker::Impl {
  struct Track {
    uint64_t id, last_sequence;
    size_t hits;
    bool confirmed;
    Proposal box;
    kalman::KAL_DATA state;
  };
  TrackerConfig config;
  kalman::KalmanFilter filter;
  std::vector<Track> tracks;
  std::optional<uint64_t> last_sequence;
  uint64_t next_id{1};
  bool retired{false};
  explicit Impl(TrackerConfig config) : config(config) {}

  void Predict(Track& track, uint64_t sequence) {
    if (track.last_sequence + 1 != sequence)
      track.state.first(7) = 0;
    filter.predict(track.state.first, track.state.second);
    const auto& mean = track.state.first;
    const float half_width = mean(2) * mean(3) / 2, half_height = mean(3) / 2;
    if (!mean.allFinite() || !track.state.second.allFinite() || half_width <= 0 || half_height <= 0)
      throw std::runtime_error("Invalid native tracker prediction");
    track.box.left = mean(0) - half_width;
    track.box.right = mean(0) + half_width;
    track.box.top = mean(1) - half_height;
    track.box.bottom = mean(1) + half_height;
  }
  void Match(
      const std::vector<size_t>& track_indices,
      const std::vector<size_t>& detection_indices,
      const std::vector<Proposal>& detections,
      float limit,
      bool weighted,
      uint64_t sequence,
      std::vector<bool>& matched_tracks,
      std::vector<bool>& matched_detections) {
    if (track_indices.empty() || detection_indices.empty())
      return;
    const size_t rows = track_indices.size(), columns = detection_indices.size();
    if (rows > config.maximum_cost_matrix_bytes / sizeof(double) / columns)
      throw std::runtime_error("Native tracker assignment exceeds configured memory bound");
    std::vector<std::vector<double>> cost(rows, std::vector<double>(columns));
    for (size_t i = 0; i < rows; ++i)
      for (size_t j = 0; j < columns; ++j) {
        const auto& track = tracks[track_indices[i]];
        const auto& detection = detections[detection_indices[j]];
        cost[i][j] = track.box.class_id != detection.class_id
            ? 1
            : 1 - Iou(track.box, detection) * (weighted ? detection.confidence : 1);
      }
    const auto assignment = Assign(cost, limit);
    for (size_t i = 0; i < rows; ++i) {
      if (assignment[i] < 0)
        continue;
      const size_t t = track_indices[i], d = detection_indices[assignment[i]];
      auto& track = tracks[t];
      track.state = filter.update(track.state.first, track.state.second, Measurement(detections[d]));
      if (!track.state.first.allFinite() || !track.state.second.allFinite())
        throw std::runtime_error("Invalid native tracker update");
      // Publish this frame's measured box. Predictions only drive association.
      track.box = detections[d];
      track.last_sequence = sequence;
      ++track.hits;
      track.confirmed |= track.hits >= config.confirmation_frames;
      matched_tracks[t] = matched_detections[d] = true;
    }
  }
};
NativeTracker::NativeTracker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeTracker::~NativeTracker() = default;
absl::Status ValidateTrackerConfig(const TrackerConfig& config) {
  if (!Probability(config.low_score) || !Probability(config.high_score) || !Probability(config.new_track_score) ||
      config.low_score > config.high_score || config.high_score > config.new_track_score ||
      !Probability(config.high_match_cost) || config.high_match_cost <= 0 || config.high_match_cost >= 1 ||
      !Probability(config.low_match_cost) || config.low_match_cost <= 0 || config.low_match_cost >= 1 ||
      !Probability(config.tentative_match_cost) || config.tentative_match_cost <= 0 ||
      config.tentative_match_cost >= 1 || !config.confirmation_frames || !config.maximum_tracks ||
      config.maximum_tracks > std::numeric_limits<int>::max() / 2 || config.maximum_cost_matrix_bytes < sizeof(double))
    return absl::InvalidArgumentError("Invalid native tracker tuning or resource bounds");
  return absl::OkStatus();
}
absl::StatusOr<std::unique_ptr<NativeTracker>> NativeTracker::Create(const TrackerConfig& config) {
  const auto valid = ValidateTrackerConfig(config);
  if (!valid.ok())
    return valid;
  return std::unique_ptr<NativeTracker>(new NativeTracker(std::make_unique<Impl>(config)));
}
void NativeTracker::Reset() {
  impl_->tracks.clear();
  impl_->last_sequence.reset();
  impl_->retired = false;
}
absl::StatusOr<std::vector<Observation>> NativeTracker::Update(
    uint64_t sequence,
    const std::vector<Proposal>& detections) {
  auto& impl = *impl_;
  const auto& config = impl.config;
  if (impl.retired)
    return absl::FailedPreconditionError("Reset native tracker after processing failure");
  if (sequence == std::numeric_limits<uint64_t>::max() || (impl.last_sequence && sequence != *impl.last_sequence + 1))
    return absl::InvalidArgumentError("Native tracker requires sequential frames or an explicit reset");
  if (detections.size() > config.maximum_tracks)
    return absl::ResourceExhaustedError("Detection count exceeds native tracker bound");
  for (const auto& p : detections)
    if (!Valid(p))
      return absl::InvalidArgumentError("Invalid native tracking detection");
  try {
    impl.tracks.erase(
        std::remove_if(
            impl.tracks.begin(),
            impl.tracks.end(),
            [&](const auto& t) {
              // Keep a track through the configured number of fully missed frames.
              return sequence - t.last_sequence - 1 > config.keep_lost_frames;
            }),
        impl.tracks.end());
    std::vector<size_t> confirmed, tentative, high, low;
    for (size_t t = 0; t < impl.tracks.size(); ++t) {
      impl.Predict(impl.tracks[t], sequence);
      (impl.tracks[t].confirmed ? confirmed : tentative).push_back(t);
    }
    for (size_t d = 0; d < detections.size(); ++d) {
      if (detections[d].confidence >= config.high_score)
        high.push_back(d);
      else if (detections[d].confidence >= config.low_score)
        low.push_back(d);
    }
    std::vector<bool> matched_tracks(impl.tracks.size()), matched_detections(detections.size());
    impl.Match(
        confirmed,
        high,
        detections,
        config.high_match_cost,
        config.weight_high_matches_by_score,
        sequence,
        matched_tracks,
        matched_detections);
    std::vector<size_t> active_unmatched;
    for (size_t t : confirmed)
      if (!matched_tracks[t] && impl.tracks[t].last_sequence + 1 == sequence)
        active_unmatched.push_back(t);
    impl.Match(
        active_unmatched, low, detections, config.low_match_cost, false, sequence, matched_tracks, matched_detections);
    high.erase(std::remove_if(high.begin(), high.end(), [&](size_t d) { return matched_detections[d]; }), high.end());
    impl.Match(
        tentative,
        high,
        detections,
        config.tentative_match_cost,
        config.weight_high_matches_by_score,
        sequence,
        matched_tracks,
        matched_detections);
    impl.tracks.erase(
        std::remove_if(
            impl.tracks.begin(),
            impl.tracks.end(),
            [&](const auto& t) { return !t.confirmed && t.last_sequence != sequence; }),
        impl.tracks.end());
    for (size_t d : high)
      if (!matched_detections[d] && detections[d].confidence >= config.new_track_score) {
        if (impl.tracks.size() == config.maximum_tracks)
          throw std::runtime_error("Native tracker capacity exhausted by live/lost tracks");
        if (impl.next_id == std::numeric_limits<uint64_t>::max())
          throw std::runtime_error("Native tracker ID exhausted");
        impl.tracks.push_back(
            {impl.next_id++,
             sequence,
             1,
             config.confirmation_frames == 1,
             detections[d],
             impl.filter.initiate(Measurement(detections[d]))});
      }
    impl.last_sequence = sequence;
    std::vector<Observation> result;
    for (const auto& t : impl.tracks)
      if (t.confirmed && t.last_sequence == sequence)
        result.push_back({t.id, t.box});
    return result;
  } catch (const std::exception& error) {
    impl.retired = true;
    return absl::InternalError(std::string("Native tracker: ") + error.what());
  }
}
} // namespace hm::tracking
