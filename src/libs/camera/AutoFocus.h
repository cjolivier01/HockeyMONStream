#pragma once

#include "absl/status/status.h"

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
    bool interactive);

}
} // namespace hm
