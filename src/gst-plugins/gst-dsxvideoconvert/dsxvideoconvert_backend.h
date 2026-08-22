/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#ifndef DSX_VIDEO_CONVERT_BACKEND_H_
#define DSX_VIDEO_CONVERT_BACKEND_H_

#include <gst/gst.h>
#include <gst/video/video.h>

#include <cuda_runtime_api.h>

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

struct DsxCropRect {
  guint left = 0;
  guint top = 0;
  guint width = 0;
  guint height = 0;

  bool enabled() const {
    return width != 0 && height != 0;
  }
};

struct DsxBackendOptions {
  guint gpu_id = 0;
  guint compute_hw = 0;
  guint interpolation_method = 6;
  guint flip_method = 0;
  guint copy_hw = 1;
  bool allow_odd_crop = true;
  bool contiguous_buffers = false;
};

class DsxVideoConvertBackend {
 public:
  DsxVideoConvertBackend();
  ~DsxVideoConvertBackend();

  DsxVideoConvertBackend(const DsxVideoConvertBackend&) = delete;
  DsxVideoConvertBackend& operator=(const DsxVideoConvertBackend&) = delete;

  bool start(guint gpu_id, gchar** error_message);
  void stop();

  bool configure(
      const GstVideoInfo& input_info,
      const GstVideoInfo& output_info,
      bool input_nvmm,
      bool output_nvmm,
      guint batch_size,
      const DsxBackendOptions& options,
      gchar** error_message);

  bool transform(
      GstBuffer* input,
      GstBuffer* output,
      const DsxCropRect& source_crop,
      const DsxCropRect& destination_crop,
      gchar** error_message);

 private:
  bool ensure_staging_surfaces(guint batch_size, gchar** error_message);
  bool upload_raw(GstBuffer* buffer, NvBufSurface* surface, guint batch_size, gchar** error_message) const;
  bool download_raw(const NvBufSurface* surface, GstBuffer* buffer, guint batch_size, gchar** error_message) const;

  GstVideoInfo input_info_{};
  GstVideoInfo output_info_{};
  bool input_nvmm_ = false;
  bool output_nvmm_ = false;
  guint batch_size_ = 1;
  DsxBackendOptions options_{};
  NvBufSurfaceColorFormat input_format_ = NVBUF_COLOR_FORMAT_INVALID;
  NvBufSurfaceColorFormat output_format_ = NVBUF_COLOR_FORMAT_INVALID;
  NvBufSurface* input_staging_ = nullptr;
  NvBufSurface* output_staging_ = nullptr;
  cudaStream_t stream_ = nullptr;
  guint gpu_id_ = 0;
  bool started_ = false;
};

NvBufSurfaceColorFormat dsx_video_info_to_nvbuf_format(const GstVideoInfo& info);

#endif // DSX_VIDEO_CONVERT_BACKEND_H_
