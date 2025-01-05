/**
 * SPDX-FileCopyrightText: Copyright (c) 2019-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <assert.h>
#include <string.h>

#include <cassert>
#include <sstream>
#include <vector>

#include <npp.h>

#include "gstnvdsmeta.h"
#include "gstvideoprep.h"

#include "cudaDraw.h"

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "videoprep.h"
#include "videoprep_property_parser.h"

#include "NVWarp360.h"

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

#define USE_CUDA_STREAM

/* Dewarper #defines */
#ifndef M_PI
#define M_PI 3.1415926535897932385
#endif /* M_PI */
#ifndef M_2PI
#define M_2PI 6.2831853071795864769
#endif /* M_2PI */
#define F_PI ((gfloat)M_PI)
#define F_2PI ((gfloat)M_2PI)
#define F_PI_6 ((gfloat)(M_PI / 6.))
#define D_RADIANS_PER_DEGREE (M_PI / 180.)
#define D_DEGREES_PER_RADIAN (180. / M_PI)
#define F_RADIANS_PER_DEGREE ((gfloat)D_RADIANS_PER_DEGREE)
#define F_DEGREES_PER_RADIAN ((gfloat)D_DEGREES_PER_RADIAN)

#define ITER_FACTOR 1.01
#define NUM_WARP_TYPES (NVDS_META_SURFACE_EQUIRECT_VERTCYLINDER + 1)

namespace hm {
namespace videoprep {

// This array maps the enum "NvDsSurfaceType" to enum "nvwarpType_t"
const nvwarpType_t NvDsSurfaceType_To_nvwarpType_t[NUM_WARP_TYPES]{
    NVWARP_NONE, // NVDS_META_SURFACE_NONE=0,
    NVWARP_FISHEYE_PUSHBROOM, // NVDS_META_SURFACE_FISH_PUSHBROOM=1,
    NVWARP_FISHEYE_ROTCYLINDER, // NVDS_META_SURFACE_FISH_VERTCYL=2,
    NVWARP_PERSPECTIVE_PERSPECTIVE, // NVDS_META_SURFACE_PERSPECTIVE_PERSPECTIVE=3,
    NVWARP_FISHEYE_PERSPECTIVE, // NVDS_META_SURFACE_FISH_PERSPECTIVE=4,
    NVWARP_FISHEYE_FISHEYE, // NVDS_META_SURFACE_FISH_FISH=5,
    NVWARP_FISHEYE_CYLINDER, // NVDS_META_SURFACE_FISH_CYL=6,
    NVWARP_FISHEYE_EQUIRECT, // NVDS_META_SURFACE_FISH_EQUIRECT=7,
    NVWARP_FISHEYE_PANINI, // NVDS_META_SURFACE_FISH_PANINI=8,
    NVWARP_PERSPECTIVE_EQUIRECT, // NVDS_META_SURFACE_PERSPECTIVE_EQUIRECT=9,
    NVWARP_PERSPECTIVE_PANINI, // NVDS_META_SURFACE_PERSPECTIVE_PANINI=10,
    NVWARP_EQUIRECT_CYLINDER, // NVDS_META_SURFACE_EQUIRECT_CYLINDER=11,
    NVWARP_EQUIRECT_EQUIRECT, // NVDS_META_SURFACE_EQUIRECT_EQUIRECT=12,
    NVWARP_EQUIRECT_FISHEYE, // NVDS_META_SURFACE_EQUIRECT_FISHEYE=13,
    NVWARP_EQUIRECT_PANINI, // NVDS_META_SURFACE_EQUIRECT_PANINI=14,
    NVWARP_EQUIRECT_PERSPECTIVE, // NVDS_META_SURFACE_EQUIRECT_PERSPECTIVE=15,
    NVWARP_EQUIRECT_PUSHBROOM, // NVDS_META_SURFACE_EQUIRECT_PUSHBROOM=16,
    NVWARP_EQUIRECT_STEREOGRAPHIC, // NVDS_META_SURFACE_EQUIRECT_STEREOGRAPHIC=17,
    NVWARP_EQUIRECT_ROTCYLINDER // NVDS_META_SURFACE_EQUIRECT_VERTCYLINDER=18
};

struct Buffer {
  const unsigned* ptr;
  unsigned width;
  unsigned height;
  unsigned rowBytes;
};

/**
 * @brief Wrapper over the Warp360 library calls.
 */
struct WarpWrapper {
  WarpWrapper() {
    nvwarpCreateInstance(&_warper);
  }

  ~WarpWrapper() {
    if (_warper)
      nvwarpDestroyInstance(_warper);
  }

  nvwarpResult setParams(const nvwarpParams_t* params) {
    return nvwarpSetParams(_warper, params);
  }

  nvwarpResult warp(cudaStream_t stream, cudaTextureObject_t srcTex, void* dstAddr, size_t dstRowBytes) {
    return nvwarpWarpBuffer(_warper, stream, srcTex, dstAddr, dstRowBytes);
  }

  void getSrcPrincipalPoint(float xy[2], bool relToCenter) const {
    nvwarpGetSrcPrincipalPoint(_warper, xy, relToCenter);
  }

  void setSrcPrincipalPoint(const float xy[2], bool relToCenter) {
    nvwarpSetSrcPrincipalPoint(_warper, xy, relToCenter);
  }

  void getDstPrincipalPoint(float xy[2], bool relToCenter) const {
    nvwarpGetDstPrincipalPoint(_warper, xy, relToCenter);
  }

  void setDstPrincipalPoint(const float xy[2], bool relToCenter) {
    nvwarpSetDstPrincipalPoint(_warper, xy, relToCenter);
  }

  void setDstFocalLength(float fl, float fy) {
    nvwarpSetDstFocalLengths(_warper, fl, fy);
  }

  void setSrcFocalLength(float fx, float fy) {
    nvwarpSetSrcFocalLengths(_warper, fx, fy);
  }

  void getSrcFocalLength(float* fx, float* fy) {
    *fx = nvwarpGetSrcFocalLength(_warper, fy);
  }
  void SetRotation(const float* R) {
    nvwarpSetRotation(_warper, R);
  }

  nvwarpHandle _warper; /**< Opaque pointer to the Warp360 library handle. Populated by the constructor of the class */
};

/********************************************************************************
 * Dewarp_Buffer
 * Initialize the warper, set advanced configurations and call the core warp library function
 ********************************************************************************/

// static cudaError Dewarp_Buffer(
//     const GstVideoPrep* videoprep,
//     const Buffer& src,
//     const Buffer& dst,
//     const nvwarpParams_t& warparams,
//     const VideoPrepParams* surfaceParams) {
//   cudaChannelFormatDesc formatDesc = {8, 8, 8, 8, cudaChannelFormatKindUnsigned}; // format descriptor for uchar4
//   cudaResourceDesc srcResDesc = {}, dstResDesc = {};
//   cudaTextureDesc srcTexDesc = {};
//   void *srcBuffer = nullptr, *dstBuffer = nullptr;
//   cudaTextureObject_t srcTex = 0;
//   gint err = 0;
//   cudaError_t cudaErr;
//   size_t cuSrcRowBytes, cuDstRowBytes;
//   // dim3 dimGrid, dimBlock;
//   WarpWrapper warper;

//   // err = err;
//   (void)err;

//   /* Set warper parameters */
//   warper.setParams(&warparams);

//   // If focal lengths are specified for both X & Y seperately, set them here
//   if ((surfaceParams->dewarpFocalLength[0]) && (surfaceParams->dewarpFocalLength[1])) {
//     warper.setSrcFocalLength(surfaceParams->dewarpFocalLength[0], surfaceParams->dewarpFocalLength[1]);
//   }

//   // In case viewing angles are not provided keep same Focal Length and PPoint (preserves detail and symmetry)
//   if (warparams.topAngle == 0 && warparams.bottomAngle == 0) {
//     float xy[2];
//     warper.getSrcPrincipalPoint(xy, true);
//     warper.setDstPrincipalPoint(xy, true);
//     warper.getSrcFocalLength(&xy[0], &xy[1]);
//     warper.setDstFocalLength(xy[0], xy[1]);
//   }
//   if (surfaceParams->rot_matrix_valid)
//     warper.SetRotation(surfaceParams->rot_matrix);

//   if (surfaceParams->dstFocalLength[0])
//     warper.setDstFocalLength(surfaceParams->dstFocalLength[0], surfaceParams->dstFocalLength[1]);
//   if (surfaceParams->dstPrincipalPoint[0] && surfaceParams->dstPrincipalPoint[1])
//     warper.setDstPrincipalPoint(surfaceParams->dstPrincipalPoint, false);

//   /* Allocate src Buffer and texture */
//   cuSrcRowBytes = src.rowBytes;

//   srcBuffer = (void*)src.ptr;
//   srcResDesc.resType = cudaResourceTypePitch2D;
//   srcResDesc.res.pitch2D.devPtr = srcBuffer;
//   srcResDesc.res.pitch2D.desc = formatDesc;
//   srcResDesc.res.pitch2D.width = src.width;
//   srcResDesc.res.pitch2D.height = src.height;
//   srcResDesc.res.pitch2D.pitchInBytes = cuSrcRowBytes;
//   srcTexDesc.addressMode[0] = (surfaceParams->addressMode == 1) ? cudaAddressModeBorder : cudaAddressModeClamp;
//   srcTexDesc.addressMode[1] = srcTexDesc.addressMode[0];

//   if (surfaceParams->addressMode == 1)
//     srcTexDesc.borderColor[0] = srcTexDesc.borderColor[1] = srcTexDesc.borderColor[2] = srcTexDesc.borderColor[3] =
//     0;

//   srcTexDesc.filterMode = cudaFilterModeLinear;
//   srcTexDesc.readMode = cudaReadModeNormalizedFloat;
//   srcTexDesc.normalizedCoords = false;

//   cudaErr = cudaCreateTextureObject(&srcTex, &srcResDesc, &srcTexDesc, nullptr);
//   BAIL_IF_FALSE(cudaSuccess == cudaErr, err, (gint)cudaErr);

//   /* Allocate dst array */
//   cuDstRowBytes = dst.rowBytes;
//   dstBuffer = (void*)dst.ptr;
//   dstResDesc.resType = cudaResourceTypePitch2D;
//   dstResDesc.res.pitch2D.devPtr = dstBuffer;
//   dstResDesc.res.pitch2D.desc = formatDesc;
//   dstResDesc.res.pitch2D.width = warparams.dstWidth;
//   dstResDesc.res.pitch2D.height = warparams.dstHeight;
//   dstResDesc.res.pitch2D.pitchInBytes = cuDstRowBytes;

//   /* Test measurement with 10 iterations */
// #ifdef USE_CUDA_STREAM
//   warper.warp(videoprep->stream, srcTex, dstBuffer, cuDstRowBytes);
//   cudaErr = cudaStreamSynchronize(videoprep->stream);
// #else
//   warper.warp(0, srcTex, dstBuffer, cuDstRowBytes);
//   cudaErr = cudaDeviceSynchronize();
// #endif

// bail:
//   /* Dispose */
//   if (srcTex)
//     cudaDestroyTextureObject(srcTex);

//   return cudaErr;
// }
#if 0
static cudaError MyDewarp(
    NvDsBatchMeta* batch_meta,
    GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    const VideoPrepParams* surfaceParams,
    NvBufSurface* out_surface) {
  nvwarpParams_t warparams;
  cudaError_t cudaErr = cudaSuccess;
  Buffer src, dst;

  src.ptr = (const unsigned int*)in_surface->surfaceList[0].dataPtr;
  src.width = in_surface->surfaceList[0].planeParams.width[0];
  src.height = in_surface->surfaceList[0].planeParams.height[0];
  src.rowBytes = in_surface->surfaceList[0].planeParams.pitch[0];

  dst.ptr = (const guint*)(surfaceParams->surface);
  dst.width = (surfaceParams->dewarpWidth == 0) ? src.width : surfaceParams->dewarpWidth;
  dst.height = (surfaceParams->dewarpHeight == 0) ? src.height : surfaceParams->dewarpHeight;
  dst.rowBytes = surfaceParams->dewarpPitch;

  assert(src.width == dst.width);
  assert(src.height == dst.height);
  assert(src.rowBytes == dst.rowBytes);
  assert(src.ptr != dst.ptr);

#if defined(__aarch64__)
  CUresult status;
  CUeglFrame eglFrame = {};
  CUgraphicsResource pResource = NULL;
  EGLImageKHR eglimage_src = NULL;

  if (in_surface->memType == NVBUF_MEM_SURFACE_ARRAY) {
    if (in_surface->surfaceList[0].mappedAddr.eglImage == NULL) {
      NvBufSurfaceMapEglImage(in_surface, 0);
    }
    eglimage_src = in_surface->surfaceList[0].mappedAddr.eglImage;

    status = cuGraphicsEGLRegisterImage(&pResource, eglimage_src, CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
    if (status != CUDA_SUCCESS) {
      printf("cuGraphicsEGLRegisterImage failed: %d, cuda process stop\n", status);
      exit(-1);
    }

    status = cuGraphicsResourceGetMappedEglFrame(&eglFrame, pResource, 0, 0);
    if (status != CUDA_SUCCESS) {
      printf("cuGraphicsSubResourceGetMappedArray failed\n");
    }

    src.ptr = (const unsigned*)eglFrame.frame.pPitch[0];
  }
#endif

  warparams.type = NvDsSurfaceType_To_nvwarpType_t[surfaceParams->projection_type];
  warparams.srcWidth = src.width;
  warparams.srcHeight = src.height;
  warparams.srcX0 = (surfaceParams->src_x0 == 0) ? (src.width - 1) * .5f : surfaceParams->src_x0;
  warparams.srcY0 = (surfaceParams->src_y0 == 0) ? (src.height - 1) * .5f : surfaceParams->src_y0;
  if (surfaceParams->dewarpFocalLength[0] == 0 && surfaceParams->srcFov > 0) {
    float ang = surfaceParams->srcFov * (.5f * F_RADIANS_PER_DEGREE);
    float rad = ((surfaceParams->srcFov == 180.f) ? src.height : (src.height - 1)) * .5F;

    if (nvwarpComputeParamsSrcFocalLength(&warparams, ang, rad)) { // Computes and sets srcFocalLen
      GST_INFO_OBJECT(
          videoprep,
          "Computing source Focal Length from source Field of View failed. "
          "Setting Focal Length to zero\n");
      warparams.srcFocalLen = 0.f;
    }

  } else {
    warparams.srcFocalLen = surfaceParams->dewarpFocalLength[0];
  }

  /* Set warper parameters */
  // if (surfaceParams->distortion) {
  warparams.dist[0] = surfaceParams->distortion[0];
  warparams.dist[1] = surfaceParams->distortion[1];
  warparams.dist[2] = surfaceParams->distortion[2];
  warparams.dist[3] = surfaceParams->distortion[3];
  warparams.dist[4] = surfaceParams->distortion[4];
  //}

  warparams.dstWidth = dst.width;
  warparams.dstHeight = dst.height;

  if (surfaceParams->rot_axes[0]) {
    if ((strcmp(surfaceParams->rot_axes, "XYZ") == 0) || (strcmp(surfaceParams->rot_axes, "XZY") == 0) ||
        (strcmp(surfaceParams->rot_axes, "YXZ") == 0) || (strcmp(surfaceParams->rot_axes, "YZX") == 0) ||
        (strcmp(surfaceParams->rot_axes, "ZXY") == 0) || (strcmp(surfaceParams->rot_axes, "ZYX") == 0)) {
      strcpy(warparams.rotAxes, surfaceParams->rot_axes);
    } else {
      GST_WARNING_OBJECT(videoprep, "rot-axes setting is incorrect. Using the default setting : %s", warparams.rotAxes);
    }
  }

  // Map Yaw, pitch and roll to appropriate position in "rotAngles" based on "rot_axes"
  for (int i = 0; i < 3; i++) {
    switch (warparams.rotAxes[i]) {
      default:
      case 'X':
        warparams.rotAngles[i] = surfaceParams->pitch * F_RADIANS_PER_DEGREE;
        break;
      case 'Y':
        warparams.rotAngles[i] = surfaceParams->yaw * F_RADIANS_PER_DEGREE;
        break;
      case 'Z':
        warparams.rotAngles[i] = surfaceParams->roll * F_RADIANS_PER_DEGREE;
        break;
    }
  }

  warparams.topAngle = surfaceParams->top_angle * F_RADIANS_PER_DEGREE;
  warparams.bottomAngle = surfaceParams->bottom_angle * F_RADIANS_PER_DEGREE;

  warparams.control[0] = surfaceParams->control;

#if 0 /* simple crop */
  {
      // Crop and rotate the surface
      NvBufSurfTransformParams transform_params = {0};
      NvBufSurfTransformRect src_rect = {0, 0, src.width, src.height};
      NvBufSurfTransformRect dst_rect = {0, 0, dst.width, dst.height};
      transform_params.src_rect = &src_rect;
      transform_params.dst_rect = &dst_rect;
      transform_params.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC;
      transform_params.transform_filter = NvBufSurfTransformInter_Nearest;

      // Perform the transformation
      if (NvBufSurfTransform(in_surface, dst.ptr, &transform_params) != 0) {
          GST_ERROR("Failed to transform surface");
          cudaErr = cudaErrorInvalidValue; // ?
      }

  }
#else
  // cudaErr = Dewarp_Buffer(videoprep, src, dst, warparams, surfaceParams);

  NppStreamContext nppStreamContext;
  // NppStatus npp_status = nppiStreamContextInit(&nppStreamContext);
  // assert(npp_status == 0);
  memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  nppStreamContext.hStream = videoprep->stream; // Assign the CUDA stream
  nppStreamContext.nStreamFlags = 0; // No special flags
  nppStreamContext.nCudaDeviceId = videoprep->gpu_id; // Default queue size

  void cropAndResizeNvBufSurface(
      NvBufSurface * srcSurface,
      NvBufSurface * dstSurface,
      size_t surface_index,
      NppiSize dstSize,
      const NppStreamContext& nppStreamContext);

  assert(in_surface->numFilled == 1);
  for (size_t j = 0; j < in_surface->numFilled; ++j) {
  }

  // cudaErr = cudaMemcpyAsync(
  //     (void*)dst.ptr, (void*)src.ptr, dst.rowBytes * dst.height, cudaMemcpyDeviceToDevice, videoprep->stream);
  assert(cudaErr == 0);

#endif
  if (videoprep->dump_frames)
  // Dump output
  {
    guint size = 0;

    if (!videoprep->output) {
      cuda_ck(cudaMallocHost(&videoprep->output, (dst.rowBytes * dst.height)));
    }

#ifdef USE_CUDA_STREAM
    cudaErr = cudaStreamSynchronize(videoprep->stream);
    GST_INFO_OBJECT(
        videoprep,
        "SPOT %s  DumpFrames i=%d Frame=%d cudaStreamSynchronize cudaErr=%d Stream=%p Completed",
        __func__,
        surfaceParams->id,
        videoprep->frame_num,
        cudaErr,
        videoprep->stream);
#endif

    std::ostringstream elem;
    elem << (void*)videoprep;

    std::string idx_str = std::to_string(surfaceParams->surface_index);
    std::string tmp =
        "_" + elem.str() + "_" + std::to_string(dst.rowBytes >> 2) + "x" + std::to_string(dst.height) + "_" + idx_str;
    std::string fname;

    fname = "Dewarper_Output" + tmp + "_interleaved.rgba";

    size = dst.rowBytes * dst.height;

    cudaMemcpy2D(
        videoprep->output, dst.rowBytes, dst.ptr, dst.rowBytes, dst.rowBytes, dst.height, cudaMemcpyDeviceToHost);

    std::ofstream outfile1;
    outfile1.open(fname, std::ofstream::out | std::ofstream::app);
    outfile1.write(reinterpret_cast<gchar*>(videoprep->output), size);
    outfile1.close();
  }
#if defined(__aarch64__)
  if (in_surface->memType == NVBUF_MEM_SURFACE_ARRAY) {
    status = cuGraphicsUnregisterResource(pResource);
    if (status != CUDA_SUCCESS) {
      printf("cuGraphicsEGLUnRegisterResource failed: %d \n", status);
    }
  }
#endif
  GST_INFO_OBJECT(
      videoprep,
      " %s Frame=%d Dewarp for Views=%d cudaErr=%d "
      "Stream=%p Completed",
      __func__,
      videoprep->frame_num,
      surfaceParams->id,
      cudaErr,
      videoprep->stream);
  return cudaErr;
}
#endif

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
void createAffineMatrix(double angleRadians, int width, int height, double matrix[2][3]) {
  // Image center
  double cx = width / 2.0;
  double cy = height / 2.0;

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

NppStatus rotateNvBufSurfaceWithNPP(
    NvBufSurface* inputSurface,
    size_t input_surface_index,
    const hm::BBox& src_rect,
    NvBufSurface* outputSurface,
    size_t output_surface_index,
    const hm::BBox& dest_rect,
    float angleDegrees,
    const NppStreamContext& nppStreamContext) {
  float angleRadians = angleDegrees * M_PI / 180.0f;

  assert(input_surface_index < inputSurface->numFilled);
  assert(output_surface_index < outputSurface->batchSize);

  // Assume first plane for simplicity
  NvBufSurfaceParams* inParams = &inputSurface->surfaceList[input_surface_index];
  NvBufSurfaceParams* outParams = &outputSurface->surfaceList[output_surface_index];
  assert(inParams->colorFormat == outParams->colorFormat);
  assert(inParams->colorFormat == NVBUF_COLOR_FORMAT_RGBA);

  // Define rotation matrix
  double affineMatrix[2][3];
  createAffineMatrix(angleRadians, static_cast<int>(inParams->width), static_cast<int>(inParams->height), affineMatrix);

  // Set source and destination ROI
  NppiRect srcROI = {0, 0, static_cast<int>(inParams->width), static_cast<int>(inParams->height)};
  NppiRect dstROI = {0, 0, static_cast<int>(outParams->width), static_cast<int>(outParams->height)};

  // assert(srcROI.width == dstROI.width);
  // assert(srcROI.height == dstROI.height);

  // Perform rotation using NPP
  BBox src_rect2(src_rect.center(), dest_rect.size());

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
  assert(srcROI.x + srcROI.width <= (int)inParams->width);
  assert(srcROI.y + srcROI.height <= (int)inParams->height);
  assert(dstROI.width == (int)outParams->width);
  assert(dstROI.height == (int)outParams->height);

  NppStatus status = NppStatus::NPP_SUCCESS;

#if 1
  // Wipe the destination image
  cudaMemsetAsync(outParams->dataPtr, 0, outParams->pitch * outParams->height, nppStreamContext.hStream);
  // Now rotate into the dest image
  status = nppiWarpAffine_8u_C4R_Ctx(
      static_cast<const Npp8u*>(inParams->dataPtr), // Source pointer
      {static_cast<int>(inParams->width), static_cast<int>(inParams->height)}, // Source size
      inParams->pitch, // Source pitch
      srcROI, // Source ROI
      static_cast<Npp8u*>(outParams->dataPtr), // Destination pointer
      outParams->pitch, // Destination pitch
      dstROI, // Destination ROI
      affineMatrix, // Affine transformation matrix
      NPPI_INTER_LINEAR, // Interpolation method
      nppStreamContext);
#endif

  if (status != NPP_SUCCESS) {
    std::cerr << "NPP rotation failed with error: " << status << std::endl;
    // assert(false);
  }
  return status;
}

#if 1
NppStatus cropAndResizeNvBufSurface(
    NvBufSurface* srcSurface,
    const BBox& src_rect,
    NvBufSurface* dstSurface,
    size_t surface_index,
    const BBox& dest_rect,
    const NppStreamContext& nppStreamContext) {
  // Extract the source image from the NvBufSurface
  const NvBufSurfaceParams& srcParams = srcSurface->surfaceList[surface_index];
  Npp8u* srcImage = (Npp8u*)srcParams.dataPtr;

  const NvBufSurfaceParams& dstParams = dstSurface->surfaceList[surface_index];
  const int dstPitch = dstParams.pitch;

  // Define source and destination rectangles for cropping
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

  // Allocate memory for the destination image in the destination surface
  Npp8u* dstImage = (Npp8u*)dstParams.dataPtr;

  // Perform cropping and resizing using nppiResize or nppiWarpAffine (interpolation)
  // For simplicity, let's use nppiResize for resizing and interpolation
  NppStatus status = NppStatus::NPP_SUCCESS;

  // Wipe the destination image
  cudaMemsetAsync(dstImage, 0, dstParams.pitch * dstParams.height, nppStreamContext.hStream);

  status = nppiResize_8u_C4R_Ctx(
      srcImage,
      srcParams.pitch, // Source image and pitch
      NppiSize{.width = (int)srcParams.width, .height = (int)srcParams.height},
      srcRect, // Source rectangle
      dstImage,
      dstPitch, // Destination image and pitch
      NppiSize{.width = (int)dstParams.width, .height = (int)dstParams.height},
      dstRect, // Destination rectangle
      NPPI_INTER_LINEAR, // Interpolation method (e.g., linear)
      nppStreamContext);

  if (status != NPP_SUCCESS) {
    std::cerr << "Error in nppiResize_8u_C4R: " << status << std::endl;
    assert(false);
  }
  return status;
}

#endif
NppStatus cropAndResizeNvBufSurface(
    NvBufSurface* srcSurface,
    const BBox& src_rect,
    VideoPrepParams* videpPrepParams,
    size_t surface_index,
    const BBox& dest_rect,
    const NppStreamContext& nppStreamContext) {
  // Extract the source image from the NvBufSurface
  const NvBufSurfaceParams& srcParams = srcSurface->surfaceList[surface_index];
  Npp8u* srcImage = (Npp8u*)srcParams.dataPtr;

  const int dstPitch = videpPrepParams->dewarpPitch;

  // Define source and destination rectangles for cropping
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

  // Allocate memory for the destination image in the destination surface
  Npp8u* dstImage = (Npp8u*)videpPrepParams->surface;

  // Perform cropping and resizing using nppiResize or nppiWarpAffine (interpolation)
  // For simplicity, let's use nppiResize for resizing and interpolation
  NppStatus status = NppStatus::NPP_SUCCESS;

  // Wipe the destination image
  cudaMemsetAsync(dstImage, 0, videpPrepParams->dewarpPitch * videpPrepParams->dewarpHeight, nppStreamContext.hStream);

  status = nppiResize_8u_C4R_Ctx(
      srcImage,
      srcParams.pitch, // Source image and pitch
      NppiSize{.width = (int)srcParams.width, .height = (int)srcParams.height},
      srcRect, // Source rectangle
      dstImage,
      dstPitch, // Destination image and pitch
      NppiSize{.width = (int)videpPrepParams->dewarpWidth, .height = (int)videpPrepParams->dewarpHeight},
      dstRect, // Destination rectangle
      NPPI_INTER_LINEAR, // Interpolation method (e.g., linear)
      nppStreamContext);

  if (status != NPP_SUCCESS) {
    std::cerr << "Error in nppiResize_8u_C4R: " << status << std::endl;
  }
  return status;
}

cudaError gst_videoprep_do_dewarp(
    NvDsBatchMeta* batch_meta,
    GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface,
    PointDiff* tbox_shift) {
  cudaError cudaErr = cudaSuccess;
#if 0
  VideoPrepParams* dewarpParams = NULL;

  NppStreamContext nppStreamContext;
  memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  nppStreamContext.hStream = videoprep->stream; // Assign the CUDA stream
  nppStreamContext.nStreamFlags = 0; // No special flags
  nppStreamContext.nCudaDeviceId = videoprep->gpu_id; // Default queue size

  std::vector<hm::BBox> tracking_boxes = get_tracking_boxes(batch_meta);
  for (size_t j = 0; j < in_surface->numFilled; ++j) {
    const BBox all_src_rect(0, 0, in_surface->surfaceList[j].width, in_surface->surfaceList[j].height);
    size_t surface_index = 0;
    dewarpParams = &videoprep->priv->vecDewarpSurface.at(surface_index);
    BBox dest_rect(0, 0, dewarpParams->dewarpWidth, dewarpParams->dewarpHeight);

    // assert((int)videoprep->pre_rotate_size.width == (int)dest_rect.width());
    // assert((int)videoprep->pre_rotate_size.height == (int)dest_rect.height());
    const BBox& tbox = tracking_boxes.at(j);

    hm::WHDims src_size{.width = (FloatValue)videoprep->input_width, .height = (FloatValue)videoprep->input_height};
    hm::WHDims output_size{.width = tbox.width(), .height = tbox.height()};
    assert(videoprep->output_height == dewarpParams->dewarpHeight);
    WHDims new_tsize = get_box_size_necessary_for_rotations(src_size, output_size);
    BBox new_tbox(tbox.center(), new_tsize);
    new_tbox = shift_box_to_edge(new_tbox, all_src_rect).bbox;
    //new_tbox = clamp_box(new_tbox, all_src_rect);
    double new_tbox_ar = new_tbox.width() / new_tbox.height();
    double tracking_ar = tbox.width() / tbox.height();
    double dest_ar = double(dewarpParams->dewarpWidth) / dewarpParams->dewarpHeight;
    double destw = double(dewarpParams->dewarpHeight) * new_tbox_ar;
    // assert(isClose(new_tbox_ar, dest_ar));
    // assert(destw <= dewarpParams->dewarpWidth);

    *tbox_shift = tbox.center() - new_tbox.center();

    // double ar = double(dewarpParams->dewarpWidth) / dewarpParams->dewarpHeight;

    // dest_rect = BBox(0, 0, destw, dewarpParams->dewarpHeight);

    assert(new_tbox.width() <= all_src_rect.width());
    assert(new_tbox.height() <= all_src_rect.height());

    NppStatus np_status = cropAndResizeNvBufSurface(
        /*srcSurface=*/in_surface,
        /*src_rect=*/ tbox,
        /*src_rect=*/ // new_tbox,
        /*src_rect=*/ // all_src_rect,
        /*videpPrepParams=*/dewarpParams,
        // out_surface,
        /*surface_index=*/j,
        /*dest_rect=*/dest_rect,
        nppStreamContext);

    // assert(dewarpParams->dewarpPitch % 4 == 0);
    // cudaError_t cuerr = cudaDrawLine(
    //     (uchar4*)(dewarpParams->surface),
    //     dewarpParams->dewarpWidth,
    //     dewarpParams->dewarpHeight,
    //     0,
    //     0,
    //     dewarpParams->dewarpWidth,
    //     dewarpParams->dewarpHeight,
    //     {1.0, 1.0, 0.0, 1.0},
    //     25.0,
    //     nppStreamContext.hStream);
    // (void)cuerr;

    if (np_status != 0 && cudaErr == 0) {
      std::cerr << "Setting cudaErr to arbitrary 'cudaErrorInvalidValue'" << std::endl;
      cudaErr = cudaErrorInvalidValue;
    }
    cudaStreamSynchronize(nppStreamContext.hStream);
  }

  cudaStreamSynchronize(nppStreamContext.hStream);
#endif
  return cudaErr;
}

uint32_t gst_videoprep_version() {
  return nvwarpVersion();
}
} // namespace videoprep
} // namespace hm
