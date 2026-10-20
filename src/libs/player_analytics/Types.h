#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hm::player_analytics {

inline constexpr size_t kMaximumTracks = 256;
inline constexpr size_t kMaximumDueRois = 32;
inline constexpr size_t kMaximumBatch = 8;
inline constexpr size_t kCocoJoints = 17;
inline constexpr size_t kActionSamples = 100;
inline constexpr uint64_t kSecond = 1000000000ULL;
inline constexpr uint64_t kInvalidTime = std::numeric_limits<uint64_t>::max();
inline constexpr uint64_t kUntrackedId = std::numeric_limits<uint64_t>::max();
inline constexpr uint64_t kActionSamplePeriod = 100000000ULL;
inline constexpr uint64_t kActionMaximumGap = 150000000ULL;
inline constexpr uint64_t kPoseDrawingMaximumAge = 250000000ULL;
inline constexpr uint8_t kNoColor = 255;
enum DrawingLayer : uint32_t {
  kDrawPlayerBoxes = 1,
  kDrawPose = 2,
  kDrawJerseys = 4,
  kDrawActions = 8,
  kDrawTeams = 16
};

// Embedding width produced by the team model. Fixed so per-track accumulation
// stays allocation-free; the contract rejects a model of any other width.
inline constexpr size_t kTeamEmbedding = 256;

// Embeddings retained toward the two-means fit. A recent window rather than
// the whole game: lighting drifts between periods, and the last few hundred
// crops describe the ice as it looks now. Fixed so nothing allocates.
inline constexpr size_t kTeamReservoir = 512;

// Two-means tunables. Every one of these is owned by configs/baseline.yaml and
// has no compiled-in default: ParseConfig refuses a team configuration that
// does not carry them, so one file answers what the shipped behaviour is.
struct TeamClusteringSettings {
  // Embeddings seen before the first fit. Fitting on a handful taken from one
  // end of the rink yields two centroids that both describe the same squad.
  size_t warmup_samples{0};
  // Embeddings between refits once fitted, so the split tracks slow drift.
  size_t refit_interval{0};
  // Normalized centroid-distance gap below which an embedding is left
  // unassigned rather than pushed onto whichever centroid is marginally
  // nearer. Goalies and partly-occluded crops land here.
  float minimum_margin{0};

  bool valid() const {
    return warmup_samples > 0 && warmup_samples <= kTeamReservoir && refit_interval > 0 && refit_interval <= 1000000 &&
        minimum_margin >= 0 && minimum_margin < 1;
  }
};

// All positions are pre-crop DeepStream metadata pixels, which may differ from
// NvBufSurface pixels. The inference adapter owns the per-axis conversion.
struct Keypoint {
  float x{0};
  float y{0};
  float confidence{0};
};

using Pose = std::array<Keypoint, kCocoJoints>;

struct Box {
  float left{0};
  float top{0};
  float width{0};
  float height{0};
};

struct JerseyResult {
  // Empty means unknown. Keep leading zeroes; at most two digits plus NUL.
  std::array<char, 3> text{};
  float confidence{0};
  float evidence{0};
  uint64_t observed_at{kInvalidTime};
  uint64_t expires_at{kInvalidTime};
};

struct ActionResult {
  int32_t label{-1};
  float confidence{0};
  uint64_t window_start{kInvalidTime};
  uint64_t observed_at{kInvalidTime};
  uint64_t expires_at{kInvalidTime};
};

// Display-only estimate in the current frame's coordinates. Never pose evidence.
struct RetainedPose {
  Pose pose{};
  uint64_t observed_at{kInvalidTime};
};

// Which side of the game a track plays for.
//
// Team identity is relative to the game being watched, so nothing predicts
// kHome directly: the clusterer splits skaters into two appearance groups,
// ranks those groups dark-against-light, and the per-game `home_is_dark`
// setting names them. With that setting absent the labels stay kDark/kLight,
// which is correct but unnamed, rather than an even bet on which side is home.
enum class TeamLabel : uint8_t { kUnknown = 0, kHome = 1, kAway = 2, kDark = 3, kLight = 4 };

struct TeamResult {
  TeamLabel label{TeamLabel::kUnknown};
  // Raw 0/1 cluster index before naming, -1 when unassigned. Kept so the
  // consensus votes on a stable quantity even if the dark/light ranking flips
  // while the centroids are still settling.
  int8_t cluster{-1};
  float evidence{0};
  uint64_t observed_at{kInvalidTime};
  uint64_t expires_at{kInvalidTime};
};

struct PlayerResult {
  uint64_t track_id{kUntrackedId};
  Box box;
  uint8_t color_slot{kNoColor};
  bool color_shared{false};
  bool has_pose{false};
  Pose pose;
  uint64_t pose_observed_at{kInvalidTime};
  RetainedPose retained_pose;
  JerseyResult jersey;
  ActionResult action;
  TeamResult team;
  // Bounded printable display label from the actual model manifest, not a remap.
  std::array<char, 65> action_text{};
};

struct FrameResult {
  // Version 4 adds TeamResult alongside the retained pose introduced in version 3.
  static constexpr uint32_t kSchemaVersion = 4;
  uint32_t schema_version{kSchemaVersion};
  uint32_t stream_id{0};
  uint64_t epoch{0};
  uint64_t sequence{0};
  uint64_t pts_ns{kInvalidTime};
  // Runtime assigns a token to the complete stitched generation, not just size.
  uint64_t geometry_token{0};
  float coordinate_width{0};
  float coordinate_height{0};
  uint32_t baked_layers{0};
  size_t player_count{0};
  std::array<PlayerResult, kMaximumTracks> players{};
};

} // namespace hm::player_analytics
