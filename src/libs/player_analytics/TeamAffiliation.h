#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

// Samples retained toward the two-means fit. A reservoir rather than the whole
// game: lighting drifts between periods, and the most recent window describes
// the ice as it looks now. Fixed so the accumulator never allocates.
inline constexpr size_t kTeamReservoir = 512;

// Embeddings seen before the first fit. Fitting on a handful taken from one
// end of the rink yields two centroids that both describe the same squad.
inline constexpr size_t kTeamWarmupSamples = 64;

// Embeddings between refits once fitted, so the split tracks slow drift
// without recomputing every frame.
inline constexpr size_t kTeamRefitInterval = 256;

// Normalised centroid-distance gap below which an embedding is left
// unassigned rather than pushed onto whichever centroid is marginally nearer.
// Goalies and partly-occluded crops land here.
inline constexpr float kTeamMinimumMargin = 0.12F;

struct TeamAssignment {
  // -1 when the split has not settled or the margin was too small to call.
  int8_t cluster{-1};
  float margin{0};
};

// Accumulates team embeddings for one game and splits them in two.
//
// Team identity is relative to the game being watched, so this fits fresh
// centroids per stream rather than loading a trained classifier: there is no
// transferable notion of "home" that a model could learn.
//
// Clustering input and the dark/light ranking are deliberately separate. The
// embedding has no photometric axis, so naming the two groups needs real crop
// lightness supplied alongside it.
class TeamClusterer {
 public:
  // `embedding` is kTeamEmbedding floats; `lightness` is the crop's mean
  // CIELAB L. Returns the assignment for this embedding under the centroids
  // in force after any refit this call triggers. No heap allocation.
  TeamAssignment Observe(const float* embedding, float lightness) noexcept;

  // Index of the lower-lightness cluster once fitted.
  std::optional<int8_t> dark_cluster() const noexcept {
    return fitted_ ? std::optional<int8_t>(dark_cluster_) : std::nullopt;
  }

  bool fitted() const noexcept {
    return fitted_;
  }

  size_t buffered() const noexcept {
    return count_;
  }

  void Reset() noexcept;

 private:
  void Fit() noexcept;

  std::array<std::array<float, kTeamEmbedding>, kTeamReservoir> samples_{};
  std::array<float, kTeamReservoir> lightness_{};
  std::array<std::array<float, kTeamEmbedding>, 2> centroids_{};
  std::array<float, kTeamEmbedding> mean_{};
  std::array<float, kTeamEmbedding> inverse_deviation_{};
  size_t count_{0};
  // Ring cursor once the reservoir is full, so the window stays recent.
  size_t cursor_{0};
  size_t since_fit_{0};
  bool full_{false};
  bool fitted_{false};
  int8_t dark_cluster_{0};
};

// Names the two clusters. `home_is_dark` absent means the caller did not say
// which sweater the home side wears, so the honest answer is kDark/kLight.
// Returns {label for cluster 0, label for cluster 1}.
std::array<TeamLabel, 2> NameClusters(int8_t dark_cluster, std::optional<bool> home_is_dark) noexcept;

// Short printable badge for a label: "HOME", "AWAY", "DARK", "LIGHT", or empty
// for kUnknown. The overlay draws nothing for an empty label rather than a
// placeholder, because an undecided track is better shown bare than wrong.
const char* TeamLabelText(TeamLabel label) noexcept;

}  // namespace hm::player_analytics
