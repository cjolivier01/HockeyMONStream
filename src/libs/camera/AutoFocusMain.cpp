#include "hstream/src/libs/camera/AutoFocus.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  int i2c_bus = 2;
  int device_id = 0;
  int capture_width = 3840;
  int capture_height = 2160;
  int fps = 30;
  bool interactive = true;
  // Basic command-line argument parsing.
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-i" || arg == "--i2c-bus") {
      if (i + 1 < argc) {
        i2c_bus = std::stoi(argv[++i]);
      }
    } else if (arg == "-d" || arg == "--device-id") {
      if (i + 1 < argc) {
        device_id = std::stoi(argv[++i]);
      }
    } else if (arg == "--interative") {
      interactive = true;
    } else if (arg == "-w" || arg == "--width") {
      if (i + 1 < argc) {
        capture_width = std::stoi(argv[++i]);
      }
    } else if (arg == "--height") {
      if (i + 1 < argc) {
        capture_height = std::stoi(argv[++i]);
      }
    } else if (arg == "-f" || arg == "--fps") {
      if (i + 1 < argc) {
        fps = std::stoi(argv[++i]);
      }
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [-i i2c_bus] [-d device_id] [-w width] [--height height] [-f fps]"
                << std::endl;
      return 0;
    }
  }

  // Focuser focuser(i2c_bus);
  // show_camera(device_id, focuser, capture_width, capture_height, fps);
  absl::Status status = hm::camera::auto_focus_csi_camera(
      device_id,
      i2c_bus,
      capture_width,
      capture_height,
      fps,
      /*fps_d=*/1,
      /*show=*/true,
      interactive);
  if (!status.ok()) {
    std::cerr << status << std::endl;
  }
  return status.raw_code();
}
