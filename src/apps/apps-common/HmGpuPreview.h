#pragma once

#include <gst/gst.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hm::stitching {
struct PlaybackRinkMap;
}

namespace hm::gpu_preview {

enum class RenderExceptionInjection {
  kNone,
  kBadAlloc,
  kLengthError,
  kUnknown,
};

enum class CallbackExceptionInjection {
  kNone,
  kBadAlloc,
  kLengthError,
  kUnknown,
};

enum class CallbackPoint {
  kIsolationChain,
  kIsolationChainList,
  kIsolationEvent,
  kIsolationQuery,
  kIsolationSetProperty,
  kIsolationGetProperty,
  kSinkStart,
  kSinkSetCaps,
  kSinkStop,
  kSinkSetProperty,
  kSinkGetProperty,
  kSinkUnlock,
  kSinkUnlockStop,
};

struct PreviewOverlayInspection {
  std::size_t path_count{0};
  bool diagnostic_coordinates_valid{false};
  std::vector<std::array<float, 4>> colors;
  std::size_t analytics_command_count{0};
  std::vector<std::array<float, 4>> disc_colors;
};

inline constexpr std::size_t kMaximumPresentedFrameCaptureBytes = 32U * 1024U * 1024U;

// Registers hmpreviewisolation and hmgpupreviewsink as process-local
// GStreamer factories. The renderer is available only on x86_64/X11.
void initialize_process();
bool register_elements();
bool renderer_available();

// hmpreviewisolation is a buffer gate and downstream-flow failure barrier.
void set_isolation_active(GstElement* isolation, bool active, std::uint64_t generation);
void set_isolation_generation(GstElement* isolation, std::uint64_t generation);
// Closes ingress automatically if a downstream drain barrier encounters a
// permanent flow failure, preventing an isolated failed preview from waking
// its queue indefinitely.
void set_isolation_failure_peer(GstElement* isolation, GstElement* ingress);
void set_renderer_generation(GstElement* sink, std::uint64_t generation);
bool isolation_active(GstElement* isolation);

// Sets the original upstream aspect ratio used for GL letterboxing after the
// bounded preview conversion. This does not touch video pixels.
void set_source_geometry(GstElement* sink, unsigned width, unsigned height);

// Waits for an in-flight render to finish and fences GL work before a target
// window can be destroyed. Returns false only when the sink is unavailable.
bool quiesce(GstElement* sink, std::uint64_t generation);

// The timestamp is acknowledged only after the GPU frame has been presented.
// It lets paused replay/stepping distinguish a newly rendered frame from the
// previous texture, without reading pixels back to the CPU.
bool presented_frame(GstElement* sink, std::uint64_t* generation, GstClockTime* pts);

// Preserves aspect ratio while enforcing the diagnostic readback budget.
// This calculation is exposed so callers and regression tests can reason
// about the dimensions returned by capture_presented_frame().
std::pair<unsigned, unsigned> bounded_capture_dimensions(unsigned width, unsigned height);

// Copies the most recently presented OpenGL texture to host memory for an
// explicit, one-shot diagnostic capture. Normal preview rendering never calls
// this function and remains entirely GPU-resident.
bool capture_presented_frame(
    GstElement* sink,
    std::vector<std::uint8_t>* rgba,
    unsigned* width,
    unsigned* height,
    std::string* error);

// Installs one-shot failures used to verify the GStreamer/GObject callback and
// explicit-capture exception boundaries. Production code never enables these
// hooks.
void set_render_exception_injection_for_test(GstElement* sink, RenderExceptionInjection injection);
void set_callback_exception_injection_for_test(
    GstElement* element,
    CallbackPoint point,
    CallbackExceptionInjection injection);
void set_capture_exception_injection_for_test(GstElement* sink, CallbackExceptionInjection injection);
// Next-run resolved preferences; layers are already gated by compute enables.
void configure_player_analytics(GstElement* sink, unsigned layers, float joint_confidence);
void configure_rink_mini_map(GstElement* sink, std::shared_ptr<const hm::stitching::PlaybackRinkMap> map);
struct RinkMaskImage;
// Explicit historical mask, validated/decoded once by the replay owner. Its
// canvas must match each frame's recorded coordinate space, including scaled archives.
void configure_recorded_rink_mask(GstElement* sink, std::shared_ptr<const RinkMaskImage> mask);
// For a paused replay with enable-last-sample=true. Reuses that one GPU buffer;
// never decodes/seeks or reads video pixels back to the CPU.
bool redraw_last_sample(GstElement* sink);
PreviewOverlayInspection inspect_preview_overlays_for_test(GstElement* sink, GstBuffer* buffer);
bool renderer_rink_mask_loaded_for_test(
    GstElement* sink,
    const std::string& output_generation,
    unsigned width,
    unsigned height);
bool renderer_rink_mask_cache_cleared_for_test(GstElement* sink);
bool renderer_resources_released_for_test(GstElement* sink);

} // namespace hm::gpu_preview
