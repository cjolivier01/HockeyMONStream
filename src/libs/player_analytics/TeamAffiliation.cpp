#include "hstream/src/libs/player_analytics/TeamAffiliation.h"

#include <algorithm>
#include <cmath>

namespace hm::player_analytics {
namespace {

// How closely the new split's axis must line up with the old one, as |cos|,
// before the pairing between them is believed. Not a tuning knob: it asks
// whether the question has an answer at all. Two fits of the same two squads
// separate them along nearly the same direction whatever the lighting has
// done; a fit of one squad split down the middle picks an arbitrary direction
// inside it, which is orthogonal to the squad axis and carries no information
// about which old cluster is which.
constexpr float kPairingAlignment = 0.5F;

float SquaredDistance(const float* a, const float* b) noexcept {
  float total = 0;
  for (size_t i = 0; i < kTeamEmbedding; ++i) {
    const float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}

} // namespace

// The centroids undone back to embedding space. Standardisation is refitted
// every time, so the stored centroids of two different fits are not comparable
// as they stand.
// Cosine between the two fits' separating axes. Positive means they number
// the groups the same way, negative means reversed, and near zero means the
// two fits are not describing the same division at all.
float TeamClusterer::Alignment(
    const std::array<std::array<float, kTeamEmbedding>, 2>& current,
    const std::array<std::array<float, kTeamEmbedding>, 2>& previous) noexcept {
  double dot = 0, current_norm = 0, previous_norm = 0;
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    const double a = current[0][d] - current[1][d], b = previous[0][d] - previous[1][d];
    dot += a * b;
    current_norm += a * a;
    previous_norm += b * b;
  }
  const double scale = std::sqrt(current_norm) * std::sqrt(previous_norm);
  return scale > 1e-12 ? static_cast<float>(dot / scale) : 0.0F;
}

std::array<std::array<float, kTeamEmbedding>, 2> TeamClusterer::RawCentroids() const noexcept {
  std::array<std::array<float, kTeamEmbedding>, 2> raw{};
  for (size_t k = 0; k < 2; ++k)
    for (size_t d = 0; d < kTeamEmbedding; ++d)
      raw[k][d] = centroids_[k][d] / inverse_deviation_[d] + mean_[d];
  return raw;
}

void TeamClusterer::Reset() noexcept {
  count_ = 0;
  cursor_ = 0;
  since_fit_ = 0;
  generation_ = 0;
  full_ = false;
  fitted_ = false;
  ranked_ = false;
  balanced_ = false;
  dark_cluster_ = 0;
  ranking_revision_ = 0;
  contrary_fits_ = 0;
  ranking_gap_ = 0;
}

void TeamClusterer::Fit() noexcept {
  const size_t n = count_;

  // Keep the previous split's identity before the standardisation that the
  // old centroids are expressed in is overwritten. Raw embedding space is the
  // only frame the two fits share.
  const bool refitting = fitted_;
  std::array<std::array<float, kTeamEmbedding>, 2> previous{};
  const auto previous_centroids = centroids_;
  const auto previous_mean = mean_;
  const auto previous_deviation = inverse_deviation_;
  if (refitting)
    previous = RawCentroids();

  // Standardise per dimension. The embedding's axes carry very different
  // variances, and a raw Euclidean fit is dominated by whichever happens to be
  // widest rather than by the one separating the squads.
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    // Accumulated in double: 512 finite floats can still sum past the float
    // range, and an infinite sum turns the whole standardisation into NaN.
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
      sum += samples_[i][d];
    }
    const double mean = sum / static_cast<double>(n);
    double variance = 0;
    for (size_t i = 0; i < n; ++i) {
      const double d0 = samples_[i][d] - mean;
      variance += d0 * d0;
    }
    variance /= static_cast<double>(n);
    mean_[d] = static_cast<float>(mean);
    const double deviation = std::sqrt(variance);
    inverse_deviation_[d] = deviation > 1e-6 ? static_cast<float>(1.0 / deviation) : 1.0F;
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

  // Lloyd's seeds from whatever is in the reservoir now, so nothing ties this
  // fit's index 0 to the last one's.
  std::array<size_t, 2> members{};
  for (size_t i = 0; i < n; ++i)
    ++members[membership[i]];

  // Two squads divide the ice roughly evenly. A lopsided split means the fit
  // separated something else, and the usual something else is the officials:
  // they are plain person detections here, and when the two sweaters are
  // close in appearance the squads merge and the stripes take the other
  // group. Badging every skater the same and the referees the opposite is
  // worse than saying nothing, so refuse the split rather than name it.
  const size_t smaller = std::min(members[0], members[1]);
  const bool balanced = static_cast<float>(smaller) >= settings_.minimum_cluster_share * static_cast<float>(n);

  if (refitting) {
    const auto current = RawCentroids();
    const float alignment = Alignment(current, previous);
    std::array<int, 2> previous_membership{};
    for (size_t k = 0; k < 2; ++k) {
      std::array<float, kTeamEmbedding> scaled_centroid{};
      for (size_t d = 0; d < kTeamEmbedding; ++d)
        scaled_centroid[d] = (current[k][d] - previous_mean[d]) * previous_deviation[d];
      previous_membership[k] = SquaredDistance(scaled_centroid.data(), previous_centroids[0].data()) <=
              SquaredDistance(scaled_centroid.data(), previous_centroids[1].data())
          ? 0
          : 1;
    }
    // Once a split is good enough to publish, protect it. A worse fit must not
    // replace it, and neither must one whose relationship to it cannot be
    // established: the reservoir goes single-squad for minutes at a time --
    // a settled track stops being sampled, so the window holds only whoever
    // is new or still undecided -- and k=2 then splits one squad down the
    // middle. Both new centroids land inside one old cluster and the pairing
    // becomes a coin flip, measured at 13 of 30 seeds, after which the two
    // squads wear a mix of badges. Keep the previous split and try again next
    // interval; stale centroids describe the ice far better than a fit of one
    // team pretending to be two.
    // Within-squad variation can align with the old squad axis too. In that
    // case both candidate centroids still belong to one old squad; alignment
    // alone cannot distinguish it from a genuine two-squad refit.
    if (ranked_ && balanced_ &&
        (!balanced || std::fabs(alignment) < kPairingAlignment || previous_membership[0] == previous_membership[1])) {
      centroids_ = previous_centroids;
      mean_ = previous_mean;
      inverse_deviation_ = previous_deviation;
      since_fit_ = 0;
      // This fit told us nothing, so it also breaks a run of contradictions:
      // "twice running" has to mean two fits good enough to judge, not two
      // separated by minutes of splits that were thrown away.
      contrary_fits_ = 0;
      return;
    }
    // Keep published indices using ownership in the previous fit's metric.
    // Raw-axis orientation alone can disagree with that metric when its
    // dimension scales differ. Provisional indices have no track evidence
    // to preserve, so only their axis orientation is carried forward.
    const bool reversed = ranked_ && balanced_ ? previous_membership[0] == 1 : alignment < 0;
    if (reversed) {
      std::swap(centroids_[0], centroids_[1]);
      std::swap(members[0], members[1]);
      for (size_t i = 0; i < n; ++i)
        membership[i] ^= 1;
    }
  }
  balanced_ = balanced;

  std::array<double, 2> lightness{};
  for (size_t i = 0; i < n; ++i)
    lightness[membership[i]] += lightness_[i];

  // Rank the two groups by mean crop lightness so they can be named. The
  // chest band takes in ice, boards and other skaters, so this mean carries
  // real noise; ranking on a gap smaller than that noise would land
  // differently at each refit and swap every badge on screen at once. The
  // centroid pairing above means index 0 still denotes the same squad across
  // refits, so a later fit measuring the opposite order is evidence that the
  // ranking in force is wrong, not that the squads changed sweaters.
  if (members[0] != 0 && members[1] != 0) {
    const double mean0 = lightness[0] / static_cast<double>(members[0]);
    const double mean1 = lightness[1] / static_cast<double>(members[1]);
    const double gap = std::fabs(mean0 - mean1);
    const int8_t order = mean0 <= mean1 ? 0 : 1;
    // Only rank from a split that is publishable. A lopsided fit separates
    // the officials from everyone else, and its lightness gap is large and
    // meaningless -- striped shirts against a merged field. Latching the
    // ranking from it and publishing that later, when the squads do separate,
    // inverts every badge for the rest of the game, measured at 10 of 20
    // seeds.
    if (!ranked_) {
      if (balanced_ && gap >= settings_.minimum_lightness_gap) {
        dark_cluster_ = order;
        ranked_ = true;
        ranking_gap_ = static_cast<float>(gap);
        ++ranking_revision_;
      }
    } else if (
        balanced_ && order != dark_cluster_ && gap >= settings_.minimum_lightness_gap * settings_.switch_hysteresis &&
        gap > static_cast<double>(ranking_gap_)) {
      // A ranking taken on a marginal gap can be wrong for the rest of the
      // game, so a later fit may overturn it -- but only on the terms the
      // per-track vote already uses for its own incumbent: beat the bar by
      // the switch margin, beat the separation the incumbent itself was taken
      // on, and say the same thing twice running. One window of bad crops
      // must not swap every badge on screen, and because each correction
      // raises the bar it set, the ranking cannot be walked back and forth.
      if (++contrary_fits_ >= 2) {
        dark_cluster_ = order;
        ranking_gap_ = static_cast<float>(gap);
        contrary_fits_ = 0;
        ++ranking_revision_;
      }
    } else {
      // Agreement, or a contradiction too weak to act on. Either way the
      // ranking in force stands: an uncertain fit is a reason to keep the
      // current answer, not to withdraw it and blank every badge.
      contrary_fits_ = 0;
    }
  }

  fitted_ = true;
  ++generation_;
  since_fit_ = 0;
}

TeamAssignment TeamClusterer::Observe(const float* embedding, float lightness) noexcept {
  if (embedding == nullptr || !settings_.valid() || !std::isfinite(lightness)) {
    return {};
  }
  // An FP16 overflow inside the network costs more than this observation: a
  // single non-finite component poisons the mean and deviation of its
  // dimension, and through them every assignment the reservoir ever produces.
  for (size_t d = 0; d < kTeamEmbedding; ++d) {
    if (!std::isfinite(embedding[d])) {
      return {};
    }
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

  if (count_ >= settings_.warmup_samples && (!fitted_ || since_fit_ >= settings_.refit_interval)) {
    Fit();
  }
  // Provisional fits may describe officials or variation within one squad.
  // They cannot supply stable cluster identities for track evidence, and
  // committing those votes would retire every track before a usable refit.
  if (!dark_cluster().has_value()) {
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
  // Written as a positive test so a NaN margin declines rather than passing a
  // `< threshold` check that is false for NaN. Failing open here would hand
  // the consensus an evidence value it can never recover from.
  if (!(margin >= settings_.minimum_margin)) {
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

} // namespace hm::player_analytics
