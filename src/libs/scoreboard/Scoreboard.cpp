#include "hstream/src/libs/scoreboard/Scoreboard.h"
#include "hstream/src/libs/common/Status.h"

#include "cupano/pano/cudaMat.h"
#include "cupano/utils/showImage.h"

#include "jetson-utils/cuda/cudaOverlay.h"

#include <opencv2/cudawarping.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include "absl/synchronization/mutex.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <cuda_runtime.h>

namespace hm {
namespace scoreboard {

/**
 * @brief Computes the Euclidean distance between two points.
 */
template <typename T_pixel>
float Scoreboard<T_pixel>::pointDistance(const cv::Point2f& pt0, const cv::Point2f& pt1) {
  return cv::norm(pt0 - pt1);
}

/**
 * @brief Orders four points in clockwise order starting from the top-left.
 */
template <typename T_pixel>
std::vector<cv::Point2f> Scoreboard<T_pixel>::orderPointsClockwise(const std::vector<cv::Point2f>& pts) {
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
template <typename T_pixel>
Scoreboard<T_pixel>::Scoreboard(
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

  // Compute the perspective transform matrix.
  perspectiveMatrix_ = cv::getPerspectiveTransform(srcPts, dstPts);
}

/**
 * @brief Applies the perspective warp transformation to the input image.
 */
template <typename T_pixel>
cv::Mat Scoreboard<T_pixel>::forward_cv(const cv::Mat& inputImage) {
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

template <typename T_pixel>
absl::Status Scoreboard<T_pixel>::forward_prod(
    const surface::Surface source_surface,
    const surface::Surface dest_surface,
    bool rewarp,
    cudaStream_t stream) {
  absl::MutexLock lk(&mu_);
  if (!warped_image_) {
    rewarp = true;
    warped_image_ = std::make_unique<hm::CudaMat<T_pixel>>(/*B=*/1, destW_, destH_);
  }

  if (rewarp) {
    assert(source_surface.bytes_per_pixel() == sizeof(T_pixel));
    assert(source_surface.pitch() % source_surface.bytes_per_pixel() == 0);
    hm::CudaMat<T_pixel> full_image(
        SurfaceInfo{
            .width = (int)source_surface.width(),
            .height = (int)source_surface.height(),
            .pitch = (int)source_surface.pitch(),
            .data_ptr = source_surface.dataptr(),
        },
        /*B=*/1);

    cv::cuda::GpuMat gpu_mat(
        full_image.height(),
        // full_image.width(),
        source_surface.pitch_width(),
        cudaPixelTypeToCvType(full_image.cuda_pixel_type()),
        full_image.data_raw());

    cv::cuda::GpuMat cv_warped_image(
        warped_image_->height(),
        warped_image_->width(),
        cudaPixelTypeToCvType(warped_image_->cuda_pixel_type()),
        warped_image_->data_raw());

    cv::cuda::warpPerspective(gpu_mat, cv_warped_image, perspectiveMatrix_, cv::Size(destW_, destH_), cv::INTER_LINEAR);

    // cv::Mat showimg;
    // cv_warped_image.download(showimg);
    // cv::imshow("showimg", showimg);
    // cv::waitKey(0);
  }

  assert(dest_surface.bytes_per_pixel() == sizeof(T_pixel));

  XCUDA_RETURN_IF_ERROR(cudaOverlayPitch<T_pixel>(
      warped_image_->data(),
      warped_image_->width(),
      warped_image_->height(),
      warped_image_->pitch(),
      dest_surface.dataptr<T_pixel*>(),
      dest_surface.width(),
      dest_surface.height(),
      dest_surface.pitch(),
      /*x=*/0,
      /*y=*/0,
      stream));
  // SHOW_IMAGE(&hm::cudaMat<uchar4>(dest_surface));
  return absl::OkStatus();
}

template <typename T_pixel>
cv::Mat Scoreboard<T_pixel>::forward_cuda(const cv::Mat& inputImage) {
  cudaError_t cuErr = cudaSetDevice(0);
  (void)cuErr;
  cudaStream_t stream;
  cuErr = cudaStreamCreate(&stream);
  (void)cuErr;

  hm::CudaMat<T_pixel> full_image(inputImage);

  cv::cuda::GpuMat gpu_mat(
      full_image.height(),
      full_image.width(),
      cudaPixelTypeToCvType(full_image.cuda_pixel_type()),
      full_image.data_raw());

  absl::MutexLock lk(&mu_);
  if (!warped_image_) {
    warped_image_ = std::make_unique<hm::CudaMat<T_pixel>>(/*B=*/1, destW_, destH_);
  }
  cv::cuda::GpuMat cv_warped_image(
      warped_image_->height(),
      warped_image_->width(),
      cudaPixelTypeToCvType(warped_image_->cuda_pixel_type()),
      warped_image_->data_raw());

  cv::cuda::warpPerspective(gpu_mat, cv_warped_image, perspectiveMatrix_, cv::Size(destW_, destH_), cv::INTER_LINEAR);

  // cv::Mat showimg;
  // cv_warped_image.download(showimg);

  // cv::imshow("showimg", showimg);
  // cv::waitKey(0);

  cuErr = cudaOverlayPitch<T_pixel>(
      warped_image_->data(),
      warped_image_->width(),
      warped_image_->height(),
      warped_image_->pitch(),
      full_image.data(),
      full_image.width(),
      full_image.height(),
      full_image.pitch(),
      /*x=*/0,
      /*y=*/0,
      stream);
  (void)cuErr;

  SHOW_IMAGE(&full_image);

  cudaStreamDestroy(stream);
  return full_image.download();
}

template class Scoreboard<uchar3>;
template class Scoreboard<uchar4>;

} // namespace scoreboard
} // namespace hm
