#pragma once

namespace hm::pipeline_internal {

// DeepStream selects the legacy or replacement nvstreammux implementation
// from this process environment before constructing the pipeline.
bool configure_streammux_runtime_environment();

} // namespace hm::pipeline_internal
