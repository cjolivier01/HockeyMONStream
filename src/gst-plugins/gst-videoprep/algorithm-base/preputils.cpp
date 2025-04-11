#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/preputils.h"

#include "cudaCrop.h"
#include "cudaWarp.h"
#include "cupano/cuda/cudaStatus.h"

#include <assert.h>
#include <string.h>

#include <cassert>
#include <iostream>

#include <npp.h>

namespace hm {

#include <cuda_runtime.h> // for cudaError_t and related enums

// Example mapping function
cudaError_t mapNppStatusToCudaError(const NppStatus& status) {
  switch (status) {
    // Success codes
    case NPP_SUCCESS:
      return cudaSuccess;

    // Not supported / unimplemented
    case NPP_NOT_SUPPORTED_MODE_ERROR:
    case NPP_ZC_MODE_NOT_SUPPORTED_ERROR:
    case NPP_ROUND_MODE_NOT_SUPPORTED_ERROR:
    case NPP_NOT_SUFFICIENT_COMPUTE_CAPABILITY:
      return cudaErrorNotSupported;

    // Pointer errors
    case NPP_INVALID_HOST_POINTER_ERROR:
      return cudaErrorInvalidHostPointer;
    case NPP_INVALID_DEVICE_POINTER_ERROR:
      return cudaErrorInvalidDevicePointer;

    // Texture binding error
    case NPP_TEXTURE_BIND_ERROR:
      return cudaErrorInvalidTextureBinding;

    // Kernel execution error
    case NPP_CUDA_KERNEL_EXECUTION_ERROR:
      return cudaErrorLaunchFailure;

    // Memory and allocation errors
    case NPP_MEMORY_ALLOCATION_ERR:
    case NPP_NO_MEMORY_ERROR:
      return cudaErrorMemoryAllocation;

    // Alignment errors
    case NPP_ALIGNMENT_ERROR:
      return cudaErrorMisalignedAddress;

    // Many of the remaining errors indicate an invalid value or configuration:
    case NPP_LUT_PALETTE_BITSIZE_ERROR:
    case NPP_WRONG_INTERSECTION_ROI_ERROR:
    case NPP_QUALITY_INDEX_ERROR:
    case NPP_OVERFLOW_ERROR:
    case NPP_NOT_EVEN_STEP_ERROR:
    case NPP_HISTOGRAM_NUMBER_OF_LEVELS_ERROR:
    case NPP_LUT_NUMBER_OF_LEVELS_ERROR:
    case NPP_CHANNEL_ORDER_ERROR:
    case NPP_ZERO_MASK_VALUE_ERROR:
    case NPP_QUADRANGLE_ERROR:
    case NPP_RECTANGLE_ERROR:
    case NPP_COEFFICIENT_ERROR:
    case NPP_NUMBER_OF_CHANNELS_ERROR:
    case NPP_COI_ERROR:
    case NPP_DIVISOR_ERROR:
    case NPP_CHANNEL_ERROR:
    case NPP_STRIDE_ERROR:
    case NPP_ANCHOR_ERROR:
    case NPP_MASK_SIZE_ERROR:
    case NPP_RESIZE_FACTOR_ERROR:
    case NPP_INTERPOLATION_ERROR:
    case NPP_MIRROR_FLIP_ERROR:
    case NPP_MOMENT_00_ZERO_ERROR:
    case NPP_THRESHOLD_NEGATIVE_LEVEL_ERROR:
    case NPP_THRESHOLD_ERROR:
    case NPP_FFT_FLAG_ERROR:
    case NPP_FFT_ORDER_ERROR:
    case NPP_STEP_ERROR:
    case NPP_SCALE_RANGE_ERROR:
    case NPP_DATA_TYPE_ERROR:
    case NPP_OUT_OFF_RANGE_ERROR:
    case NPP_DIVIDE_BY_ZERO_ERROR:
    case NPP_NULL_POINTER_ERROR:
    case NPP_RANGE_ERROR:
    case NPP_SIZE_ERROR:
    case NPP_BAD_ARGUMENT_ERROR:
      return cudaErrorInvalidValue;

    // Context errors
    case NPP_CONTEXT_MATCH_ERROR:
      return cudaErrorInvalidConfiguration;

    // Corrupted data
    case NPP_CORRUPTED_DATA_ERROR:
      return cudaErrorIllegalAddress;

    // Not implemented
    case NPP_NOT_IMPLEMENTED_ERROR:
      return cudaErrorNotYetImplemented;

    // Generic error fall-throughs
    case NPP_ERROR:
    case NPP_ERROR_RESERVED:
      return cudaErrorUnknown;

    // Warnings: In this example, warnings are treated as non-errors.
    case NPP_NO_OPERATION_WARNING:
    case NPP_DIVIDE_BY_ZERO_WARNING:
    case NPP_AFFINE_QUAD_INCORRECT_WARNING:
    case NPP_WRONG_INTERSECTION_ROI_WARNING:
    case NPP_WRONG_INTERSECTION_QUAD_WARNING:
    case NPP_DOUBLE_SIZE_WARNING:
      return cudaSuccess;
    case NPP_MISALIGNED_DST_ROI_WARNING:
      return cudaErrorMisalignedAddress;

    // If the status isn’t recognized, return a generic unknown error.
    default:
      return cudaErrorUnknown;
  }
}

/**
 * @brief Wrapper over the Warp360 library calls.
 */

std::vector<hm::BBox> get_object_boxes(NvDsBatchMeta* batch_meta, size_t class_id_low, size_t class_id_hi) {
  std::vector<hm::BBox> results;
  const size_t batch_size = g_list_length(batch_meta->frame_meta_list);
  results.reserve(batch_size);
  for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)l_frame->data;
    for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
      if (obj_meta->class_id >= class_id_low && obj_meta->class_id <= class_id_hi) {
        results.emplace_back(hm::BBox(
            obj_meta->rect_params.left,
            obj_meta->rect_params.top,
            obj_meta->rect_params.left + obj_meta->rect_params.width,
            obj_meta->rect_params.top + obj_meta->rect_params.height));
        break;
      }
    }
  }
  // All or nothing
  assert(results.empty() || results.size() == batch_size);
  return results;
}

// Rotate an image around the image's center
template <typename F>
void createAffineMatrix(double angleRadians, int width, int height, const Point& anchorPoint, F matrix[2][3]) {
  // Image center
  // double cx = width / 2.0;
  // double cy = height / 2.0;
  double cx = anchorPoint.x;
  double cy = anchorPoint.y;

  // Rotation components
  double cosTheta = std::cos(angleRadians);
  double sinTheta = std::sin(angleRadians);

  // Compute affine matrix
  matrix[0][0] = cosTheta; // m00
  matrix[0][1] = -sinTheta; // m01
  matrix[0][2] = cx - cosTheta * cx + sinTheta * cy; // m02 (x-translation)
  matrix[1][0] = sinTheta; // m10
  matrix[1][1] = cosTheta; // m11
  matrix[1][2] = cy - sinTheta * cx - cosTheta * cy; // m12 (y-translation)
}

CudaStatus cropSurface(
    const hm::surface::Surface& in_surface,
    const hm::BBox& src_rect,
    hm::surface::Surface out_surface,
    const NppStreamContext& nppStreamContext) {
  // pitch must match width alignment for the destination surface
  // assert(out_surface.pitch() == out_surface.width() * 4);
  const NppiSize src_image_size = get_nppisize(in_surface);
  (void)src_image_size;
  const NppiSize dest_image_size = get_nppisize(out_surface);
  (void)dest_image_size;
  // Sanity check everything
  assert(src_rect.left >= 0 && src_rect.top >= 0);
  assert(src_rect.width() <= in_surface.width());
  assert(src_rect.height() <= in_surface.height());
  // If not this, we need to clear
  assert((int)src_rect.width() <= dest_image_size.width);
  assert((int)src_rect.height() <= dest_image_size.height);

  const int4 roi{
      .x = (int)src_rect.left,
      .y = (int)src_rect.top,
      .z = (int)src_rect.right - 1,
      .w = (int)src_rect.bottom - 1,
  };
  assert((guint)src_rect.width() <= out_surface.width());
  assert((guint)src_rect.height() <= out_surface.height());
  CUDA_RETURN_IF_ERROR(cudaCrop(
      in_surface.dataptr<uchar4*>(),
      out_surface.dataptr<uchar4*>(),
      roi,
      in_surface.width(),
      in_surface.height(),
      in_surface.pitch(),
      out_surface.pitch(),
      nppStreamContext.hStream));
  return CudaStatus::OkStatus();
}

CudaStatus rotateNvBufSurfaceWithNPP(
    const hm::surface::Surface& in_surface,
    const hm::BBox& src_rect,
    hm::surface::Surface out_surface,
    const hm::BBox& dest_rect,
    float angleDegrees,
    const Point& anchor_point,
    const NppStreamContext& nppStreamContext) {
  float angleRadians = angleDegrees * M_PI / 180.0f;

  assert(in_surface->colorFormat == out_surface->colorFormat);
  assert(in_surface->colorFormat == NVBUF_COLOR_FORMAT_RGBA);

  // Set source and destination ROI
  NppiRect srcROI = {0, 0, static_cast<int>(in_surface.width()), static_cast<int>(in_surface.height())};
  NppiRect dstROI = {0, 0, static_cast<int>(out_surface.width()), static_cast<int>(out_surface.height())};

  // assert(srcROI.width == dstROI.width);
  // assert(srcROI.height == dstROI.height);

  // Perform rotation using NPP
  // BBox src_rect2(src_rect.center(), dest_rect.size());
  BBox src_rect2 = src_rect;

  srcROI.x = src_rect2.left;
  srcROI.y = src_rect2.top;
  srcROI.width = src_rect2.width();
  srcROI.height = src_rect2.height();

  dstROI.x = dest_rect.left;
  dstROI.y = dest_rect.top;
  dstROI.width = dest_rect.width();
  dstROI.height = dest_rect.height();

  // srcROI = dstROI;

  assert(srcROI.width == dstROI.width);
  assert(srcROI.height == dstROI.height);
  assert(srcROI.x + srcROI.width <= (int)in_surface.width());
  assert(srcROI.y + srcROI.height <= (int)in_surface.height());
  // assert(dstROI.width == (int)outParams->width);
  // assert(dstROI.height == (int)outParams->height);

  CUDA_RETURN_IF_ERROR(
      cudaMemsetAsync(out_surface.dataptr(), 0, out_surface.pitch() * out_surface.height(), nppStreamContext.hStream));
  float affineMatrix[2][3];
  createAffineMatrix(
      angleRadians,
      static_cast<int>(src_rect2.width()),
      static_cast<int>(src_rect2.height()),
      anchor_point,
      affineMatrix);
  assert(in_surface.width() == out_surface.width());
  assert(in_surface.height() == out_surface.height());
  CUDA_RETURN_IF_ERROR(cudaWarpAffine(
      in_surface.dataptr<uchar4*>(),
      out_surface.dataptr<uchar4*>(),
      (uint32_t)in_surface.width(),
      (uint32_t)in_surface.height(),
      affineMatrix,
      /*transform_inverted=*/false,
      nppStreamContext.hStream));
  return CudaStatus::OkStatus();
}

CudaStatus cropAndResizeNvBufSurface(
    const hm::surface::Surface& in_surface,
    const BBox& src_rect,
    hm::surface::Surface out_surface,
    const BBox& dest_rect,
    const NppStreamContext& nppStreamContext) {
  // Define source and destination rectangles for cropping
  // cudaError_t cuerr = cudaSuccess;
  NppiRect srcRect{
      .x = (int)src_rect.left,
      .y = (int)src_rect.top,
      .width = (int)src_rect.width(),
      .height = (int)src_rect.height()};

  NppiRect dstRect = {
      .x = (int)dest_rect.left,
      .y = (int)dest_rect.top,
      .width = (int)dest_rect.width(),
      .height = (int)dest_rect.height()};

  // Perform cropping and resizing using nppiResize or nppiWarpAffine (interpolation)
  // For simplicity, let's use nppiResize for resizing and interpolation
  CUDA_RETURN_IF_ERROR(mapNppStatusToCudaError(nppiResize_8u_C4R_Ctx(
      in_surface.dataptr<Npp8u*>(),
      in_surface.pitch(), // Source image and pitch
      NppiSize{.width = (int)in_surface.width(), .height = (int)in_surface.height()},
      srcRect, // Source rectangle
      out_surface.dataptr<Npp8u*>(),
      out_surface.pitch(), // Destination image and pitch
      NppiSize{.width = (int)out_surface.width(), .height = (int)out_surface.height()},
      dstRect, // Destination rectangle
      NPPI_INTER_LINEAR, // Interpolation method (e.g., linear)
      nppStreamContext)));

  return CudaStatus::OkStatus();
}

} // namespace hm
