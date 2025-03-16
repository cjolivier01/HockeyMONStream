#include "hstream/src/libs/camera/AutoFocus.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

#include <opencv2/opencv.hpp>

namespace hm {
namespace camera {
namespace {
// Focuser class to control the focus via an i2cset system call.
class Focuser {
 public:
  int bus{-1};
  int focus_value{0};
  static const int CHIP_I2C_ADDR = 0x0C;
  static const int OPT_BASE = 0x1000;
  static const int OPT_FOCUS = OPT_BASE | 0x01;
  // Allowed focus range for OPT_FOCUS.
  static const int MIN_VALUE = 0;
  static const int MAX_VALUE = 1000;
  static const int DEF_VALUE = 0;

  Focuser(int bus) : bus(bus), focus_value(0) {}

  int read() const {
    return focus_value;
  }

  // Write a focus value using the i2cset command.
  bool write(int chip_addr, int value, bool verbose) {
    if (value < 0) {
      value = 0;
    }
    focus_value = value;

    // Compute the register value: shift left 4 and mask to 10 bits.
    int reg_value = (value << 4) & 0x3FF0;
    int data1 = (reg_value >> 8) & 0x3F;
    int data2 = reg_value & 0xF0;

    std::stringstream cmd;
    cmd << "i2cset -y " << bus << " 0x" << std::hex << std::uppercase << chip_addr << " " << std::dec << data1 << " "
        << data2;
    std::string command = cmd.str();
    if (verbose) {
      std::cout << command << std::endl;
    }
    int rc = system(command.c_str());
    if (rc) {
      std::cerr << "Error running i2cset: " << strerror(errno) << std::endl;
      return false;
    }
    return true;
  }

  void reset(int /*opt*/, int flag = 1) {
    set(OPT_FOCUS, DEF_VALUE, flag);
  }

  int get(int /*opt*/, int flag = 0) {
    return read();
  }

  bool set(int /*opt*/, int value, bool verbose, int flag = 1) {
    if (value > MAX_VALUE) {
      value = MAX_VALUE;
    } else if (value < MIN_VALUE) {
      value = MIN_VALUE;
    }
    if (!write(CHIP_I2C_ADDR, value, verbose)) {
      return false;
    }
    if (verbose) {
      std::cout << "write: " << value << std::endl;
    }
    return true;
  }
};

// Helper function to set focus using the Focuser.
bool focusing(Focuser& focuser, int val, bool verbose) {
  return focuser.set(Focuser::OPT_FOCUS, val, verbose);
}

// Compute the focus measure using the Laplacian operator.
double laplacian(const cv::Mat& img) {
  cv::Mat gray;
  cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
  cv::Mat lap;
  cv::Laplacian(gray, lap, CV_16U);
  cv::Scalar meanVal = cv::mean(lap);
  return meanVal[0];
}

// Construct a GStreamer pipeline for capturing from the CSI camera.
std::string gstreamer_pipeline(
    int sensor_id,
    int capture_width,
    int capture_height,
    int display_width,
    int display_height,
    int framerate_n,
    int framerate_d,
    int flip_method) {
  char pipeline[1024];
  snprintf(
      pipeline,
      sizeof(pipeline),
      "nvarguscamerasrc sensor-id=%d ! "
      "video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, "
      "format=(string)NV12, framerate=(fraction)%d/%d ! "
      "nvvidconv flip-method=%d ! "
      "video/x-raw, width=(int)%d, height=(int)%d, format=(string)BGRx ! "
      "videoconvert ! "
      "video/x-raw, format=(string)BGR ! appsink",
      sensor_id,
      capture_width,
      capture_height,
      framerate_n,
      framerate_d,
      flip_method,
      display_width,
      display_height);
  return std::string(pipeline);
}

// Opens the camera stream, displays the image, and auto-adjusts the focus.
absl::Status show_camera(
    int device_id,
    Focuser& focuser,
    int capture_width,
    int capture_height,
    int fps_n,
    int fps_d,
    bool show,
    bool interactive,
    bool verbose) {
  int max_index = 10;
  double max_value = 0.0;
  double last_value = 0.0;
  int dec_count = 0;
  int focal_distance = 10;
  bool focus_finished = false;

  std::string pipeline = gstreamer_pipeline(
      device_id, capture_width, capture_height, capture_width / 2, capture_height / 2, fps_n, fps_d, 0);
  if (verbose) {
    std::cout << pipeline << std::endl;
  }

  cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);

  // Set an initial focus value.
  if (!focusing(focuser, focal_distance, verbose)) {
    return absl::InternalError("Could not focus camera");
  }
  int skip_frame = 6;

  if (cap.isOpened()) {
    auto cleanup_cv2 = absl::Cleanup([&cap, show]() {
      cap.release();
      if (show) {
        cv::destroyAllWindows();
      }
    });

    constexpr int kFocalDistanceIncrement = 4;

    if (show) {
      cv::namedWindow("CSI Camera", cv::WINDOW_AUTOSIZE);
    }
    std::cout << "Focusing camera sensor device " << device_id << std::flush;
    while (!show || cv::getWindowProperty("CSI Camera", cv::WND_PROP_AUTOSIZE) >= 0) {
      cv::Mat img;
      if (!cap.read(img)) {
        std::cerr << "Failed to capture frame." << std::endl;
        break;
      }
      if (show) {
        cv::imshow("CSI Camera", img);
      }

      if (skip_frame == 0) {
        skip_frame = 6;
        if (dec_count < 6 && focal_distance < 1000) {
          std::cout << '.' << std::flush;
          if (!focusing(focuser, focal_distance, verbose)) {
            return absl::InternalError("Could not focus camera");
          }
          double val = laplacian(img);
          if (val > max_value) {
            max_index = focal_distance;
            max_value = val;
          }
          if (val < last_value) {
            dec_count++;
          } else {
            dec_count = 0;
          }
          if (dec_count < 6) {
            last_value = val;
            focal_distance += kFocalDistanceIncrement;
          } else if (!focus_finished) {
            if (!focusing(focuser, max_index, verbose)) {
              return absl::InternalError("Could not focus camera");
            }
            focus_finished = true;
            std::cout << "Done." << std::endl;
          }
        }
      } else {
        skip_frame--;
      }
      const int keyCode = cv::waitKey(16) & 0xFF;
      if (interactive) {
        if (keyCode == 27) { // ESC key to exit
          break;
        } else if (keyCode == 10 || keyCode == 32) { // ENTER or SPACE resets focusing
          max_index = 10;
          max_value = 0.0;
          last_value = 0.0;
          dec_count = 0;
          focal_distance = 10;
          focus_finished = false;
        } else if (keyCode && keyCode != 255) {
          if (verbose) {
            std::cout << "keyCode = " << keyCode << std::endl;
          }
        }
      }
      if (!interactive && focus_finished) {
        break;
      }
    }
  } else {
    return absl::InternalError("Unable to open camera");
  }
  return absl::OkStatus();
}

struct AutoFocusCache {
  absl::Mutex mu;
  std::unordered_map<int, int> focused_sensors ABSL_GUARDED_BY(mu);
};
AutoFocusCache af_cache;

} // namespace

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
    bool force) {
  if (force) {
    absl::MutexLock lk(&af_cache.mu);
    if (af_cache.focused_sensors.count(sensor_id) && af_cache.focused_sensors.at(sensor_id) == i2c_bus) {
      return absl::OkStatus();
    }
  }
  Focuser focuser(i2c_bus);
  auto status = show_camera(sensor_id, focuser, width, height, fps_n, fps_d, show, interactive, verbose);
  if (status.ok()) {
    absl::MutexLock lk(&af_cache.mu);
    af_cache.focused_sensors[sensor_id] = i2c_bus;
  }
  return status;
}

} // namespace camera
} // namespace hm
