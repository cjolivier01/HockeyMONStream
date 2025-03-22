#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include "cupano/pano/cudaMat.h"
#include "cupano/pano/showImage.h"

#include "jetson-utils/cuda/cudaResizeRoi.h"
#include "jetson-utils/cuda/cudaWarp.h"
#include "jetson-utils/cuda/cudaWarpPerspective.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
// #include <opencv2/cudawarping.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <nppi.h>

#include <cuda_runtime.h>

namespace hm {
namespace scoreboard {

/**
 * @brief Computes the Euclidean distance between two points.
 */
float Scoreboard::pointDistance(const cv::Point2f& pt0, const cv::Point2f& pt1) {
  return cv::norm(pt0 - pt1);
}

/**
 * @brief Orders four points in clockwise order starting from the top-left.
 */
std::vector<cv::Point2f> Scoreboard::orderPointsClockwise(const std::vector<cv::Point2f>& pts) {
  if (pts.size() != 4) {
    throw std::runtime_error("orderPointsClockwise: exactly 4 points are required.");
  }

  std::vector<cv::Point2f> ordered(4);
  std::vector<float> sumPts, diffPts;
  for (const auto& pt : pts) {
    sumPts.push_back(pt.x + pt.y);
    diffPts.push_back(pt.x - pt.y);
  }

  // Top-left has the smallest sum.
  int tlIdx = std::min_element(sumPts.begin(), sumPts.end()) - sumPts.begin();
  // Bottom-right has the largest sum.
  int brIdx = std::max_element(sumPts.begin(), sumPts.end()) - sumPts.begin();
  // Top-right has the smallest difference.
  int trIdx = std::min_element(diffPts.begin(), diffPts.end()) - diffPts.begin();
  // Bottom-left has the largest difference.
  int blIdx = std::max_element(diffPts.begin(), diffPts.end()) - diffPts.begin();

  ordered[0] = pts[tlIdx];
  ordered[1] = pts[trIdx];
  ordered[2] = pts[brIdx];
  ordered[3] = pts[blIdx];

  return ordered;
}

/**
 * @brief Constructs a Scoreboard object.
 */
Scoreboard::Scoreboard(
    const std::vector<cv::Point2f>& srcPts,
    int destWidth,
    int destHeight,
    bool autoAspect,
    const cv::Rect* clipBox)
    : destWidth_(destWidth), destHeight_(destHeight) {
  if (srcPts.size() != 4) {
    throw std::runtime_error("Scoreboard: exactly 4 source points required.");
  }

  // Order the points in clockwise order.
  // srcPts_ = orderPointsClockwise(srcPts);
  srcPts_ = srcPts;

  // Adjust the source points if a clipping rectangle is provided.
  if (clipBox != nullptr) {
    for (auto& pt : srcPts_) {
      pt.x -= clipBox->x;
      pt.y -= clipBox->y;
    }
  }

  // Compute the bounding box of the source points.
  float minX = srcPts_[0].x, minY = srcPts_[0].y;
  float maxX = srcPts_[0].x, maxY = srcPts_[0].y;
  for (const auto& pt : srcPts_) {
    minX = std::min(minX, pt.x);
    minY = std::min(minY, pt.y);
    maxX = std::max(maxX, pt.x);
    maxY = std::max(maxY, pt.y);
  }
  bboxSrc_ = cv::Rect(cv::Point2f(minX, minY), cv::Point2f(maxX, maxY));

  // Adjust the source points relative to the bounding box.
  for (auto& pt : srcPts_) {
    pt.x -= bboxSrc_.x;
    pt.y -= bboxSrc_.y;
  }

  float srcWidth = bboxSrc_.width;
  float srcHeight = bboxSrc_.height;

  // Adjust output dimensions based on the source aspect ratio if autoAspect is true.
  if (autoAspect) {
    float wTop = pointDistance(srcPts_[0], srcPts_[1]);
    float wBottom = pointDistance(srcPts_[3], srcPts_[2]); // bottom-left to bottom-right
    float hLeft = pointDistance(srcPts_[0], srcPts_[3]);
    float hRight = pointDistance(srcPts_[1], srcPts_[2]);
    float wAvg = (wTop + wBottom) / 2.0f;
    float hAvg = (hLeft + hRight) / 2.0f;
    float aspectRatio = wAvg / hAvg;
    int destWidthNew = static_cast<int>(destHeight_ * aspectRatio);
    int destHeightNew = static_cast<int>(destWidth_ / aspectRatio);
    // Choose the adjustment that causes the least relative change.
    if (std::fabs(destWidth_ - destWidthNew) / float(destWidth_) <
        std::fabs(destHeight_ - destHeightNew) / float(destHeight_)) {
      destWidth_ = destWidthNew;
    } else {
      destHeight_ = destHeightNew;
    }
  }

  // Determine intermediate scaling dimensions.
  int totW = std::max(destWidth_, static_cast<int>(srcWidth));
  int totH = std::max(destHeight_, static_cast<int>(srcHeight));
  if (totW > srcWidth || totH > srcHeight) {
    float ratioW = totW / srcWidth;
    float ratioH = totH / srcHeight;
    destW_ = totW;
    destH_ = totH;
    for (auto& pt : srcPts_) {
      pt.x *= ratioW;
      pt.y *= ratioH;
    }
  } else {
    destW_ = static_cast<int>(srcWidth);
    destH_ = static_cast<int>(srcHeight);
  }

  // Define destination points: top-left, top-right, bottom-right, bottom-left.
  std::vector<cv::Point2f> dstPts;
  dstPts.push_back(cv::Point2f(0, 0)); // top-left
  dstPts.push_back(cv::Point2f(destWidth_ - 1, 0)); // top-right
  dstPts.push_back(cv::Point2f(destWidth_ - 1, destHeight_ - 1)); // bottom-right
  dstPts.push_back(cv::Point2f(0, destHeight_ - 1)); // bottom-left

 /**
  * Calculates perspective transform coefficients given source rectangular ROI
  * and its destination quadrangle projection
  *
  * \param oSrcROI Source ROI
  * \param quad Destination quadrangle
  * \param aCoeffs Perspective transform coefficients
  * \return Error codes:
  *         - NPP_SIZE_ERROR Indicates an error condition if any image dimension
  *           has zero or negative value
  *         - NPP_RECTANGLE_ERROR Indicates an error condition if width or height of
  *           the intersection of the oSrcROI and source image is less than or
  *           equal to 1
  *         - NPP_COEFFICIENT_ERROR Indicates an error condition if coefficient values
  *           are invalid
  */
  // NppStatus status = 
  // nppiGetPerspectiveTransform(NppiRect oSrcROI, const double quad[4][2], double aCoeffs[3][3]);



  // Compute the perspective transform matrix.
  perspectiveMatrix_ = cv::getPerspectiveTransform(srcPts_, dstPts, int(cv::DECOMP_LU) | int(cv::DECOMP_NORMAL));
  assert(perspectiveMatrix_.cols == 3);
  assert(perspectiveMatrix_.rows == 3);
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      fperspectiveMatrix_[i][j] = perspectiveMatrix_.at<float>(i, j);
      dperspectiveMatrix_[i][j] = perspectiveMatrix_.at<float>(i, j);
    }
  }
}

/**
 * @brief Applies the perspective warp transformation to the input image.
 */
cv::Mat Scoreboard::forward_cv(const cv::Mat& inputImage) {
  // Extract the region of interest using the computed bounding box.
  cv::Mat srcImage = inputImage(bboxSrc_).clone();

  // Resize the source image to the intermediate dimensions.
  cv::Mat resizedImage;
  cv::resize(srcImage, resizedImage, cv::Size(destW_, destH_), 0, 0, cv::INTER_NEAREST);

  // Apply the perspective transformation.
  cv::Mat warpedImage;
  cv::warpPerspective(resizedImage, warpedImage, perspectiveMatrix_, cv::Size(destW_, destH_), cv::INTER_LINEAR);

  // Crop the warped image to the final desired dimensions.
  cv::Rect cropRect(0, 0, destWidth_, destHeight_);
  if (cropRect.x + cropRect.width > warpedImage.cols || cropRect.y + cropRect.height > warpedImage.rows) {
    throw std::runtime_error("Warped image size is smaller than destination size.");
  }
  cv::Mat finalImage = warpedImage(cropRect).clone();
  return finalImage;
}

cv::Mat Scoreboard::forward_cuda(const cv::Mat& inputImage) {
  cudaError_t cuErr = cudaError_t::cudaSuccess;
  cuErr = cudaSetDevice(0);
  cudaStream_t stream;
  cuErr = cudaStreamCreate(&stream);

  hm::CudaMat<uchar3> full_image(inputImage);
  // hm::CudaMat<uchar3> roi_image(/*B=*/1, bboxSrc_.width, bboxSrc_.height);
  hm::CudaMat<uchar3> resized_image(/*B=*/1, destW_, destH_);
  hm::CudaMat<uchar3> warped_image(/*B=*/1, destW_, destH_);

  // cuErr = cudaCrop(
  //     full_image.data(),
  //     roi_image.data(),
  //     {bboxSrc_.x, bboxSrc_.y, bboxSrc_.x + bboxSrc_.width, bboxSrc_.y + bboxSrc_.height},
  //     full_image.width(),
  //     full_image.height(),
  //     stream);

  // SHOW_IMAGE(&roi_image);

  // Extract the region of interest using the computed bounding box.
  cv::Mat srcImage = inputImage(bboxSrc_).clone();

  // Resize the source image to the intermediate dimensions.
  cv::Mat resizedImage;
  cv::resize(srcImage, resizedImage, cv::Size(destW_, destH_), 0, 0, cv::INTER_NEAREST);
  // cv::imshow("resizedImage", resizedImage);
  // cv::waitKey(0);

  cuErr = cudaResizeROI(
      full_image.data(),
      full_image.width(),
      full_image.height(),
      bboxSrc_.x,
      bboxSrc_.y,
      bboxSrc_.width,
      bboxSrc_.height,
      resized_image.data(),
      resized_image.width(),
      resized_image.height(),
      /*dstX=*/0,
      /*dstY=*/0,
      /*dstWidth=*/resized_image.width(),
      /*dstHeight=*/resized_image.height(),
      cudaFilterMode::FILTER_POINT,
      stream);
  // SHOW_IMAGE(&resized_image);

  // template<typename T>
  // cudaError_t cudaWarpPerspective( T* input, uint32_t inputWidth, uint32_t inputHeight,
  //                                  T* output, uint32_t outputWidth, uint32_t outputHeight,
  //                                  const float transform[3][3], bool transform_inverted=false,
  //                                  cudaStream_t stream=0 )

  // nppiWarpPerspective_8u_C3R(
  //     resized_image.data_raw(),
  //     {resized_image.width(), resized_image.height()},
  //     resized_image.pitch(),
  //     {0, 0, resized_image.width(), resized_image.height()},
  //     warped_image.data_raw(),
  //     warped_image.pitch(),
  //     {0, 0, warped_image.width(), warped_image.height()},
  //     dperspectiveMatrix_,
  //     NPPI_INTER_LINEAR);

  cuErr = cudaWarpPerspective(
      resized_image.data(),
      resized_image.width(),
      resized_image.height(),
      warped_image.data(),
      warped_image.width(),
      warped_image.height(),
      fperspectiveMatrix_,
      /*transform_inverted=*/false,
      stream);
  SHOW_IMAGE(&warped_image);

  // Apply the perspective transformation.
  cv::Mat warpedImage;
  // cv::warpPerspective(resizedImage, warpedImage, perspectiveMatrix_, cv::Size(destW_, destH_), cv::INTER_LINEAR);
  cv::warpPerspective(
      resized_image.download(), warpedImage, perspectiveMatrix_, cv::Size(destW_, destH_), cv::INTER_LINEAR);

  cv::imshow("warpedImage", warpedImage);
  cv::waitKey(0);

  // // Crop the warped image to the final desired dimensions.
  // cv::Rect cropRect(0, 0, destWidth_, destHeight_);
  // if (cropRect.x + cropRect.width > warpedImage.cols || cropRect.y + cropRect.height > warpedImage.rows) {
  //   throw std::runtime_error("Warped image size is smaller than destination size.");
  // }
  // cv::Mat finalImage = warpedImage(cropRect).clone();

  cudaStreamDestroy(stream);
  // return finalImage;
  return inputImage;
}

/**
 * @brief Gets the final output width.
 */
int Scoreboard::getWidth() const {
  return destWidth_;
}

/**
 * @brief Gets the final output height.
 */
int Scoreboard::getHeight() const {
  return destHeight_;
}
} // namespace scoreboard
} // namespace hm
