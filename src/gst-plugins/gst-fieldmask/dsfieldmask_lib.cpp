/*
 * SPDX-FileCopyrightText: Copyright (c) 2017-2020 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier:
 * LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "dsfieldmask_lib.h"

#include <opencv2/opencv.hpp>

#include <cassert>
#include <vector>

#include <opencv4/opencv2/core/types.hpp>
#include <stdio.h>
#include <stdlib.h>

struct DsFieldMaskCtx {
  DsFieldMaskInitParams initParams;
  cv::Mat detection_bit_mask;
  cv::Mat detection_u8_mask;
  cv::Point2f detection_mask_centroid;
};

namespace {

// I dont understand why this is backwards (negative)
constexpr float raise_bbox_center_by_height_ratio = -0.1;
constexpr float lower_bbox_bottom_by_height_ratio = -0.1;

bool is_bit_set(const cv::Mat& mask, const cv::Point& point) {
  int byteIndex = (point.y * mask.cols + point.x) / 8; // Byte index in the data
  int bitIndex = point.x % 8; // Bit index within the byte
  return (mask.data[byteIndex] & (1 << bitIndex)) != 0;
}

// Convert 8-bit-per-pixel mask to 1-bit-per-pixel (packed as bytes)
cv::Mat convert_to_bit_mask(const cv::Mat& inputMask) {
  if (inputMask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }

  // Output matrix: 1/8th the number of columns (rounded up), same rows
  int packedCols = (inputMask.cols + 7) / 8;
  cv::Mat bitMask(inputMask.rows, packedCols, CV_8UC1, cv::Scalar(0));

  for (int y = 0; y < inputMask.rows; ++y) {
    const uchar* inputRow = inputMask.ptr<uchar>(y);
    uchar* outputRow = bitMask.ptr<uchar>(y);

    for (int x = 0; x < inputMask.cols; ++x) {
      if (inputRow[x] != 0) {
        outputRow[x / 8] |= (1 << (7 - (x % 8))); // Set the corresponding bit
      }
    }
  }

  return bitMask;
}

// Compute the centroid of a binary mask
cv::Point2f compute_centroid(const cv::Mat& mask) {
  if (mask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }

  double sumX = 0.0, sumY = 0.0;
  int count = 0;

  for (int y = 0; y < mask.rows; ++y) {
    const uchar* row = mask.ptr<uchar>(y);
    for (int x = 0; x < mask.cols; ++x) {
      if (row[x] != 0) { // Check for non-zero values
        sumX += x;
        sumY += y;
        ++count;
      }
    }
  }

  if (count == 0) {
    throw std::runtime_error("The mask has no non-zero pixels.");
  }

  // Return the centroid as a floating-point point
  return cv::Point2f(sumX / count, sumY / count);
}

// Load mask from file
cv::Mat load_mask_from_file(const std::string& filePath) {
  // Load the image as a single-channel grayscale image
  cv::Mat mask = cv::imread(filePath, cv::IMREAD_GRAYSCALE);
  if (mask.empty()) {
    throw std::runtime_error("Failed to load mask from file: " + filePath);
  }

  return mask;
}

void prune_detection_boxes(NvDsFrameMeta* frame_meta, const DsFieldMaskCtx* ctx) {
  if (!frame_meta->obj_meta_list || !frame_meta->bInferDone) {
    return;
  }
  const float mask_height = ctx->detection_u8_mask.rows;
  const float mask_width = ctx->detection_u8_mask.cols;

  assert(guint(mask_width) == frame_meta->source_frame_width);
  assert(guint(mask_height) == frame_meta->source_frame_height);

  const float scale_height = float(mask_height) / frame_meta->pipeline_height;
  const float scale_width = float(mask_width) / frame_meta->pipeline_width;

  NvDsMetaList* l_next = nullptr;
  static float max_y = 0;
  static float max_x = 0;
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_next) {
    l_next = l_obj->next;
    NvDsMetaList* remove_me{nullptr};
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvDsComp_BboxInfo& detector_bbox_info = obj_meta->detector_bbox_info;
    const float half_width = detector_bbox_info.org_bbox_coords.width / 2;
    const float center_x = detector_bbox_info.org_bbox_coords.left + half_width;
    const float half_height = detector_bbox_info.org_bbox_coords.height / 2;

    max_x = std::max(max_x, detector_bbox_info.org_bbox_coords.left + detector_bbox_info.org_bbox_coords.width);
    max_y = std::max(max_y, detector_bbox_info.org_bbox_coords.top + detector_bbox_info.org_bbox_coords.height);

    const int raise_center_height_amount =
        float(detector_bbox_info.org_bbox_coords.height) * raise_bbox_center_by_height_ratio;
    const int lower_bottom_height_amount =
        float(detector_bbox_info.org_bbox_coords.height) * lower_bbox_bottom_by_height_ratio;

    cv::Point2f ptCenter = cv::Point2f(
        center_x,
        detector_bbox_info.org_bbox_coords.top + half_height - raise_center_height_amount);
    cv::Point2f ptBottom = cv::Point2f(
        center_x,
        detector_bbox_info.org_bbox_coords.top + detector_bbox_info.org_bbox_coords.height +
            lower_bottom_height_amount);


    ptBottom.x *= scale_width;
    ptBottom.y *= scale_height;
    ptCenter.x *= scale_width;
    ptCenter.y *= scale_height;

    // ok, well, let's just do bottoms against centroid y
    if (ptBottom.y <= ctx->detection_mask_centroid.y) {
      // It's in the top half of the ice, so we just look at the (adjusted) bottom in the mask
      ptBottom.x = std::clamp(ptBottom.x, 0.0f, mask_width - 1);
      ptBottom.y = std::clamp(ptBottom.y, 0.0f, mask_height - 1);
      if (ctx->detection_u8_mask.at<uchar>(ptBottom) == 0) {
        remove_me = l_obj;
      }
      // if (!is_bit_set(ctx->detection_bit_mask, ptBottom)) {
      //   remove_me = l_obj;
      // }
    } else {
      // It's in the bottom half of the ice, so we check center
      ptCenter.x = std::clamp(ptCenter.x, 0.0f, mask_width - 1);
      ptCenter.y = std::clamp(ptCenter.y, 0.0f, mask_height - 1);
      if (ctx->detection_u8_mask.at<uchar>(ptCenter) == 0) {
        remove_me = l_obj;
      }
      // if (!is_bit_set(ctx->detection_bit_mask, ptCenter)) {
      //   remove_me = l_obj;
      // }
    }
    if (remove_me) {
      nvds_remove_obj_meta_from_frame(frame_meta, obj_meta);
    } else {
      usleep(0);
    }
  }
}
} // namespace

DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* initParams) {
  DsFieldMaskCtx* ctx = new DsFieldMaskCtx();
  ctx->initParams = *initParams;
  if (!ctx->initParams.detection_mask_file.empty()) {
    // extra memeory used here, try to settle on but mask
    ctx->detection_u8_mask = load_mask_from_file(ctx->initParams.detection_mask_file);
    ctx->detection_mask_centroid = compute_centroid(ctx->detection_u8_mask);
    ctx->detection_bit_mask = convert_to_bit_mask(ctx->detection_u8_mask);
    // cv::imshow("Mask", ctx->detection_u8_mask);
    //cv::imshow("Mask", ctx->detection_bit_mask);
    cv::waitKey(10);
  }
  return ctx;
}

void DsFieldMaskProcessFrame(NvDsFrameMeta* frame_meta, DsFieldMaskCtx* ctx) {
  if (ctx->initParams.detection_mask_file.empty()) {
    return;
  }
  prune_detection_boxes(frame_meta, ctx);
}

void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx) {
  if (ctx) {
    delete ctx;
  }
}
