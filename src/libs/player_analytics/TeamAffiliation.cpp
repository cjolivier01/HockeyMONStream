#include "hstream/src/libs/player_analytics/TeamAffiliation.h"

#include <cmath>

namespace hm::player_analytics {
namespace {

float SquaredDistance(const float* a, const float* b) noexcept {
  float total = 0;
  for (size_t i = 0; i < kTeamEmbedding; ++i) {
    const float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}

}  // namespace

void TeamClusterer::Reset() noexcept {
  count_ = 0;
  cursor_ = 0;
  since_fit_ = 0;
  full_ = false;
  fitted_ = false;
  dark_cluster_ = 0;
}

void TeamClusterer::Fit() noexcept {
  const size_t n = count_;

  // Standardise per dimension. The embedding's axes carry very different
  // variances, and a raw Euclidean fit is dominated by whichever happens to be
  // widest rather than by the one separating the squads.
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    float sum = 0;
    for (size_t i = 0; i < n; ++i) {
      sum += samples_[i][d];
    }
    const float mean = sum / static_cast<float>(n);
    float variance = 0;
    for (size_t i = 0; i < n; ++i) {
      const float d0 = samples_[i][d] - mean;
      variance += d0 * d0;
    }
    variance /= static_cast<float>(n);
    mean_[d] = mean;
    const float deviation = std::sqrt(variance);
    inverse_deviation_[d] = deviation > 1e-6F ? 1.0F / deviation : 1.0F;
  }

  // Scratch for the standardised reservoir. Static extent, no allocation.
  static thread_local std::array<std::array<float, kTeamEmbedding>, kTeamReservoir> scaled;
  for (size_t i = 0; i < n; ++i) {
    for (size_t d = 0; d < kTeamEmbedding; ++d) {
      scaled[i][d] = (samples_[i][d] - mean_[d]) * inverse_deviation_[d];
    }
  }

  // Seed deterministically -- the point farthest from the mean, then the point
  // farthest from that. A random seed would make the two squads swap labels
  // between runs on identical input, and the overlay flicker with them.
  std::array<float, kTeamEmbedding> centre{};
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    float sum = 0;
    for (size_t i = 0; i < n; ++i) {
      sum += scaled[i][d];
    }
    centre[d] = sum / static_cast<float>(n);
  }
  size_t first = 0;
  float best = -1;
  for (size_t i = 0; i < n; ++i) {
    const float d = SquaredDistance(scaled[i].data(), centre.data());
    if (d > best) {
      best = d;
      first = i;
    }
  }
  size_t second = 0;
  best = -1;
  for (size_t i = 0; i < n; ++i) {
    const float d = SquaredDistance(scaled[i].data(), scaled[first].data());
    if (d > best) {
      best = d;
      second = i;
    }
  }
  centroids_[0] = scaled[first];
  centroids_[1] = scaled[second];

  // Lloyd's. Bounded iterations; this converges in a handful for k=2.
  static thread_local std::array<uint8_t, kTeamReservoir> membership;
  for (int iteration = 0; iteration < 25; ++iteration) {
    bool moved = false;
    for (size_t i = 0; i < n; ++i) {
      const float d0 = SquaredDistance(scaled[i].data(), centroids_[0].data());
      const float d1 = SquaredDistance(scaled[i].data(), centroids_[1].data());
      membership[i] = d0 <= d1 ? 0 : 1;
    }
    for (uint8_t k = 0; k < 2; ++k) {
      std::array<float, kTeamEmbedding> sum{};
      size_t members = 0;
      for (size_t i = 0; i < n; ++i) {
        if (membership[i] != k) {
          continue;
        }
        ++members;
        for (size_t d = 0; d < kTeamEmbedding; ++d) {
          sum[d] += scaled[i][d];
        }
      }
      if (members == 0) {
        // The seed pair collapsed. Leaving the centroid put beats reseeding
        // into an arbitrary point, which would not be reproducible.
        continue;
      }
      for (size_t d = 0; d < kTeamEmbedding; ++d) {
        const float value = sum[d] / static_cast<float>(members);
        if (std::fabs(value - centroids_[k][d]) > 1e-6F) {
          moved = true;
        }
        centroids_[k][d] = value;
      }
    }
    if (!moved) {
      break;
    }
  }

  // Rank the two groups by mean crop lightness so they can be named.
  std::array<double, 2> lightness{};
  std::array<size_t, 2> members{};
  for (size_t i = 0; i < n; ++i) {
    lightness[membership[i]] += lightness_[i];
    ++members[membership[i]];
  }
  const double mean0 = members[0] != 0 ? lightness[0] / static_cast<double>(members[0]) : 1e9;
  const double mean1 = members[1] != 0 ? lightness[1] / static_cast<double>(members[1]) : 1e9;
  dark_cluster_ = mean0 <= mean1 ? 0 : 1;

  fitted_ = true;
  since_fit_ = 0;
}

TeamAssignment TeamClusterer::Observe(const float* embedding, float lightness) noexcept {
  if (embedding == nullptr) {
    return {};
  }

  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    samples_[cursor_][d] = embedding[d];
  }
  lightness_[cursor_] = lightness;
  cursor_ = (cursor_ + 1) % kTeamReservoir;
  if (cursor_ == 0) {
    full_ = true;
  }
  count_ = full_ ? kTeamReservoir : cursor_;
  ++since_fit_;

  if (count_ >= kTeamWarmupSamples && (!fitted_ || since_fit_ >= kTeamRefitInterval)) {
    Fit();
  }
  if (!fitted_) {
    return {};
  }

  std::array<float, kTeamEmbedding> scaled{};
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    scaled[d] = (embedding[d] - mean_[d]) * inverse_deviation_[d];
  }
  const float d0 = std::sqrt(SquaredDistance(scaled.data(), centroids_[0].data()));
  const float d1 = std::sqrt(SquaredDistance(scaled.data(), centroids_[1].data()));
  const float total = d0 + d1;
  const float margin = total > 1e-6F ? std::fabs(d0 - d1) / total : 0.0F;
  if (margin < kTeamMinimumMargin) {
    return {};
  }
  return {static_cast<int8_t>(d0 <= d1 ? 0 : 1), margin};
}

std::array<TeamLabel, 2> NameClusters(int8_t dark_cluster, std::optional<bool> home_is_dark) noexcept {
  TeamLabel dark = TeamLabel::kDark;
  TeamLabel light = TeamLabel::kLight;
  if (home_is_dark.has_value()) {
    dark = *home_is_dark ? TeamLabel::kHome : TeamLabel::kAway;
    light = *home_is_dark ? TeamLabel::kAway : TeamLabel::kHome;
  }
  if (dark_cluster == 0) {
    return {dark, light};
  }
  return {light, dark};
}

const char* TeamLabelText(TeamLabel label) noexcept {
  switch (label) {
    case TeamLabel::kHome:
      return "HOME";
    case TeamLabel::kAway:
      return "AWAY";
    case TeamLabel::kDark:
      return "DARK";
    case TeamLabel::kLight:
      return "LIGHT";
    case TeamLabel::kUnknown:
      break;
  }
  return "";
}

}  // namespace hm::player_analytics
