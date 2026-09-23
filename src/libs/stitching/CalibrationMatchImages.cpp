#include "hstream/src/libs/stitching/CalibrationMatchImages.h"

#include <algorithm>
#include <cmath>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

namespace hm::stitching {

absl::StatusOr<std::array<cv::Mat, 2>> MakeCalibrationMatchImages(
    const std::array<cv::Mat, 2>& images,
    const std::vector<FeatureMatch>& selected,
    const AkazeMatchingCalibration& calibration) {
  try {
    std::array<cv::Mat, 2> thumbnails;
    for (size_t camera = 0; camera < images.size(); ++camera) {
      const cv::Mat& image = images[camera];
      if (image.empty() || (image.depth() != CV_8U && image.depth() != CV_16U) ||
          (image.channels() != 1 && image.channels() != 3 && image.channels() != 4))
        return absl::InvalidArgumentError("Invalid calibration match image");
      const double scale = std::min(1.0, 1024.0 / std::max(image.cols, image.rows));
      cv::resize(image, thumbnails[camera], cv::Size(), scale, scale, cv::INTER_AREA);
      if (image.depth() == CV_16U)
        thumbnails[camera].convertTo(thumbnails[camera], CV_8U, 255.0 / 65535.0);
      if (image.channels() != 3)
        cv::cvtColor(
            thumbnails[camera], thumbnails[camera], image.channels() == 1 ? cv::COLOR_GRAY2BGR : cv::COLOR_BGRA2BGR);
    }
    cv::Mat canvas = cv::Mat::zeros(
        std::max(thumbnails[0].rows, thumbnails[1].rows), thumbnails[0].cols + thumbnails[1].cols, CV_8UC3);
    thumbnails[0].copyTo(canvas(cv::Rect(0, 0, thumbnails[0].cols, thumbnails[0].rows)));
    thumbnails[1].copyTo(canvas(cv::Rect(thumbnails[0].cols, 0, thumbnails[1].cols, thumbnails[1].rows)));
    std::array<cv::Mat, 2> result{canvas.clone(), canvas.clone()};
    const cv::Scalar green(0, 255, 0);
    for (const auto& match : selected) {
      std::array<cv::Point, 2> points;
      for (size_t camera = 0; camera < points.size(); ++camera) {
        cv::Point2f source = camera == 0 ? match.left : match.right;
        // Calibrated AKAZE returns rectified coordinates. Draw on the original
        // camera still by projecting those coordinates through the KB4 lens.
        const auto& lens = camera == 0 ? calibration.left : calibration.right;
        if (lens) {
          if (lens->resolution.width <= 0 || lens->resolution.height <= 0 || lens->fx <= 0 || lens->fy <= 0)
            return absl::InvalidArgumentError("Invalid calibration match lens");
          const double sx = static_cast<double>(images[camera].cols) / lens->resolution.width;
          const double sy = static_cast<double>(images[camera].rows) / lens->resolution.height;
          const double fx = lens->fx * sx, fy = lens->fy * sy, cx = lens->cx * sx, cy = lens->cy * sy;
          const std::vector<cv::Point2d> normalized{{(source.x - cx) / fx, (source.y - cy) / fy}};
          std::vector<cv::Point2d> distorted;
          cv::fisheye::distortPoints(
              normalized,
              distorted,
              cv::Matx33d(fx, 0, cx, 0, fy, cy, 0, 0, 1),
              cv::Vec4d(lens->distortion[0], lens->distortion[1], lens->distortion[2], lens->distortion[3]));
          source = distorted.front();
        }
        if (!std::isfinite(source.x) || !std::isfinite(source.y) || source.x < 0 || source.y < 0 ||
            source.x >= images[camera].cols || source.y >= images[camera].rows)
          return absl::InvalidArgumentError("Calibration match falls outside its source image");
        points[camera] = {
            std::min(thumbnails[camera].cols - 1, cvRound(source.x * thumbnails[camera].cols / images[camera].cols)) +
                (camera == 0 ? 0 : thumbnails[0].cols),
            std::min(thumbnails[camera].rows - 1, cvRound(source.y * thumbnails[camera].rows / images[camera].rows))};
      }
      cv::line(result[1], points[0], points[1], green, 1, cv::LINE_AA);
      for (auto& image : result) {
        for (const auto& point : points)
          cv::circle(image, point, 2, green, -1, cv::LINE_AA);
      }
    }
    return result;
  } catch (const cv::Exception& error) {
    return absl::InternalError("Cannot render calibration matches: " + std::string(error.what()));
  }
}

} // namespace hm::stitching
