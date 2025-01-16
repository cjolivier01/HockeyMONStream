#include "cudaCrop.h"
#include "cudaWarp.h"

#include <assert.h>
#include <string.h>

#include <cassert>
#include <iostream>
#include <vector>

#include <npp.h>

#include "videoprep.h"

// #include "NVWarp360.h"

namespace hm {
namespace videoprep {

/**
 * @brief Wrapper over the Warp360 library calls.
 */

std::vector<hm::BBox> get_tracking_boxes(NvDsBatchMeta* batch_meta) {
  std::vector<hm::BBox> results;
  const size_t batch_size = g_list_length(batch_meta->frame_meta_list);
  results.reserve(batch_size);
  for (NvDsMetaList* l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)l_frame->data;
    for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
      if (obj_meta->class_id == 99) {
        results.emplace_back(hm::BBox(
            obj_meta->rect_params.left,
            obj_meta->rect_params.top,
            obj_meta->rect_params.left + obj_meta->rect_params.width,
            obj_meta->rect_params.top + obj_meta->rect_params.height));
        break;
      }
    }
  }
  assert(results.size() == batch_size);
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

NppStatus cropSurface(
    const hm::surface::Surface& in_surface,
    const hm::BBox& src_rect,
    hm::surface::Surface out_surface,
    bool clear_output_surface,
    const NppStreamContext& nppStreamContext) {
  NppStatus status = NppStatus::NPP_SUCCESS;

  cudaError_t cuerr = cudaSuccess;
  (void)cuerr;
  // pitch must match width alignment for the destination surface
  // assert(out_surface.pitch() == out_surface.width() * 4);
  const NppiSize src_image_size = get_nppisize(in_surface);
  (void)src_image_size;
  const NppiSize dest_image_size = get_nppisize(out_surface);
  (void)dest_image_size;
  if (clear_output_surface) {
    assert(false); // shouldnt need if theyre the same size
    cuerr = cudaMemsetAsync(
        out_surface.dataptr(), 128, out_surface.pitch() * out_surface.height(), nppStreamContext.hStream);
  }
  // Sanity check everything
  assert(src_rect.left >= 0 && src_rect.top >= 0);
  assert(src_rect.width() <= in_surface.width());
  assert(src_rect.height() <= in_surface.height());
  // If not this, we need to clear
  assert((int)src_rect.width() <= dest_image_size.width);
  assert((int)src_rect.height() <= dest_image_size.height);
  // Do we have a use-case for not startiong at 0, 0? does the resize functionw ork at all?
  if (cuerr == cudaSuccess) {
#if 1
#if 0
    const NppiRect dstRect{.x = 0, .y = 0, .width = (int)src_rect.width(), .height = (int)src_rect.height()};
    status = nppiResize_8u_C4R_Ctx(
        in_surface.dataptr<Npp8u*>(),
        in_surface.pitch(), // Source image and pitch
        src_image_size,
        // src_rect_size,
        get_nppirect(src_rect), // Source rectangle
        out_surface.dataptr<Npp8u*>(),
        out_surface.pitch(), // Destination image and pitch
        dest_image_size,
        // dest_rect_size,
        dstRect, // Destination rectangle
        NPPI_INTER_LINEAR, // Interpolation method (e.g., linear)
        nppStreamContext);
    // std::cout << (int)src_rect.width() << ", " << dest_image_size.width << std::endl;
#else
    // reset error
    cuerr = cudaGetLastError();
    // if (cuerr != 0) {
    //   std::cerr << "Cuda error during crop" << std::endl;
    //   assert(false);
    // }

    // cuerr = cudaMemsetAsync(
    //     in_surface.dataptr(), 128, in_surface.pitch() * in_surface.height(), nppStreamContext.hStream);

    // if (cuerr != 0) {
    //   std::cerr << "Cuda error during crop" << std::endl;
    //   assert(false);
    // }

    // cuerr = cudaMemsetAsync(
    //     out_surface.dataptr(), 255, out_surface.pitch() * out_surface.height(), nppStreamContext.hStream);

    // if (cuerr != 0) {
    //   std::cerr << "Cuda error during crop" << std::endl;
    //   assert(false);
    // }

    // *((char *)out_surface.dataptr()) = 3;
    // if (cuerr != 0) {
    //   std::cerr << "Cuda error during cudaMemsetAsync()" << std::endl;
    //   assert(false);
    // }
    // cuerr =
    //     cudaMemsetAsync(in_surface.dataptr(), 0, in_surface.pitch() * in_surface.height(), nppStreamContext.hStream);
    // if (cuerr != 0) {
    //   std::cerr << "Cuda error during crop" << std::endl;
    //   assert(false);
    // }
    const int4 roi{
        .x = (int)src_rect.left,
        .y = (int)src_rect.top,
        .z = (int)src_rect.right - 1,
        .w = (int)src_rect.bottom - 1,
    };
    assert((guint)src_rect.width() <= out_surface.width());
    assert((guint)src_rect.height() <= out_surface.height());
    cudaStreamSynchronize(nppStreamContext.hStream);
    cuerr = cudaGetLastError();
    if (cuerr != 0) {
      std::cerr << "Cuda error during crop" << std::endl;
      assert(false);
    }

    cuerr = cudaCrop(
        in_surface.dataptr<uchar4*>(),
        out_surface.dataptr<uchar4*>(),
        roi,
        in_surface.width(),
        in_surface.height(),
        in_surface.pitch(),
        out_surface.pitch(),
        nppStreamContext.hStream);

    assert(cudaGetLastError() == cudaSuccess);
    // cudaDeviceSynchronize();
    cudaStreamSynchronize(nppStreamContext.hStream);
    cuerr = cudaGetLastError();
    assert(cuerr == cudaSuccess);
#endif
#endif
  }
  if (cuerr != 0) {
    std::cerr << "Cuda error during crop" << std::endl;
    assert(false);
  }

  return status;
}

NppStatus rotateNvBufSurfaceWithNPP(
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

  NppStatus status = NppStatus::NPP_SUCCESS;

  // Define rotation matrix
#if 1
#if 1
  float affineMatrix[2][3];
  createAffineMatrix(
      angleRadians,
      static_cast<int>(src_rect2.width()),
      static_cast<int>(src_rect2.height()),
      anchor_point,
      affineMatrix);
  assert(in_surface.width() == out_surface.width());
  assert(in_surface.height() == out_surface.height());
  cudaError_t cuerr = cudaWarpAffine(
      in_surface.dataptr<uchar4*>(),
      out_surface.dataptr<uchar4*>(),
      (uint32_t)in_surface.width(),
      (uint32_t)in_surface.height(),
      affineMatrix,
      /*transform_inverted=*/false,
      nppStreamContext.hStream);
  if (cuerr != 0) {
    std::cerr << "NPP rotation failed with error: " << cuerr << std::endl;
    assert(false);
  }
#else
  // Wipe the destination image
  cudaMemsetAsync(out_surface.dataptr(), 0, out_surface.pitch() * out_surface.height(), nppStreamContext.hStream);
  double affineMatrix[2][3];
  createAffineMatrix(
      angleRadians,
      static_cast<int>(src_rect2.width()),
      static_cast<int>(src_rect2.height()),
      anchor_point,
      affineMatrix);
  status = nppiWarpAffine_8u_C4R_Ctx(
      in_surface.dataptr<Npp8u*>(), // Source pointer
      {static_cast<int>(in_surface.width()), static_cast<int>(in_surface.height())}, // Source size
      in_surface.pitch(), // Source pitch
      srcROI, // Source ROI
      out_surface.dataptr<Npp8u*>(), // Destination pointer
      out_surface.pitch(), // Destination pitch
      dstROI, // Destination ROI
      affineMatrix, // Affine transformation matrix
      NPPI_INTER_LINEAR, // Interpolation method
      nppStreamContext);
#endif
#endif

  if (status != NPP_SUCCESS) {
    std::cerr << "NPP rotation failed with error: " << status << std::endl;
    assert(false);
  }
  return status;
}

NppStatus cropAndResizeNvBufSurface(
    const hm::surface::Surface& in_surface,
    const BBox& src_rect,
    hm::surface::Surface out_surface,
    const BBox& dest_rect,
    const NppStreamContext& nppStreamContext) {
  // Define source and destination rectangles for cropping
  cudaError_t cuerr = cudaSuccess;
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
  NppStatus status = NppStatus::NPP_SUCCESS;

  assert(cudaGetLastError() == cudaSuccess);

  // Wipe the destination image
  // cuerr =
  //     cudaMemsetAsync(out_surface.dataptr(), 0, out_surface.pitch() * out_surface.height(),
  //     nppStreamContext.hStream);

  if (cuerr != 0) {
    std::cerr << "cudaMemsetAsync failed with error: " << cuerr << std::endl;
    assert(false);
  }

  status = nppiResize_8u_C4R_Ctx(
      in_surface.dataptr<Npp8u*>(),
      in_surface.pitch(), // Source image and pitch
      NppiSize{.width = (int)in_surface.width(), .height = (int)in_surface.height()},
      srcRect, // Source rectangle
      out_surface.dataptr<Npp8u*>(),
      out_surface.pitch(), // Destination image and pitch
      NppiSize{.width = (int)out_surface.width(), .height = (int)out_surface.height()},
      dstRect, // Destination rectangle
      NPPI_INTER_LINEAR, // Interpolation method (e.g., linear)
      nppStreamContext);

  if (status != NPP_SUCCESS) {
    std::cerr << "Error in nppiResize_8u_C4R: " << status << std::endl;
    assert(false);
  }
  return status;
}

} // namespace videoprep
} // namespace hm
