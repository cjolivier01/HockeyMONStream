#include "PipelineRuntimeEnvironment.h"

#include <cstdlib>

bool hm::pipeline_internal::configure_streammux_runtime_environment() {
  const char* configured = std::getenv("USE_NEW_NVSTREAMMUX");
  if (configured && *configured)
    return true;

  // DeepStream 9.1's legacy mux rejects the native 8K source caps used by
  // stitching. Preserve a nonempty caller override for older releases and
  // diagnostics, but make every direct hstream-cli launch safe by default.
  return ::setenv("USE_NEW_NVSTREAMMUX", "yes", /*overwrite=*/1) == 0;
}
