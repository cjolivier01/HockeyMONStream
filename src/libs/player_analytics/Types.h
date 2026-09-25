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
inline constexpr uint8_t kNoColor = 255;
enum DrawingLayer : uint32_t { kDrawPlayerBoxes = 1, kDrawPose = 2, kDrawJerseys = 4, kDrawActions = 8 };

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

struct PlayerResult {
  uint64_t track_id{kUntrackedId};
  Box box;
  uint8_t color_slot{kNoColor};
  bool color_shared{false};
  bool has_pose{false};
  Pose pose;
  uint64_t pose_observed_at{kInvalidTime};
  JerseyResult jersey;
  ActionResult action;
  // Bounded printable display label from the actual model manifest, not a remap.
  std::array<char, 65> action_text{};
};

struct FrameResult {
  static constexpr uint32_t kSchemaVersion = 1;
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
