#pragma once

#include "absl/status/status.h"

#include <vector>

namespace hm {
namespace camera {

struct CameraConnection {
  int sensor_id{0};
  int i2c_bus{0};
  int width{0};
  int height{0};
  int fps_n{0};
  int fps_d{0};
};

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

absl::Status auto_focus_cameras(
    const std::vector<CameraConnection>& cameras,
    bool show,
    bool interactive,
    bool verbose,
    bool force = false);

} // namespace camera
} // namespace hm
