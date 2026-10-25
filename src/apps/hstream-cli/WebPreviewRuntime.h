#pragma once

#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"

#include <gst/gst.h>

namespace hm::preview {

// Route/overlay mutations may drain streaming callbacks. A paused sink can
// retain one of those callbacks until Resume, which is read on this same
// control thread. Never enter that drain while playback is paused/transitioning.
// The caller serializes runtime commands and graph ownership on the main loop.
inline std::string WebPreviewMutationBlockReason(GstElement* pipeline) {
  if (!pipeline)
    return "preview-unavailable";
  GstState current = GST_STATE_NULL;
  GstState pending = GST_STATE_VOID_PENDING;
  const auto result = gst_element_get_state(pipeline, &current, &pending, 0);
  if (result == GST_STATE_CHANGE_FAILURE)
    return "preview-unavailable";
  if (result == GST_STATE_CHANGE_ASYNC || pending != GST_STATE_VOID_PENDING)
    return "preview-transitioning";
  if (current == GST_STATE_PAUSED)
    return "preview-paused";
  return current == GST_STATE_PLAYING ? std::string{} : "preview-unavailable";
}

inline RouteOwner::Result SelectWebPreviewRoute(
    GstElement* pipeline,
    RouteOwner& owner,
    const RouteOwner::Request& request,
    const RouteOwner::SelectionChanged& changed) {
  if (!(request.session == owner.session()))
    return {RouteOwner::Status::kWrongSession, "stale-session"};
  const auto reason = WebPreviewMutationBlockReason(pipeline);
  if (!reason.empty())
    return {RouteOwner::Status::kFailed, reason};
  return owner.select(request, changed);
}

} // namespace hm::preview
