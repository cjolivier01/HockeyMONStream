#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

struct TeamAssignment {
  // -1 when the split cannot be named or the margin was too small to call.
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
  // Settings are owned by configs/baseline.yaml; there is no default
  // constructor, so no caller can quietly invent its own tuning. An invalid
  // settings block leaves the clusterer inert rather than guessing.
  explicit TeamClusterer(const TeamClusteringSettings& settings) noexcept : settings_(settings) {}

  // `embedding` is kTeamEmbedding floats; `lightness` is the crop's mean
  // CIELAB L. Returns the assignment for this embedding under the centroids
  // in force after any refit this call triggers, only once the fit is balanced
  // and ranked. Provisional fits never contribute track votes. No heap allocation.
  TeamAssignment Observe(const float* embedding, float lightness) noexcept;

  // Index of the lower-lightness cluster, once a fit has separated the two
  // groups by enough crop lightness to tell them apart and the split is
  // balanced enough to be two squads rather than the officials against
  // everyone. Absent means the overlay names nobody, which is the honest
  // answer; a guessed ranking would flip at the next refit and take every
  // badge on screen with it. Once established it is never withdrawn, only
  // replaced by a later fit that contradicts it convincingly.
  std::optional<int8_t> dark_cluster() const noexcept {
    return ranked_ && balanced_ ? std::optional<int8_t>(dark_cluster_) : std::nullopt;
  }

  // Bumped by each refit. A track that committed under an older generation is
  // worth re-reading: the centroids it was judged against have moved.
  uint32_t generation() const noexcept {
    return generation_;
  }

  // Bumped when the dark/light ranking is first established and again each
  // time a later fit overturns it. Callers watch this to report a correction;
  // it does not change on an ordinary refit that agrees with the ranking.
  uint32_t ranking_revision() const noexcept {
    return ranking_revision_;
  }

  // Mean-lightness gap, in CIELAB L, that the current ranking was taken on.
  float ranking_gap() const noexcept {
    return ranking_gap_;
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
  std::array<std::array<float, kTeamEmbedding>, 2> RawCentroids() const noexcept;
  static float Alignment(
      const std::array<std::array<float, kTeamEmbedding>, 2>& current,
      const std::array<std::array<float, kTeamEmbedding>, 2>& previous) noexcept;

  TeamClusteringSettings settings_;

  std::array<std::array<float, kTeamEmbedding>, kTeamReservoir> samples_{};
  std::array<float, kTeamReservoir> lightness_{};
  std::array<std::array<float, kTeamEmbedding>, 2> centroids_{};
  std::array<float, kTeamEmbedding> mean_{};
  std::array<float, kTeamEmbedding> inverse_deviation_{};
  size_t count_{0};
  // Ring cursor once the reservoir is full, so the window stays recent.
  size_t cursor_{0};
  size_t since_fit_{0};
  uint32_t generation_{0};
  bool full_{false};
  uint32_t ranking_revision_{0};
  // Consecutive accepted fits whose lightness order contradicts the ranking by
  // more than the switch margin. One such fit is noise; the correction waits.
  uint8_t contrary_fits_{0};
  // How many of those a correction needs. It grows with each correction, so
  // sustained evidence can always win but a ranking cannot be walked back and
  // forth cheaply. Capped so recovery stays reachable in a long game.
  uint8_t required_confirmations_{2};
  float ranking_gap_{0};
  bool fitted_{false};
  bool ranked_{false};
  bool balanced_{false};
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

} // namespace hm::player_analytics
