#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/RuntimeOutputCaps.h"

namespace hm::videoprep {

bool update_runtime_output_caps(GstBaseTransform* transform, GstCaps* caps) {
  return transform && caps && gst_base_transform_update_src_caps(transform, caps);
}

} // namespace hm::videoprep
