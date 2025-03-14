#include "hstream/src/libs/camera/AutoFocus.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/opencv.hpp>

namespace hm {
namespace camera {
namespace {
// Focuser class to control the focus via an i2cset system call.
class Focuser {
 public:
  int bus;
  int focus_value;
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
  void write(int chip_addr, int value) {
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
    std::cout << command << std::endl;
    system(command.c_str());
  }

  void reset(int /*opt*/, int flag = 1) {
    set(OPT_FOCUS, DEF_VALUE, flag);
  }

  int get(int /*opt*/, int flag = 0) {
    return read();
  }

  void set(int /*opt*/, int value, int flag = 1) {
    if (value > MAX_VALUE) {
      value = MAX_VALUE;
    } else if (value < MIN_VALUE) {
      value = MIN_VALUE;
    }
    write(CHIP_I2C_ADDR, value);
    std::cout << "write: " << value << std::endl;
  }
};

// Helper function to set focus using the Focuser.
void focusing(Focuser& focuser, int val) {
  focuser.set(Focuser::OPT_FOCUS, val);
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
void show_camera(
    int device_id,
    Focuser& focuser,
    int capture_width,
    int capture_height,
    int fps_n,
    int fps_d,
    bool show,
    bool interactive) {
  int max_index = 10;
  double max_value = 0.0;
  double last_value = 0.0;
  int dec_count = 0;
  int focal_distance = 10;
  bool focus_finished = false;

  std::string pipeline = gstreamer_pipeline(
      device_id, capture_width, capture_height, capture_width / 2, capture_height / 2, fps_n, fps_d, 0);
  std::cout << pipeline << std::endl;

  cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
  // Set an initial focus value.
  focusing(focuser, focal_distance);
  int skip_frame = 6;

  if (cap.isOpened()) {
    cv::namedWindow("CSI Camera", cv::WINDOW_AUTOSIZE);
    while (cv::getWindowProperty("CSI Camera", cv::WND_PROP_AUTOSIZE) >= 0) {
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
          focusing(focuser, focal_distance);
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
            focal_distance += 10;
          } else if (!focus_finished) {
            focusing(focuser, max_index);
            focus_finished = true;
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
          std::cout << "keyCode = " << keyCode << std::endl;
        }
      }
      if (!interactive && focus_finished) {
        break;
      }
    }
    cap.release();
    cv::destroyAllWindows();
  } else {
    std::cerr << "Unable to open camera" << std::endl;
  }
}
} // namespace

absl::Status auto_focus_csi_camera(
    int sensor_id,
    int i2c_bus,
    int width,
    int height,
    int fps_n,
    int fps_d,
    bool show,
    bool interactive) {
  Focuser focuser(i2c_bus);
  show_camera(sensor_id, focuser, width, height, fps_n, fps_d, show, interactive);
  return absl::OkStatus();
}

} // namespace camera
} // namespace hm
