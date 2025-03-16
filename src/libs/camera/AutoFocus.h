#pragma once

#include "absl/status/status.h"

#include <unordered_map>
namespace hm {
namespace camera {

absl::Status auto_focus_csi_camera(
    int sensor_id,
    int i2c_bus,
    int width,
    int height,
    int fps_n,
    int fps_d,
    bool show,
    bool interactive,
    bool verbose,
    bool force = false);

} // namespace camera
} // namespace hm
