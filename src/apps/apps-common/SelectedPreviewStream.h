#pragma once

#include <gst/gst.h>

#include <cstdint>
#include <functional>

namespace hm::preview {

// Metadata on admitted raw preview frames. The encoder adapter must preserve a
// bounded PTS-to-identity association; activation alone is not presentation.
struct FrameIdentityMeta {
  GstMeta meta;
  gchar* channel;
  std::uint64_t operation;
  std::uint64_t sequence;
  GstClockTime source_pts;
  GstClockTime presentation_pts;
};
const FrameIdentityMeta* frame_identity(GstBuffer* buffer);
// Copy only the logical identity onto an independently owned GPU output. Native
// layout/geometry metadata must not be blindly copied after scaling/letterbox.
bool copy_frame_identity(GstBuffer* output, GstBuffer* input);
// Borrowed source buffer, retained by the admitted frame's parent-buffer meta.
// Backend-specific consumers can inspect native metadata that has no generic
// copy transform without shortening the source pool's lifetime.
GstBuffer* source_frame(GstBuffer* buffer);

// No encoder or CPU video conversion lives here. Raw video memory is retained
// by reference, and the original buffer remains retained against pool reuse;
// only buffer headers/metadata receive the presentation clock.
// All three strings are required. Production NVIDIA consumers require NVMM;
// CPU fixtures explicitly request SystemMemory. The main-output route owns EOF,
// independently of which logical route is currently selected.
GstElement* selected_stream_create(
    const char* name,
    const char* stream_id,
    const char* memory_feature,
    const char* eof_channel);
// Returned pad is an owned reference. Routes are borrowed by the selector until
// remove_route or element destruction. Add/remove only while native ingress and
// drain gates are closed; remove before retiring their graph objects.
GstPad* selected_stream_add_route(GstElement* stream, const char* channel);
void selected_stream_remove_route(GstElement* stream, GstPad* pad);
// The route owner serializes operations. Closing then opening within the same
// operation is allowed; an older operation cannot change the selected route.
bool selected_stream_select(GstElement* stream, GstPad* pad, std::uint64_t operation);
bool selected_stream_active(GstElement* stream, GstPad* pad, std::uint64_t operation);
// Called under the same flow lock that assigns operation identities. False
// drops a stale native frame without failing the preview. The callback must
// only inspect metadata and must not reenter this selector or mutate its graph.
using FrameAdmission = std::function<bool(const char* channel, GstBuffer* original)>;
bool selected_stream_set_frame_admission(GstElement* stream, FrameAdmission admission);

} // namespace hm::preview
