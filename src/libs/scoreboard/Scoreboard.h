#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/cudawarping.hpp>

#include <memory>
#include <vector>

namespace hm {
namespace scoreboard {

/**
 * @brief A class for applying perspective transforms to scoreboard images.
 *
 * This class orders the source points, computes a perspective transform matrix,
 * and applies the transform to a given image to extract the scoreboard region.
 */
class Scoreboard {
 public:
  /**
   * @brief Constructs a Scoreboard object.
   *
   * @param srcPts Vector of four source points (in any order; they will be ordered clockwise starting from top-left).
   * @param destWidth Desired output width.
   * @param destHeight Desired output height.
   * @param autoAspect If true, adjusts the output dimensions based on the source aspect ratio.
   * @param clipBox Optional pointer to a clipping rectangle; if provided, it is subtracted from the source points.
   */
  Scoreboard(
      const std::vector<cv::Point2f>& srcPts,
      int destWidth,
      int destHeight,
      bool autoAspect = true,
      const cv::Rect* clipBox = nullptr);

  /**
   * @brief Applies the perspective warp transformation to the input image.
   *
   * Extracts the region of interest, resizes it, applies the perspective transform,
   * and crops to the final output dimensions.
   *
   * @param inputImage The input image.
   * @return cv::Mat The warped image.
   */
  cv::Mat forward_cv(const cv::Mat& inputImage);
  cv::Mat forward_cuda(const cv::Mat& inputImage);

  /**
   * @brief Gets the final output width.
   *
   * @return int The width of the warped image.
   */
  int getWidth() const;

  /**
   * @brief Gets the final output height.
   *
   * @return int The height of the warped image.
   */
  int getHeight() const;

 private:
  /**
   * @brief Orders four points in clockwise order starting from the top-left.
   *
   * This method uses the sum and difference of coordinates to determine the order.
   *
   * @param pts Vector of four points.
   * @return std::vector<cv::Point2f> The points in clockwise order.
   */
  static std::vector<cv::Point2f> orderPointsClockwise(const std::vector<cv::Point2f>& pts);

  /**
   * @brief Computes the Euclidean distance between two points.
   *
   * @param pt0 First point.
   * @param pt1 Second point.
   * @return float The Euclidean distance.
   */
  static float pointDistance(const cv::Point2f& pt0, const cv::Point2f& pt1);

  std::vector<cv::Point2f> srcPts_; ///< Source points in clockwise order.
  cv::Rect bboxSrc_; ///< Bounding box of the source points.
  int destWidth_; ///< Final output width.
  int destHeight_; ///< Final output height.
  int destW_; ///< Intermediate width (possibly scaled).
  int destH_; ///< Intermediate height (possibly scaled).
  cv::Mat perspectiveMatrix_; ///< Perspective transformation matrix.
  std::unique_ptr<cv::cuda::GpuMat> warped_image_scratch_buffer_;
};

} // namespace scoreboard
} // namespace hm
