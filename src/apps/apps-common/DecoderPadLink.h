#pragma once

#include <gst/gst.h>

namespace hm::gst {

// nvv4l2decoder can advertise featureless raw formats in a CAPS query while
// its exposed pad already owns fixed NVMM CAPS. An observational NVMM branch
// must not force a conversion solely to accommodate that query discrepancy.
// Retry only the no-format case, only for a caller-verified native decoder,
// and only when downstream accepts the exact negotiated NVMM format. Ordinary
// pad hierarchy/direction checks and subsequent CAPS negotiation remain active.
inline GstPadLinkReturn LinkDecodedPad(GstPad* source, GstPad* sink, bool native_nvmm_decoder) {
  const auto initial = gst_pad_link(source, sink);
  if (initial != GST_PAD_LINK_NOFORMAT || !native_nvmm_decoder)
    return initial;
  auto* caps = gst_pad_get_current_caps(source);
  const bool compatible = caps && gst_caps_is_fixed(caps) && gst_caps_get_size(caps) == 1 &&
      gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-raw") &&
      gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM") &&
      gst_pad_query_accept_caps(sink, caps);
  if (caps)
    gst_caps_unref(caps);
  return compatible ? gst_pad_link_full(source, sink, GST_PAD_LINK_CHECK_HIERARCHY) : initial;
}

} // namespace hm::gst
