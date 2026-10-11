#pragma once

#include <gst/gst.h>

namespace hm {

// Arm after FLUSH_STOP, while upstream is stopped. For asynchronous base
// transforms such as nvinfer whose generate_output returns a cached push result
// and no buffer. Models and worker threads remain loaded across the seek.
bool ArmAsyncInferenceSeekRecovery(GstElement* element);

} // namespace hm
