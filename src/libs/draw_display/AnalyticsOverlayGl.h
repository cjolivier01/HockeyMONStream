#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

namespace hm::draw_display::analytics {

inline constexpr uint64_t kMaximumGlFragmentCandidates = 64 * 1024 * 1024;
enum class GlRenderStatus { kOk, kCapacity, kInvalidArgument, kFontUnavailable, kGlError };
struct GlRenderResult {
  GlRenderStatus status{GlRenderStatus::kOk};
  unsigned gl_error{0};
  uint32_t commands{0};
  size_t upload_bytes{0};
  bool drawn{false};
};
struct GlCounters {
  uint64_t renders{0}, empty_renders{0}, draws{0}, buffer_allocations{0}, buffer_bytes{0};
  uint64_t atlas_uploads{0}, atlas_bytes{0}, vertex_uploads{0}, vertex_bytes{0}, capacity_suppressions{0};
};

// x86 GLX compatibility context, OpenGL 2.1/GLSL 1.20. One calling thread and
// context per instance, bound lazily at first visible render. Renders into the
// caller's CURRENT framebuffer/viewport with top-left output-pixel coordinates.
// Destination must be opaque and use linear blending (as in HmGpuPreview's
// video/black framebuffer; no GL_FRAMEBUFFER_SRGB conversion).
// No surface copy, framebuffer readback, glFinish, or context changes occur.
// Empty lists do not allocate or call any GL/GLX API. Commands are copied into
// bounded reusable vertices; the caller may clear them immediately on return.
// State changed by Render is restored. Fixed VBOs rotate across three frames;
// GL owns upload lifetime and may wait internally if the GPU falls behind.
// Destroy with the owning context current. If the context cannot be made current,
// destruction leaves GL names for exclusive owner-context destruction to reclaim;
// it never deletes names in an unrelated context. Do not share this context's
// objects with another context if relying on that fallback.
// A GL error retires the instance. Capacity/font failure suppress only overlay.
class GlCompositor {
 public:
  explicit GlCompositor(const char* font_path = nullptr);
  ~GlCompositor();
  GlCompositor(const GlCompositor&) = delete;
  GlCompositor& operator=(const GlCompositor&) = delete;
  GlRenderResult Render(float coordinate_width, float coordinate_height, const CommandList& commands);
  const GlCounters& counters() const noexcept {
    return counters_;
  }

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::string font_path_;
  GlCounters counters_;
};

} // namespace hm::draw_display::analytics
