#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hm::draw_display::analytics {

inline constexpr uint32_t kMaximumCommands = 8192;
inline constexpr uint32_t kMaximumGlyphs = 1024;
inline constexpr uint32_t kMaximumActiveTiles = 32768;
inline constexpr uint32_t kMaximumTileReferences = 262144;
inline constexpr size_t kMaximumUploadBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaximumAtlasBytes = 256 * 1024;
inline constexpr uint32_t kTileSize = 16;

struct Color {
  float red{0}, green{0}, blue{0}, alpha{1};
};
enum class PixelFormat { kRgba8, kRgb10A2 };
struct ImageView {
  void* data{nullptr};
  size_t pitch{0};
  uint32_t width{0}, height{0};
  // Packed R is bits 0..9, G 10..19, B 20..29, A 30..31.
  PixelFormat format{PixelFormat::kRgba8};
};

namespace detail {
enum class Kind : uint32_t { kLine, kDisc, kRectangle, kFill, kGlyph };
struct Command {
  float x0, y0, x1, y1;
  Color color;
  float radius;
  Kind kind;
  uint32_t glyph, reserved;
};
struct Tile {
  uint32_t x, y, first_reference, reference_count;
};
} // namespace detail

// Lazy CPU-only storage; construction, Clear(), and invisible (alpha=0) drawing
// allocate nothing. Add in priority order: rejected commands do not evict earlier
// ones. Text is atomic, printable ASCII, at most 64 characters; caller owns labels.
// Coordinates are already transformed to output pixels. No metadata/color state.
class CommandList {
 public:
  explicit CommandList(uint32_t capacity = kMaximumCommands, uint32_t glyph_capacity = kMaximumGlyphs);
  void Clear() noexcept;
  bool AddLine(float x0, float y0, float x1, float y1, float width, Color color);
  bool AddDisc(float x, float y, float radius, Color color);
  // Rectangle is [left,right) x [top,bottom), with an inward border.
  bool AddRectangle(float left, float top, float right, float bottom, float width, Color color);
  bool AddFill(float left, float top, float right, float bottom, Color color);
  // Text occupies fixed monospace cells: advance = 2/3 pixel_height. y is top,
  // not baseline. Spaces advance without emitting a glyph. Font atlas is fixed.
  bool AddText(float x, float y, float pixel_height, std::string_view text, Color color);
  size_t size() const noexcept {
    return commands_.size();
  }
  bool empty() const noexcept {
    return commands_.empty();
  }
  uint64_t rejected() const noexcept {
    return rejected_;
  }
  const detail::Command* data() const noexcept {
    return commands_.data();
  }

 private:
  bool Add(detail::Command command);
  std::vector<detail::Command> commands_;
  uint32_t capacity_, glyph_capacity_, glyphs_{0};
  uint64_t rejected_{0};
};

struct Limits {
  uint32_t active_tiles{kMaximumActiveTiles};
  uint32_t tile_references{kMaximumTileReferences};
  size_t upload_bytes{kMaximumUploadBytes};
};
enum class RenderStatus { kOk, kCapacity, kBusy, kInvalidArgument, kFontUnavailable, kCudaError };
struct RenderResult {
  RenderStatus status{RenderStatus::kOk};
  cudaError_t cuda_error{cudaSuccess};
  uint32_t commands{0}, active_tiles{0}, tile_references{0};
  size_t upload_bytes{0}, glyph_upload_bytes{0};
  bool launched{false};
};
struct Counters {
  uint64_t renders{0}, empty_renders{0}, raster_launches{0};
  uint64_t device_allocations{0}, host_allocations{0}, event_creations{0};
  uint64_t device_bytes{0}, pinned_host_bytes{0};
  uint64_t h2d_calls{0}, h2d_bytes{0}, glyph_upload_bytes{0};
  uint64_t capacity_suppressions{0}, busy_suppressions{0}, invalid_requests{0};
};
// Optional externally owned timing events for tests/profilers. Created/destroyed
// by the caller; Render only records them on its own stream for launched frames.
struct TimingEvents {
  cudaEvent_t before_upload{nullptr}, before_raster{nullptr}, after_raster{nullptr};
};

// One instance, one calling thread, one CUDA device and stream (bound on first
// nonempty render). The image is BORROWED, OWNED writable Program output; it must
// remain alive through caller-stream completion. Never call on a shared tee input.
// CPU commands may be cleared immediately after Render returns: async H2D reads
// compositor-owned pinned slots, retained until their completion events fire.
// Three slots bound in-flight uploads. Busy/capacity suppress ONLY the overlay,
// never the video frame; counters make suppression visible. No device/stream sync.
// Caller MUST complete its stream before destruction, including after an error,
// and destroy with that device current. No GPU/staging resources exist before demand.
// A CUDA error retires the instance; recreate it after the caller handles/drains
// the failed stream. This prevents reuse after a failed asynchronous submission.
// Font raster/upload happens once, lazily on first visible glyph. Optional path
// selects one trusted installed monospace TTF; no per-label fonts or font discovery.
class Compositor {
 public:
  explicit Compositor(Limits limits = {}, const char* font_path = nullptr);
  ~Compositor();
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;
  RenderResult Render(
      const ImageView& image,
      const CommandList& commands,
      cudaStream_t stream,
      const TimingEvents* timing = nullptr);
  const Counters& counters() const noexcept {
    return counters_;
  }

 private:
  struct State;
  std::unique_ptr<State> state_;
  Limits limits_;
  std::string font_path_;
  Counters counters_;
};

} // namespace hm::draw_display::analytics
