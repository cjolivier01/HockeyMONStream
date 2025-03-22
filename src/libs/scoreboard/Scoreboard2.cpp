#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Scoreboard2.h"
#include "ScoreboardKernels.h"
#include "cupano/pano/cudaMat.h"

#include <opencv2/opencv.hpp>

#include "cupano/pano/cudaMat.h"
#include "cupano/pano/showImage.h"

namespace sc2 {

// Compute bounding box of a set of points.
void Scoreboard::computeBBox(const std::vector<Point2f>& pts, int bbox[4]) const {
  float minx = pts[0].x, miny = pts[0].y, maxx = pts[0].x, maxy = pts[0].y;
  for (size_t i = 1; i < pts.size(); i++) {
    minx = std::min(minx, pts[i].x);
    miny = std::min(miny, pts[i].y);
    maxx = std::max(maxx, pts[i].x);
    maxy = std::max(maxy, pts[i].y);
  }
  bbox[0] = static_cast<int>(std::floor(minx));
  bbox[1] = static_cast<int>(std::floor(miny));
  bbox[2] = static_cast<int>(std::ceil(maxx));
  bbox[3] = static_cast<int>(std::ceil(maxy));
}

// (Optional) Order points in clockwise order.
// For brevity, we assume the points are already ordered.
void Scoreboard::orderPointsClockwise(std::vector<Point2f>& pts) const {
  // (Implementation omitted.)
}

// Compute perspective transform matrix H (3x3) that maps src to dst.
// We solve for 8 unknowns (setting H[2][2] = 1) using Gaussian elimination.
void Scoreboard::computePerspectiveTransform(
    const std::vector<Point2f>& src,
    const std::vector<Point2f>& dst,
    float H[3][3]) const {
  float A[8][8] = {0};
  float b[8] = {0};
  for (int i = 0; i < 4; i++) {
    float x = src[i].x, y = src[i].y;
    float u = dst[i].x, v = dst[i].y;
    A[2 * i][0] = x;
    A[2 * i][1] = y;
    A[2 * i][2] = 1;
    A[2 * i][3] = 0;
    A[2 * i][4] = 0;
    A[2 * i][5] = 0;
    A[2 * i][6] = -u * x;
    A[2 * i][7] = -u * y;
    b[2 * i] = u;

    A[2 * i + 1][0] = 0;
    A[2 * i + 1][1] = 0;
    A[2 * i + 1][2] = 0;
    A[2 * i + 1][3] = x;
    A[2 * i + 1][4] = y;
    A[2 * i + 1][5] = 1;
    A[2 * i + 1][6] = -v * x;
    A[2 * i + 1][7] = -v * y;
    b[2 * i + 1] = v;
  }
  float M[8][9];
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++)
      M[i][j] = A[i][j];
    M[i][8] = b[i];
  }
  for (int i = 0; i < 8; i++) {
    int pivot = i;
    for (int j = i + 1; j < 8; j++) {
      if (fabs(M[j][i]) > fabs(M[pivot][i]))
        pivot = j;
    }
    for (int j = 0; j < 9; j++) {
      std::swap(M[i][j], M[pivot][j]);
    }
    float div = M[i][i];
    assert(fabs(div) > 1e-6);
    for (int j = i; j < 9; j++)
      M[i][j] /= div;
    for (int j = i + 1; j < 8; j++) {
      float factor = M[j][i];
      for (int k = i; k < 9; k++)
        M[j][k] -= factor * M[i][k];
    }
  }
  float h[8];
  for (int i = 7; i >= 0; i--) {
    h[i] = M[i][8];
    for (int j = i + 1; j < 8; j++)
      h[i] -= M[i][j] * h[j];
  }
  H[0][0] = h[0];
  H[0][1] = h[1];
  H[0][2] = h[2];
  H[1][0] = h[3];
  H[1][1] = h[4];
  H[1][2] = h[5];
  H[2][0] = h[6];
  H[2][1] = h[7];
  H[2][2] = 1.0f;
}

// Scoreboard constructor.
Scoreboard::Scoreboard(const std::vector<Point2f>& srcPts, int destWidth, int destHeight, bool autoAspect) {
  assert(srcPts.size() == 4);
  _srcPts = srcPts;
  computeBBox(_srcPts, _bbox);
  // Adjust source points relative to the bounding box.
  for (auto& pt : _srcPts) {
    pt.x -= _bbox[0];
    pt.y -= _bbox[1];
  }
  _roiWidth = _bbox[2] - _bbox[0];
  _roiHeight = _bbox[3] - _bbox[1];

  if (autoAspect) {
    float w_top = std::hypot(_srcPts[1].x - _srcPts[0].x, _srcPts[1].y - _srcPts[0].y);
    float w_bot = std::hypot(_srcPts[2].x - _srcPts[3].x, _srcPts[2].y - _srcPts[3].y);
    float h_left = std::hypot(_srcPts[3].x - _srcPts[0].x, _srcPts[3].y - _srcPts[0].y);
    float h_right = std::hypot(_srcPts[2].x - _srcPts[1].x, _srcPts[2].y - _srcPts[1].y);
    float w_avg = (w_top + w_bot) / 2.0f;
    float h_avg = (h_left + h_right) / 2.0f;
    float aspect = w_avg / h_avg;
    destWidth = static_cast<int>(destHeight * aspect);
  }
  _destWidth = destWidth;
  _destHeight = destHeight;

  // Scale the ROI up so that its dimensions are at least as large as the destination.
  _scaledDestW = std::max(_destWidth, _roiWidth);
  _scaledDestH = std::max(_destHeight, _roiHeight);
  float scaleX = (float)_scaledDestW / _roiWidth;
  float scaleY = (float)_scaledDestH / _roiHeight;
  for (auto& pt : _srcPts) {
    pt.x *= scaleX;
    pt.y *= scaleY;
  }
  // Destination points in the resized ROI coordinates.
  std::vector<Point2f> dstPts = {
      {0.f, 0.f},
      {static_cast<float>(_destWidth - 1), 0.f},
      {static_cast<float>(_destWidth - 1), static_cast<float>(_destHeight - 1)},
      {0.f, static_cast<float>(_destHeight - 1)}};
  computePerspectiveTransform(_srcPts, dstPts, _H);
#if 0
  auto perspectiveMatrix_ = cv::getPerspectiveTransform(_srcPts, dstPts);
  assert(perspectiveMatrix_.cols == 3);
  assert(perspectiveMatrix_.rows == 3);
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      _H[i][j] = perspectiveMatrix_.at<float>(i, j);
    }
  }
#endif
}

// forward() performs the following on the GPU:
// 1. Crop the input image to the ROI (_bbox).
// 2. Resize the cropped ROI to (_scaledDestW x _scaledDestH).
// 3. Apply the warp perspective (using _H) to produce an image of size (_scaledDestW x _scaledDestH).
// 4. Crop the warped image to the final scoreboard size (_destWidth x _destHeight).
cv::Mat Scoreboard::forward(const cv::Mat& input_image) {

  hm::CudaMat<uchar3> src_image(input_image);

  int inW = src_image.width(), inH = src_image.height();
  // Step 1: Crop the ROI.
  int cropW = _roiWidth, cropH = _roiHeight;
  //uchar3* d_crop = nullptr;
  //cudaMalloc(&d_crop, cropW * cropH * sizeof(uchar3));
  hm::CudaMat<uchar3> cropped(1, cropW, cropH);
  launchCropKernel(src_image.data(), inW, inH, cropped.data(), cropped.width(), cropped.height(), _bbox[0], _bbox[1]);

  //SHOW_IMAGE(&cropped);

  // Step 2: Resize the cropped ROI.
  //uchar3* d_resized = nullptr;
  hm::CudaMat<uchar3> resized(1, _scaledDestW, _scaledDestH);
  //cudaMalloc(&d_resized, _scaledDestW * _scaledDestH * sizeof(uchar3));
  launchResizeKernel(cropped.data(), cropped.width(), cropped.height(), resized.data(), resized.width(), resized.height());

  SHOW_IMAGE(&resized);
  //cudaFree(d_crop);
  // Step 3: Warp perspective.
  // uchar3* d_warped = nullptr;
  // cudaMalloc(&d_warped, _scaledDestW * _scaledDestH * sizeof(uchar3));
  hm::CudaMat<uchar3> warped(1, _scaledDestW, _scaledDestH);
  {
    // Pack _H into three float3 rows.
    float3 m0 = {_H[0][0], _H[0][1], _H[0][2]};
    float3 m1 = {_H[1][0], _H[1][1], _H[1][2]};
    float3 m2 = {_H[2][0], _H[2][1], _H[2][2]};
    launchWarpPerspectiveKernel(
        resized.data(), resized.width(), resized.height(), warped.data(), warped.width(), warped.height(), m0, m1, m2);
  }

  SHOW_IMAGE(&warped);

#if 0
  // cudaFree(d_resized);
  // Step 4: Crop to final scoreboard dimensions.
  uchar3* d_final = nullptr;
  cudaMalloc(&d_final, _destWidth * _destHeight * sizeof(uchar3));
  launchCropKernel(d_warped, _scaledDestW, _scaledDestH, d_final, _destWidth, _destHeight, 0, 0);
  cudaFree(d_warped);
#endif
  // Image output;
  // output.width = _destWidth;
  // output.height = _destHeight;
  // output.d_data = d_final;
  return cv::Mat();
}
} // namespace sc2
