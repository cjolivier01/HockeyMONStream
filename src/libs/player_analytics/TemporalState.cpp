#include "hstream/src/libs/player_analytics/TemporalState.h"

#include <algorithm>
#include <cmath>

namespace hm::player_analytics {
namespace {

constexpr uint64_t kJerseyHalfLife = 2 * kSecond;
constexpr uint64_t kJerseyExpiry = 3 * kSecond;
constexpr uint64_t kActionExpiry = 2 * kSecond;

bool Probability(float value) {
  return std::isfinite(value) && value >= 0 && value <= 1;
}

uint64_t Deadline(uint64_t pts, uint64_t interval) {
  return pts > kInvalidTime - 1 - interval ? kInvalidTime - 1 : pts + interval;
}

} // namespace

TrackScheduler::TrackScheduler(size_t maximum_tracks, uint64_t retention_ns)
    : maximum_tracks_(std::min(maximum_tracks, kMaximumTracks)), retention_ns_(retention_ns) {}

void TrackScheduler::Reset() noexcept {
  tracks_ = {};
  epoch_ = 0;
  now_ = kInvalidTime;
  next_incarnation_ = 1;
  excluded_count_ = 0;
}

const TrackRecord* TrackScheduler::Find(uint64_t id) const noexcept {
  if (id == kUntrackedId)
    return nullptr;
  for (size_t i = 0; i < maximum_tracks_; ++i)
    if (tracks_[i].id == id)
      return &tracks_[i];
  return nullptr;
}

bool TrackScheduler::BeginFrame(uint64_t epoch, uint64_t pts_ns, const uint64_t* ids, size_t count) noexcept {
  if (pts_ns == kInvalidTime || (!ids && count)) {
    Reset();
    return false;
  }
  if (now_ != kInvalidTime && (epoch != epoch_ || pts_ns < now_))
    Reset();
  if (now_ == pts_ns)
    return false;
  epoch_ = epoch;
  now_ = pts_ns;
  std::array<uint64_t, kMaximumTracks> sorted{};
  const size_t inspected = std::min(count, sorted.size());
  excluded_count_ += count - inspected;
  size_t valid = 0;
  for (size_t i = 0; i < inspected; ++i)
    if (ids[i] != kUntrackedId)
      sorted[valid++] = ids[i];
  std::sort(sorted.begin(), sorted.begin() + valid);
  valid = std::unique(sorted.begin(), sorted.begin() + valid) - sorted.begin();
  for (size_t i = 0; i < maximum_tracks_; ++i) {
    auto& track = tracks_[i];
    if (track.id == kUntrackedId)
      continue;
    if (now_ - track.last_seen > retention_ns_) {
      track = {};
      continue;
    }
    track.visible = std::binary_search(sorted.begin(), sorted.begin() + valid, track.id);
    if (track.visible)
      track.last_seen = now_;
  }
  for (size_t i = 0; i < valid; ++i) {
    if (Find(sorted[i]))
      continue;
    auto free = std::find_if(tracks_.begin(), tracks_.begin() + maximum_tracks_, [](const TrackRecord& record) {
      return record.id == kUntrackedId;
    });
    if (free == tracks_.begin() + maximum_tracks_) {
      ++excluded_count_;
      continue;
    }
    *free = {};
    free->id = sorted[i];
    free->incarnation = next_incarnation_++;
    free->last_seen = now_;
    free->visible = true;
  }
  return true;
}

size_t TrackScheduler::SelectDue(
    Feature feature,
    uint64_t interval_ns,
    std::array<uint64_t, kMaximumDueRois>* output,
    size_t limit) const noexcept {
  const size_t f = static_cast<size_t>(feature);
  if (!output || f >= 3 || now_ == kInvalidTime || interval_ns == 0)
    return 0;
  std::array<const TrackRecord*, kMaximumTracks> due{};
  size_t count = 0;
  for (size_t i = 0; i < maximum_tracks_; ++i) {
    const auto& track = tracks_[i];
    if (track.id != kUntrackedId && track.visible &&
        (track.last_attempt[f] == kInvalidTime || now_ - track.last_attempt[f] >= interval_ns))
      due[count++] = &track;
  }
  std::sort(due.begin(), due.begin() + count, [f](const TrackRecord* a, const TrackRecord* b) {
    if (a->last_attempt[f] != b->last_attempt[f]) {
      if (a->last_attempt[f] == kInvalidTime)
        return true;
      if (b->last_attempt[f] == kInvalidTime)
        return false;
      return a->last_attempt[f] < b->last_attempt[f];
    }
    return a->id < b->id;
  });
  count = std::min({count, limit, output->size()});
  for (size_t i = 0; i < count; ++i)
    (*output)[i] = due[i]->id;
  return count;
}

bool TrackScheduler::MarkAttempt(Feature feature, uint64_t id) noexcept {
  const size_t f = static_cast<size_t>(feature);
  if (f >= 3 || now_ == kInvalidTime || id == kUntrackedId)
    return false;
  for (size_t i = 0; i < maximum_tracks_; ++i)
    if (tracks_[i].id == id && tracks_[i].visible) {
      tracks_[i].last_attempt[f] = now_;
      return true;
    }
  return false;
}

void JerseyConsensus::Reset() noexcept {
  evidence_ = {};
  observed_ = {};
  now_ = kInvalidTime;
  last_attempt_ = kInvalidTime;
  current_ = -1;
}

void JerseyConsensus::Advance(uint64_t now_ns) noexcept {
  if (now_ns == kInvalidTime) {
    Reset();
    return;
  }
  if (now_ != kInvalidTime && now_ns < now_)
    Reset();
  if (now_ != kInvalidTime) {
    const double decay = std::exp2(-static_cast<double>(now_ns - now_) / kJerseyHalfLife);
    for (size_t i = 0; i < evidence_.size(); ++i) {
      evidence_[i] = now_ns - observed_[i] > kJerseyExpiry ? 0 : evidence_[i] * decay;
      if (evidence_[i] < 0.0001)
        evidence_[i] = 0;
    }
  }
  now_ = now_ns;
  Select();
}

void JerseyConsensus::Select() noexcept {
  const int best = std::max_element(evidence_.begin(), evidence_.end()) - evidence_.begin();
  if (evidence_[best] < 1) {
    current_ = -1;
    return;
  }
  if (current_ < 0 || current_ == best || evidence_[best] >= evidence_[current_] * 1.35)
    current_ = best;
}

bool JerseyConsensus::Observe(uint64_t pts_ns, std::string_view text, float confidence, float minimum) noexcept {
  Advance(pts_ns);
  if (pts_ns == kInvalidTime || last_attempt_ == pts_ns)
    return false;
  last_attempt_ = pts_ns;
  if (!Probability(confidence) || !Probability(minimum) || confidence < minimum || text.empty() || text.size() > 2 ||
      !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return false;
  int index = text[0] - '0';
  if (text.size() == 2)
    index = 10 + index * 10 + text[1] - '0';
  evidence_[index] = std::min(1000.0, evidence_[index] + confidence);
  observed_[index] = pts_ns;
  Select();
  return true;
}

JerseyResult JerseyConsensus::Result(uint64_t now_ns) noexcept {
  Advance(now_ns);
  JerseyResult result;
  if (current_ < 0)
    return result;
  if (current_ < 10) {
    result.text[0] = '0' + current_;
  } else {
    result.text[0] = '0' + (current_ - 10) / 10;
    result.text[1] = '0' + (current_ - 10) % 10;
  }
  double total = 0;
  for (double value : evidence_)
    total += value;
  result.confidence = static_cast<float>(evidence_[current_] / total);
  result.evidence = static_cast<float>(evidence_[current_]);
  result.observed_at = observed_[current_];
  result.expires_at = Deadline(result.observed_at, kJerseyExpiry);
  return result;
}

void ActionHistory::Reset() noexcept {
  // Counts retire the ring without clearing a 20 KiB array on every gap.
  previous_pts_ = kInvalidTime;
  next_sample_ = kInvalidTime;
  width_ = height_ = 0;
  count_ = next_ = 0;
}

void ActionHistory::Append(uint64_t pts_ns, const Pose& pose) noexcept {
  samples_[next_] = pose;
  times_[next_] = pts_ns;
  next_ = (next_ + 1) % kActionSamples;
  count_ = std::min(count_ + 1, kActionSamples);
}

bool ActionHistory::Observe(uint64_t pts_ns, const Pose& pose, float width, float height) noexcept {
  if (pts_ns == kInvalidTime || !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
    Reset();
    return false;
  }
  if (previous_pts_ != kInvalidTime &&
      (pts_ns < previous_pts_ || pts_ns - previous_pts_ > kActionMaximumGap || width != width_ || height != height_))
    Reset();
  if (pts_ns == previous_pts_)
    return false;
  Pose normalized{};
  size_t confident = 0;
  for (size_t i = 0; i < pose.size(); ++i) {
    const auto& p = pose[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !Probability(p.confidence)) {
      Reset();
      return false;
    }
    if (p.confidence >= 0.3F) {
      normalized[i] = {2 * p.x / width - 1, 2 * p.y / height - 1, p.confidence};
      if (!std::isfinite(normalized[i].x) || !std::isfinite(normalized[i].y)) {
        Reset();
        return false;
      }
      ++confident;
    }
  }
  if (confident < 8 || pts_ns > kInvalidTime - 1 - kActionSamplePeriod) {
    Reset();
    return false;
  }
  if (previous_pts_ == kInvalidTime) {
    Append(pts_ns, normalized);
    next_sample_ = pts_ns + kActionSamplePeriod;
  } else {
    while (next_sample_ <= pts_ns) {
      const float weight = static_cast<float>(next_sample_ - previous_pts_) / (pts_ns - previous_pts_);
      Pose sample{};
      size_t valid_joints = 0;
      for (size_t i = 0; i < sample.size(); ++i) {
        const float confidence = next_sample_ == pts_ns ? normalized[i].confidence
                                                        : std::min(previous_[i].confidence, normalized[i].confidence);
        if (confidence > 0) {
          sample[i] = {
              previous_[i].x + weight * (normalized[i].x - previous_[i].x),
              previous_[i].y + weight * (normalized[i].y - previous_[i].y),
              confidence};
          ++valid_joints;
        }
      }
      if (valid_joints < 8) {
        Reset();
        return false;
      }
      Append(next_sample_, sample);
      next_sample_ += kActionSamplePeriod;
    }
  }
  previous_ = normalized;
  previous_pts_ = pts_ns;
  width_ = width;
  height_ = height;
  return true;
}

bool ActionHistory::ready(uint64_t now_ns) const noexcept {
  return count_ == kActionSamples && now_ns != kInvalidTime && previous_pts_ != kInvalidTime &&
      now_ns >= previous_pts_ && now_ns - previous_pts_ <= kActionMaximumGap;
}

uint64_t ActionHistory::window_start() const noexcept {
  return count_ ? times_[(next_ + kActionSamples - count_) % kActionSamples] : kInvalidTime;
}

uint64_t ActionHistory::window_end() const noexcept {
  return count_ ? times_[(next_ + kActionSamples - 1) % kActionSamples] : kInvalidTime;
}

bool ActionHistory::WriteTensor(uint64_t now_ns, float* output, size_t count) const noexcept {
  if (!output || count != kTensorFloats || !ready(now_ns))
    return false;
  std::fill(output, output + count, 0);
  size_t cursor = 0;
  for (size_t time = 0; time < kActionSamples; ++time) {
    const auto& pose = samples_[(next_ + time) % kActionSamples];
    for (const auto& point : pose) {
      output[cursor++] = point.x;
      output[cursor++] = point.y;
      output[cursor++] = point.confidence;
    }
  }
  return true;
}

void ActionLabelState::Reset() noexcept {
  current_ = {};
  last_attempt_ = kInvalidTime;
  contender_ = -1;
  contender_count_ = 0;
}

ActionResult ActionLabelState::Result(uint64_t now_ns) noexcept {
  if (now_ns == kInvalidTime || (last_attempt_ != kInvalidTime && now_ns < last_attempt_)) {
    Reset();
  } else if (current_.label >= 0 && now_ns > current_.expires_at) {
    current_ = {};
    contender_ = -1;
    contender_count_ = 0;
  }
  return current_;
}

bool ActionLabelState::Observe(
    uint64_t pts_ns,
    uint64_t window_start,
    int label,
    float confidence,
    float minimum) noexcept {
  Result(pts_ns);
  if (pts_ns == kInvalidTime || pts_ns == last_attempt_)
    return false;
  last_attempt_ = pts_ns;
  if (window_start == kInvalidTime || window_start > pts_ns ||
      pts_ns - window_start < (kActionSamples - 1) * kActionSamplePeriod || label < 0 || label >= 60 ||
      !Probability(confidence) || !Probability(minimum) || confidence < minimum) {
    contender_ = -1;
    contender_count_ = 0;
    return false;
  }
  if (label != contender_) {
    contender_ = label;
    contender_count_ = 0;
  }
  contender_count_ = std::min(contender_count_ + 1, size_t{2});
  if (current_.label < 0 || current_.label == label || confidence >= current_.confidence + 0.1F ||
      contender_count_ >= 2) {
    current_ = {label, confidence, window_start, pts_ns, Deadline(pts_ns, kActionExpiry)};
    return true;
  }
  return false;
}

} // namespace hm::player_analytics
