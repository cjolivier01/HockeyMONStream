#pragma once

#include <gst/gst.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hm::gpu_preview {

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

} // namespace hm::gpu_preview
