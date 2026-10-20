#include "hstream/src/libs/player_analytics/TeamAffiliation.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using hm::player_analytics::kTeamEmbedding;
using hm::player_analytics::NameClusters;
using hm::player_analytics::TeamClusterer;
using hm::player_analytics::TeamClusteringSettings;
using hm::player_analytics::TeamLabel;
using hm::player_analytics::TeamLabelText;

// Mirrors the shipped configs/baseline.yaml team block. The clusterer has no
// defaults of its own, so a test must say what tuning it is exercising.
constexpr TeamClusteringSettings kSettings{200, 600, 0.12F, 6.0F, 0.25F, 1.0, 1.35, 60.0F};

bool Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
  }
  return condition;
}

// Deterministic pseudo-random so a failure is reproducible.
uint32_t Next(uint32_t& state) noexcept {
  state = state * 1664525U + 1013904223U;
  return state;
}

float Jitter(uint32_t& state, float scale) noexcept {
  return (static_cast<float>(Next(state) >> 8) / 16777216.0F - 0.5F) * scale;
}

// Two separated squads in embedding space: one centred at -1 on the first
// half of the axes, one at +1, both with the same small jitter everywhere.
std::array<float, kTeamEmbedding> MakeEmbedding(uint32_t& state, bool second_squad, float spread) {
  std::array<float, kTeamEmbedding> out{};
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    const float centre = (d < kTeamEmbedding / 2) ? (second_squad ? 1.0F : -1.0F) : 0.0F;
    out[d] = centre + Jitter(state, spread);
  }
  return out;
}

bool SeparatesTwoSquads() {
  TeamClusterer clusterer(kSettings);
  uint32_t state = 7;
  // Dark squad gets low lightness, light squad high, so the ranking is known.
  for (int i = 0; i < 200; ++i) {
    const bool second = (i % 2) == 1;
    const auto embedding = MakeEmbedding(state, second, 0.2F);
    clusterer.Observe(embedding.data(), second ? 80.0F : 20.0F);
  }
  bool ok = Check(clusterer.fitted(), "clusterer fits once warmed up");
  ok &= Check(clusterer.dark_cluster().has_value(), "dark cluster identified");

  int agree = 0;
  int assigned = 0;
  for (int i = 0; i < 100; ++i) {
    const bool second = (i % 2) == 1;
    const auto embedding = MakeEmbedding(state, second, 0.2F);
    const auto result = clusterer.Observe(embedding.data(), second ? 80.0F : 20.0F);
    if (result.cluster < 0) {
      continue;
    }
    ++assigned;
    // The dark cluster must be the low-lightness squad.
    const bool is_dark = result.cluster == *clusterer.dark_cluster();
    if (is_dark == !second) {
      ++agree;
    }
  }
  ok &= Check(assigned > 90, "well-separated embeddings are assigned, not declined");
  ok &= Check(agree == assigned, "every assignment matches the squad that produced it");
  return ok;
}

bool DeclinesBeforeWarmup() {
  TeamClusterer clusterer(kSettings);
  uint32_t state = 11;
  bool ok = true;
  for (size_t i = 0; i < kSettings.warmup_samples - 1; ++i) {
    const auto embedding = MakeEmbedding(state, i % 2 == 1, 0.2F);
    ok &= Check(clusterer.Observe(embedding.data(), 50.0F).cluster < 0, "no assignment before the warmup threshold");
  }
  ok &= Check(!clusterer.fitted(), "not fitted before the warmup threshold");
  return ok;
}

bool DeclinesAmbiguousEmbeddings() {
  TeamClusterer clusterer(kSettings);
  uint32_t state = 13;
  for (int i = 0; i < 200; ++i) {
    const auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.2F);
    clusterer.Observe(embedding.data(), (i % 2) == 1 ? 80.0F : 20.0F);
  }
  // Exactly between the two squads: equidistant, so the margin collapses.
  std::array<float, kTeamEmbedding> midpoint{};
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    midpoint[d] = (d < kTeamEmbedding / 2) ? 0.0F : 0.0F;
  }
  const auto result = clusterer.Observe(midpoint.data(), 50.0F);
  return Check(result.cluster < 0, "an embedding between the centroids is declined, not guessed");
}

bool IsDeterministic() {
  // A random seed would swap the two squads' labels between runs on identical
  // input and make the overlay flicker.
  std::vector<int8_t> first;
  std::vector<int8_t> second;
  for (int run = 0; run < 2; ++run) {
    TeamClusterer clusterer(kSettings);
    uint32_t state = 29;
    std::vector<int8_t>& into = run == 0 ? first : second;
    for (int i = 0; i < 200; ++i) {
      const auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.2F);
      into.push_back(clusterer.Observe(embedding.data(), (i % 2) == 1 ? 80.0F : 20.0F).cluster);
    }
  }
  return Check(first == second, "identical input produces an identical split");
}

bool NamesClusters() {
  bool ok = true;
  auto unnamed = NameClusters(0, std::nullopt);
  ok &= Check(
      unnamed[0] == TeamLabel::kDark && unnamed[1] == TeamLabel::kLight,
      "without home_is_dark the labels stay dark/light");

  auto home_dark = NameClusters(0, true);
  ok &= Check(
      home_dark[0] == TeamLabel::kHome && home_dark[1] == TeamLabel::kAway, "home_is_dark names the dark cluster home");

  auto home_light = NameClusters(0, false);
  ok &= Check(
      home_light[0] == TeamLabel::kAway && home_light[1] == TeamLabel::kHome,
      "home_is_light names the light cluster home");

  // The mapping follows the dark cluster, not the index.
  auto flipped = NameClusters(1, true);
  ok &= Check(
      flipped[0] == TeamLabel::kAway && flipped[1] == TeamLabel::kHome, "naming follows whichever cluster is darker");
  return ok;
}

bool RendersLabels() {
  bool ok = true;
  ok &= Check(std::string_view(TeamLabelText(TeamLabel::kHome)) == "HOME", "home badge text");
  ok &= Check(std::string_view(TeamLabelText(TeamLabel::kLight)) == "LIGHT", "light badge text");
  ok &= Check(
      std::string_view(TeamLabelText(TeamLabel::kUnknown)).empty(),
      "an undecided track renders no badge rather than a placeholder");
  return ok;
}

bool SurvivesNullEmbedding() {
  TeamClusterer clusterer(kSettings);
  return Check(clusterer.Observe(nullptr, 50.0F).cluster < 0, "a null embedding is declined");
}

// The bug this guards: Lloyd's reseeds from the current reservoir, so a refit
// can hand squad A the index squad B had. A track that already settled keeps
// voting for its stored index and silently changes sides -- permanently, since
// a settled track is never sampled again.
bool RefitKeepsClusterIdentity() {
  // Refit often relative to warmup so the test actually crosses a refit.
  constexpr TeamClusteringSettings kFrequent{64, 64, 0.12F, 6.0F, 0.25F, 1.0, 1.35, 60.0F};
  bool ok = true;
  // Several seeds: a single one can pass by luck, and the failure was
  // intermittent (measured at roughly half of refits before the fix).
  for (uint32_t seed = 1; seed <= 12; ++seed) {
    TeamClusterer clusterer(kFrequent);
    uint32_t state = seed;
    for (int i = 0; i < 80; ++i) {
      const auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.2F);
      clusterer.Observe(embedding.data(), (i % 2) == 1 ? 80.0F : 20.0F);
    }
    const auto settled = MakeEmbedding(state, false, 0.2F);
    const auto before = clusterer.Observe(settled.data(), 20.0F);
    ok &= Check(before.cluster >= 0, "the first squad is assigned before the refit");
    const auto dark_before = clusterer.dark_cluster();

    // Drift the whole distribution the way changing ice lighting would, which
    // is what refitting exists to track, then cross several refits.
    for (int i = 0; i < 400; ++i) {
      auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.25F);
      for (size_t d = 0; d < kTeamEmbedding; ++d)
        embedding[d] += 0.15F;
      clusterer.Observe(embedding.data(), (i % 2) == 1 ? 78.0F : 22.0F);
    }
    const auto after = clusterer.Observe(settled.data(), 20.0F);
    ok &= Check(after.cluster >= 0, "the same squad is still assigned after the refit");
    ok &= Check(
        after.cluster == before.cluster,
        "a refit renamed the clusters, so every settled track changed sides (seed " + std::to_string(seed) + ")");
    ok &= Check(clusterer.dark_cluster() == dark_before, "the dark cluster followed the rename");
  }
  return ok;
}

bool DeclinesNonFiniteEmbeddings() {
  TeamClusterer clusterer(kSettings);
  uint32_t state = 17;
  for (int i = 0; i < 200; ++i) {
    const auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.2F);
    clusterer.Observe(embedding.data(), (i % 2) == 1 ? 80.0F : 20.0F);
  }
  bool ok = Check(clusterer.fitted(), "fitted before the poisoned sample");
  const size_t buffered = clusterer.buffered();

  // An FP16 overflow in the network must not reach the reservoir: it would
  // poison the mean and deviation of its dimension and every later assignment.
  auto poisoned = MakeEmbedding(state, false, 0.2F);
  poisoned[7] = std::numeric_limits<float>::infinity();
  ok &= Check(clusterer.Observe(poisoned.data(), 20.0F).cluster < 0, "a non-finite embedding is declined");
  auto nan_sample = MakeEmbedding(state, true, 0.2F);
  nan_sample[200] = std::numeric_limits<float>::quiet_NaN();
  ok &= Check(clusterer.Observe(nan_sample.data(), 80.0F).cluster < 0, "a NaN embedding is declined");
  const auto clean = MakeEmbedding(state, false, 0.2F);
  ok &= Check(
      clusterer.Observe(clean.data(), std::numeric_limits<float>::quiet_NaN()).cluster < 0,
      "a NaN lightness is declined");
  ok &= Check(clusterer.buffered() == buffered, "declined samples never enter the reservoir");

  // And the split still works afterwards.
  int agree = 0, assigned = 0;
  for (int i = 0; i < 40; ++i) {
    const bool second = (i % 2) == 1;
    const auto embedding = MakeEmbedding(state, second, 0.2F);
    const auto result = clusterer.Observe(embedding.data(), second ? 80.0F : 20.0F);
    if (result.cluster < 0)
      continue;
    ++assigned;
    if ((result.cluster == *clusterer.dark_cluster()) == !second)
      ++agree;
  }
  ok &= Check(assigned > 30 && agree == assigned, "the reservoir survived the rejected samples intact");
  return ok;
}

// The bug this guards: the dark/light ranking was recomputed from scratch at
// every refit with no minimum separation. The chest band takes in ice, boards
// and other skaters, so when the two sweaters are close in lightness the
// ranking is a coin flip -- and because the overlay reads it fresh every
// frame, every badge on screen swapped HOME for AWAY together, repeatedly.
bool RankingIsStableAndHonest() {
  constexpr TeamClusteringSettings kFrequent{64, 64, 0.12F, 6.0F, 0.25F, 1.0, 1.35, 60.0F};
  bool ok = true;
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    // Two squads that are genuinely distinct in embedding space but nearly
    // identical in lightness: white against pale grey.
    TeamClusterer clusterer(kFrequent);
    uint32_t state = seed;
    std::vector<std::optional<int8_t>> ranks;
    for (int i = 0; i < 600; ++i) {
      const bool second = (i % 2) == 1;
      const auto embedding = MakeEmbedding(state, second, 0.2F);
      // Half a lightness unit apart, swamped by a 12-unit per-crop spread.
      clusterer.Observe(embedding.data(), (second ? 60.5F : 60.0F) + Jitter(state, 12.0F));
      if (clusterer.fitted())
        ranks.push_back(clusterer.dark_cluster());
    }
    for (size_t i = 1; i < ranks.size(); ++i)
      ok &= Check(
          ranks[i] == ranks[i - 1],
          "the dark/light ranking changed mid-game, so every badge on screen swapped (seed " + std::to_string(seed) +
              ")");
  }

  // A real separation is still ranked, and the ranking is still the dark one.
  TeamClusterer clear(kFrequent);
  uint32_t state = 101;
  for (int i = 0; i < 400; ++i) {
    const bool second = (i % 2) == 1;
    const auto embedding = MakeEmbedding(state, second, 0.2F);
    clear.Observe(embedding.data(), second ? 80.0F : 20.0F);
  }
  ok &= Check(clear.dark_cluster().has_value(), "a 60-unit lightness gap went unranked");
  const auto dark = *clear.dark_cluster();
  const auto probe = MakeEmbedding(state, false, 0.2F);
  ok &= Check(clear.Observe(probe.data(), 20.0F).cluster == dark, "the dark cluster is not the darker squad");
  return ok;
}

// Officials are plain person detections natively, so when the two sweaters are
// close the fit can separate the stripes from everyone else instead of the two
// squads. Naming that split badges every skater the same and the referees the
// opposite, confidently. Refuse it.
bool RefusesALopsidedSplit() {
  constexpr TeamClusteringSettings kFrequent{64, 64, 0.12F, 6.0F, 0.25F, 1.0, 1.35, 60.0F};
  TeamClusterer clusterer(kFrequent);
  uint32_t state = 5;
  for (int i = 0; i < 400; ++i) {
    // One crop in eight is an official: a third appearance that is far from
    // both squads, which are themselves nearly identical.
    const bool official = (i % 8) == 0;
    std::array<float, kTeamEmbedding> embedding{};
    for (size_t d = 0; d < kTeamEmbedding; ++d) {
      const float centre = official ? 4.0F : ((d < kTeamEmbedding / 2) ? ((i % 2) ? 0.02F : -0.02F) : 0.0F);
      embedding[d] = centre + Jitter(state, 0.2F);
    }
    clusterer.Observe(embedding.data(), official ? 50.0F : 20.0F + (i % 2) * 60.0F);
  }
  bool ok = Check(clusterer.fitted(), "the clusterer still fits a lopsided population");
  ok &= Check(
      !clusterer.dark_cluster().has_value(),
      "a split that isolated one small group was named as if it were the two squads");
  return ok;
}

bool ResetClearsState() {
  TeamClusterer clusterer(kSettings);
  uint32_t state = 31;
  for (int i = 0; i < 200; ++i) {
    const auto embedding = MakeEmbedding(state, (i % 2) == 1, 0.2F);
    clusterer.Observe(embedding.data(), 50.0F);
  }
  bool ok = Check(clusterer.fitted(), "fitted before reset");
  clusterer.Reset();
  ok &= Check(!clusterer.fitted() && clusterer.buffered() == 0, "reset clears the reservoir");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= SeparatesTwoSquads();
  ok &= RefitKeepsClusterIdentity();
  ok &= DeclinesNonFiniteEmbeddings();
  ok &= RankingIsStableAndHonest();
  ok &= RefusesALopsidedSplit();
  ok &= DeclinesBeforeWarmup();
  ok &= DeclinesAmbiguousEmbeddings();
  ok &= IsDeterministic();
  ok &= NamesClusters();
  ok &= RendersLabels();
  ok &= SurvivesNullEmbedding();
  ok &= ResetClearsState();
  if (ok) {
    std::cout << "team affiliation tests passed\n";
  }
  return ok ? 0 : 1;
}
