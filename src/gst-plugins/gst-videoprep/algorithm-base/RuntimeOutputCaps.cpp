#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/RuntimeOutputCaps.h"

namespace hm::videoprep {

namespace {

bool output_pad_is_terminal(GstBaseTransform* transform) {
  GstPad* src_pad = GST_BASE_TRANSFORM_SRC_PAD(transform);
  return GST_PAD_IS_FLUSHING(src_pad) || !gst_pad_is_linked(src_pad);
}

} // namespace

RuntimeOutputCapsUpdateResult update_runtime_output_caps(GstBaseTransform* transform, GstCaps* caps) {
  if (!transform || !caps) {
    return RuntimeOutputCapsUpdateResult::kFailed;
  }
  if (output_pad_is_terminal(transform)) {
    return RuntimeOutputCapsUpdateResult::kCancelled;
  }
  if (gst_base_transform_update_src_caps(transform, caps)) {
    return RuntimeOutputCapsUpdateResult::kUpdated;
  }
  // Pipeline teardown can race the CAPS event. Recheck the pad after the
  // failed update so a transition to flushing/unlinked is not reported as a
  // live negotiation error.
  return output_pad_is_terminal(transform) ? RuntimeOutputCapsUpdateResult::kCancelled
                                           : RuntimeOutputCapsUpdateResult::kFailed;
}

} // namespace hm::videoprep
