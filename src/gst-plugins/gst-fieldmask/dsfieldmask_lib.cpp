#include "dsfieldmask_lib.h"
#include "fieldmask_payload.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/common/PlotContext.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>

#include <opencv2/core/types.hpp>
#include <stdio.h>
#include <stdlib.h>

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

namespace fs = std::filesystem;

using FieldMaskPayload = hm::fieldmask::FieldMaskPayload;

struct DsFieldMaskCtx {
  DsFieldMaskInitParams initParams;
  size_t total_frame_count{0};
  cv::Mat detection_bit_mask;
  cv::Mat detection_u8_mask;
  cv::Point2f detection_mask_centroid;
  cv::Rect2i field_box;
};

namespace {

constexpr float lower_bbox_center_by_height_ratio = 0.1;
// In order to avoid picking up players on the bench, we raise the bounding box up a little in order to have some buffer
// that they need to be lower on the ice than just at the top edge
constexpr float raise_bbox_bottom_by_height_ratio = 0.15;
constexpr float side_edges_bbox_by_half_width_ratio = 0.2;

bool is_bit_set(const cv::Mat& mask, const cv::Point& point) {
  int byteIndex = (point.y * mask.cols + point.x / 8); // Byte index in the data
  int bitIndex = point.x % 8; // Bit index within the byte
  // int bitIndex = point.x & 7;
  return (mask.data[byteIndex] & (1 << bitIndex)) != 0;
  // int bitIndex = point.x % 8; // Bit index within the byte
  // uchar val = mask.at<uchar>(cv::Point2l(point.x/8, point.y));
  // return (val & (1 << bitIndex)) != 0;
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
cv::Point2f compute_centroid(const cv::Mat& mask, cv::Rect2i& bbox) {
  if (mask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }
  // cv::Rect bbox(0,)
  double sumX = 0.0, sumY = 0.0;
  int count = 0;

  int low_x = std::numeric_limits<int>::max(), low_y = std::numeric_limits<int>::max();
  int high_x = -1, high_y = -1;

  for (int y = 0; y < mask.rows; ++y) {
    const uchar* row = mask.ptr<uchar>(y);
    for (int x = 0; x < mask.cols; ++x) {
      if (row[x] != 0) { // Check for non-zero values
        sumX += x;
        sumY += y;
        low_x = std::min(x, low_x);
        low_y = std::min(y, low_y);
        high_x = std::max(x, high_x);
        high_y = std::max(y, high_y);
        ++count;
      }
    }
  }

  if (count == 0) {
    throw std::runtime_error("The mask has no non-zero pixels.");
  }

  if (low_x != std::numeric_limits<int>::max() && low_y != std::numeric_limits<int>::max() && high_x > 0 &&
      high_y > 0) {
    bbox = cv::Rect2i(low_x, low_y, high_x - low_x, high_y - low_y);
  } else {
    bbox = cv::Rect2i(0, 0, 0, 0);
  }

  // Return the centroid as a floating-point point
  return cv::Point2f(sumX / count, sumY / count);
}

// Load mask from file
absl::StatusOr<cv::Mat> load_mask_from_file(const std::string& filePath) {
  if (!fs::exists(filePath)) {
    return absl::NotFoundError(TO_STRING("Mask file does not exist: " << filePath));
  }
  // Load the image as a single-channel grayscale image
  cv::Mat mask = cv::imread(filePath, cv::IMREAD_GRAYSCALE);
  if (mask.empty()) {
    return absl::InvalidArgumentError(TO_STRING("Failed to load mask from file: " << filePath));
  }
  return mask;
}

void prune_detection_boxes(NvDsFrameMeta* frame_meta, const DsFieldMaskCtx* ctx, bool draw) {
  if (!frame_meta->obj_meta_list || !frame_meta->bInferDone) {
    return;
  }

  std::unique_ptr<hm::utils::PlotContext> plot_context;

  // assert(guint(ctx->detection_u8_mask.cols) <= frame_meta->source_frame_width);
  // assert(guint(ctx->detection_u8_mask.rows) <= frame_meta->source_frame_height);

  assert(guint(ctx->detection_u8_mask.cols) == frame_meta->source_frame_width);
  assert(guint(ctx->detection_u8_mask.rows) == frame_meta->source_frame_height);

  assert(frame_meta->pipeline_height);
  assert(frame_meta->pipeline_width);

  const float scale_height = float(frame_meta->source_frame_height) / frame_meta->pipeline_height;
  const float scale_width = float(frame_meta->source_frame_width) / frame_meta->pipeline_width;

  NvDsMetaList* l_next = nullptr;
  // static float max_y = 0;
  // static float max_x = 0;
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_next) {
    l_next = l_obj->next;

  // void plot_circle(
  //     const Point center,
  //     int radius,
  //     int thickness,
  //     const ColorT& color,
  //     const std::optional<ColorT>& fill_color = std::nullopt);

    if (draw && !plot_context) {
      // plot_context = std::make_unique<hm::utils::PlotContext>(frame_meta);
    }

    NvDsMetaList* remove_me{nullptr};
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvDsComp_BboxInfo& detector_bbox_info = obj_meta->detector_bbox_info;
    const float half_width = detector_bbox_info.org_bbox_coords.width / 2;
    const float bbox_center_x = detector_bbox_info.org_bbox_coords.left + half_width;
    const float half_height = detector_bbox_info.org_bbox_coords.height / 2;

    // Keep track of extremes for debugging
    // max_x = std::max(max_x, detector_bbox_info.org_bbox_coords.left + detector_bbox_info.org_bbox_coords.width);
    // max_y = std::max(max_y, detector_bbox_info.org_bbox_coords.top + detector_bbox_info.org_bbox_coords.height);

    const int lower_center_height_amount =
        float(detector_bbox_info.org_bbox_coords.height) * lower_bbox_center_by_height_ratio;
    // const int raise_bottom_height_amount =
    //     float(detector_bbox_info.org_bbox_coords.height) * raise_bbox_bottom_by_height_ratio;

    // Center of bounding box
    cv::Point2f ptCenter =
        cv::Point2f(bbox_center_x, detector_bbox_info.org_bbox_coords.top + half_height - lower_center_height_amount);

    // Bottom of bounding box (for testing if their feet are on the ice)
    cv::Point2f ptBottom =
        cv::Point2f(bbox_center_x, detector_bbox_info.org_bbox_coords.top + detector_bbox_info.org_bbox_coords.height);
    ptBottom.y -= float(detector_bbox_info.org_bbox_coords.height) * raise_bbox_bottom_by_height_ratio;

    if (ptBottom.x <= ctx->detection_mask_centroid.x) {
      // left side, so move right just a little bit
      ptBottom.x += half_width * side_edges_bbox_by_half_width_ratio;
    } else {
      // right side, so move left just a little bit
      ptBottom.x -= half_width * side_edges_bbox_by_half_width_ratio;
    }

    ptBottom.x *= scale_width;
    ptBottom.y *= scale_height;
    ptCenter.x *= scale_width;
    ptCenter.y *= scale_height;

    // ok, well, let's just do bottoms against centroid y
    if (ptBottom.y <= ctx->detection_mask_centroid.y) {
      // It's in the top half of the ice, so we just look at the (adjusted) bottom in the mask
      ptBottom.x = std::clamp(ptBottom.x, 0.0f, float(frame_meta->source_frame_width - 1));
      ptBottom.y = std::clamp(ptBottom.y, 0.0f, float(frame_meta->source_frame_height - 1));
      // if (ctx->detection_u8_mask.at<uchar>(ptBottom) == 0) {
      //   remove_me = l_obj;
      // }
      if (!is_bit_set(ctx->detection_bit_mask, ptBottom)) {
        remove_me = l_obj;
      }
    } else {
      // It's in the bottom half of the ice, so we check center
      ptCenter.x = std::clamp(ptCenter.x, 0.0f, float(frame_meta->source_frame_width - 1));
      ptCenter.y = std::clamp(ptCenter.y, 0.0f, float(frame_meta->source_frame_height - 1));
      // if (ctx->detection_u8_mask.at<uchar>(ptCenter) == 0) {
      //   remove_me = l_obj;
      // }
      if (!is_bit_set(ctx->detection_bit_mask, ptCenter)) {
        remove_me = l_obj;
      }
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
  return ctx;
}

absl::Status DsFieldMaskProcessFrame(
    NvBufSurface* surface,
    size_t frame_index,
    NvDsFrameMeta* frame_meta,
    DsFieldMaskCtx* ctx,
    bool draw) {
  if (ctx->initParams.detection_mask_file.empty()) {
    // We are a No-op
    return absl::OkStatus();
  }

  bool is_obsolete_detection_mask = false; // TODO: only check first frame
  if (guint(ctx->detection_u8_mask.cols) != frame_meta->source_frame_width ||
      guint(ctx->detection_u8_mask.rows) != frame_meta->source_frame_height) {
    // std::cout << "Obsolete detection mask(s)" << std::endl;
    is_obsolete_detection_mask = true;
  }

  if (ctx->total_frame_count == 0 && (ctx->detection_u8_mask.empty() || is_obsolete_detection_mask)) {
    fs::path mask_path = ctx->initParams.detection_mask_file;
    // if (!fs::exists(fs::path(mask_path))) {
    assert(frame_index < surface->numFilled);
#ifdef __aarch64__
    hm::surface::EglSurfaceMapper egl_surface_mapper(surface, frame_index, /*read_only=*/true);
    hm::surface::Surface this_surface = egl_surface_mapper.get_surface();
#else
    hm::surface::Surface this_surface(&surface->surfaceList[frame_index]);
#endif
    if (!hm::stitching::is_field_mask_configured(mask_path.parent_path().string())) {
      HM_RETURN_IF_ERROR(hm::stitching::create_field_mask(mask_path.parent_path().string(), this_surface));
    }
    //}
    HM_ASSIGN_OR_RETURN(ctx->detection_u8_mask, load_mask_from_file(ctx->initParams.detection_mask_file));
    ctx->detection_mask_centroid = compute_centroid(ctx->detection_u8_mask, ctx->field_box);
    ctx->detection_bit_mask = convert_to_bit_mask(ctx->detection_u8_mask);
  }
  prune_detection_boxes(frame_meta, ctx, draw);
  FieldMaskPayload::create_and_add<FieldMaskPayload>(frame_meta, ctx->detection_mask_centroid, ctx->field_box);
  ++ctx->total_frame_count;
  return absl::OkStatus();
}

void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx) {
  if (ctx) {
    delete ctx;
  }
}
