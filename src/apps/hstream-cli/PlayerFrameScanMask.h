#pragma once

#include "absl/status/status.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::pipeline {

// Validate the actual mask metadata produced by Program's fieldmask plugin.
// An empty object list means empty ice only after this succeeds; inference
// skipping, no-op filtering and mismatched masks are never valid observations.
inline absl::Status ValidatePlayerFrameScanMask(const NvDsFrameMeta* frame) {
  const auto error = [](const std::string& message) {
    return absl::FailedPreconditionError("Player frame scan: " + message);
  };
  if (!frame || !frame->bInferDone)
    return error("a sampled frame did not run primary inference");
  const auto* mask = hm::UserApplicationPayload::get_payload<hm::fieldmask::FieldMaskPayload>(frame);
  const auto* output = stitching::find_stitched_output_generation_meta(frame);
  if (!mask || mask->mask().empty() || mask->mask().type() != CV_8UC1)
    return error("the ordinary rink-mask filter did not supply its mask");
  if (mask->mask().cols != static_cast<int>(frame->source_frame_width) ||
      mask->mask().rows != static_cast<int>(frame->source_frame_height))
    return error("rink-mask dimensions do not match the stitched frame");
  if (!output || output->generation().empty() ||
      mask->revision() != output->generation() + ":" + output->authorization_id())
    return error("rink-mask revision does not match the stitched output generation");
  return absl::OkStatus();
}

} // namespace hm::pipeline
