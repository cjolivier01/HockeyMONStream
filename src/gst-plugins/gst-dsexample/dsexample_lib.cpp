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

#include "dsexample_lib.h"
#include "utils.h"

#include <opencv2/opencv.hpp>

#include <cassert>
#include <vector>

#include <opencv4/opencv2/core/types.hpp>
#include <stdio.h>
#include <stdlib.h>

struct DsExampleCtx {
  DsExampleInitParams initParams;
  cv::Mat detection_bit_mask;
  cv::Mat detection_u8_mask;
  cv::Point2f detection_mask_centroid;
};

namespace {

constexpr float raise_bbox_center_by_height_ratio = 0.1;
constexpr float lower_bbox_bottom_by_height_ratio = 0.1;

// Function to check if points are nonzero in the mask
std::vector<bool> check_points_in_mask(const cv::Mat& mask, const std::vector<cv::Point>& points) {
  // Ensure the mask is a bit mask (1 bit per pixel)
  if (mask.type() != CV_8UC1) {
    throw std::invalid_argument("Mask must be a single-channel (binary) image.");
  }

  std::vector<bool> results;
  results.reserve(points.size());

  for (const auto& point : points) {
    // Check bounds
    if (point.x >= 0 && point.x < mask.cols && point.y >= 0 && point.y < mask.rows) {
      // Check if the mask value at the point is nonzero
      results.push_back(mask.at<uchar>(point) != 0);
    } else {
      // Out-of-bounds points are considered false
      results.push_back(false);
    }
  }

  return results;
}

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

void prune_detection_boxes(NvDsFrameMeta* frame_meta, const DsExampleCtx* ctx) {
  if (!frame_meta->obj_meta_list) {
    return;
  }
  // std::size_t nr_items = g_list_length(frame_meta->obj_meta_list);

  // std::vector<cv::Point2f> all_centers, all_bottoms;
  // all_centers.reserve(nr_items);
  // all_bottoms.reserve(nr_items);
  const float mask_height = ctx->detection_u8_mask.rows;
  const float mask_width = ctx->detection_u8_mask.cols;
  NvDsMetaList* l_next = nullptr;
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_next) {
    l_next = l_obj->next;
    NvDsMetaList* remove_me{nullptr};
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvDsComp_BboxInfo& detector_bbox_info = obj_meta->detector_bbox_info;
    float half_width = detector_bbox_info.org_bbox_coords.width / 2;
    float center_x = detector_bbox_info.org_bbox_coords.left + half_width;
    float half_height = detector_bbox_info.org_bbox_coords.height / 2;
    int raise_center_height_amount =
        float(detector_bbox_info.org_bbox_coords.height) * raise_bbox_center_by_height_ratio;
    int lower_bottom_height_amount =
        float(detector_bbox_info.org_bbox_coords.height) * lower_bbox_bottom_by_height_ratio;
    cv::Point2f ptCenter = cv::Point2f(
        center_x,
        detector_bbox_info.org_bbox_coords.top + half_height - raise_center_height_amount);
    cv::Point2f ptBottom = cv::Point2f(
        center_x,
        detector_bbox_info.org_bbox_coords.top + detector_bbox_info.org_bbox_coords.height +
            lower_bottom_height_amount);
    // all_centers.emplace_back(ptCenter);
    // all_bottoms.emplace_back(ptBottom);

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

  //     std::vector<NvDsMetaList*> to_remove;
  //     to_remove.reserve(nr_items);
  //     std::size_t counter = 0;
  //     NvDsObjectMeta* obj_meta;
  //     for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_next, ++counter) {
  //       l_next = l_obj->next;
  //       obj_meta = (NvDsObjectMeta*)(l_obj->data);
  //       // const NvDsComp_BboxInfo& detector_bbox_info = obj_meta->detector_bbox_info;
  // #ifndef NDEBUG
  //       // Make sure that tracking wasn't done yet
  //       const NvDsComp_BboxInfo& tracker_bbox_info = obj_meta->tracker_bbox_info;
  //       assert(tracker_bbox_info.org_bbox_coords.height == 0 && tracker_bbox_info.org_bbox_coords.width == 0);
  // #endif
  //       if ((counter & 1) != 0) {
  //         nvds_remove_obj_meta_from_frame(frame_meta, obj_meta);
  //       }
  //     }
  //     std::size_t new_nr_items = g_list_length(frame_meta->obj_meta_list);
  //     if (new_nr_items != nr_items) {
  //       usleep(0);
  //     }
  //  }
  //}
}
} // namespace

DsExampleCtx* DsExampleCtxInit(DsExampleInitParams* initParams) {
  DsExampleCtx* ctx = new DsExampleCtx();
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

void DsExampleProcessFrame(NvDsFrameMeta* frame_meta, DsExampleCtx* ctx) {
  if (ctx->initParams.detection_mask_file.empty()) {
    return;
  }
  prune_detection_boxes(frame_meta, ctx);
}

// In case of an actual processing library, processing on data wil be completed
// in this function and output will be returned
DsExampleOutput* DsExampleProcess(DsExampleCtx* ctx, unsigned char* data) {
  DsExampleOutput* out = (DsExampleOutput*)calloc(1, sizeof(DsExampleOutput));

  if (data != NULL) {
    // Process your data here
  }
  // Fill output structure using processed output
  // Here, we fake some detected objects and labels
  if (ctx->initParams.fullFrame) {
    out->numObjects = 2;
    out->object[0] = (DsExampleObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj0"};

    out->object[1] = (DsExampleObject){
        (float)(ctx->initParams.processingWidth) / 2,
        (float)(ctx->initParams.processingHeight) / 2,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj1"};
  } else {
    out->numObjects = 1;
    out->object[0] = (DsExampleObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        ""};
    // Set the object label
    snprintf(out->object[0].label, 64, "Obj_label");
  }

  return out;
}

void DsExampleCtxDeinit(DsExampleCtx* ctx) {
  if (ctx) {
    delete ctx;
  }
}
