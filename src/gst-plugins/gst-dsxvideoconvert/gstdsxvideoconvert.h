/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#ifndef GST_DSX_VIDEO_CONVERT_H_
#define GST_DSX_VIDEO_CONVERT_H_

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "dsxvideoconvert_backend.h"

G_BEGIN_DECLS

#define GST_TYPE_DSX_VIDEO_CONVERT (gst_dsx_video_convert_get_type())
G_DECLARE_FINAL_TYPE(GstDsxVideoConvert, gst_dsx_video_convert, GST, DSX_VIDEO_CONVERT, GstBaseTransform)
GST_PLUGIN_EXPORT GType gst_dsx_video_convert_get_type(void);

G_END_DECLS

struct _GstDsxVideoConvert {
  GstBaseTransform parent;

  GstVideoInfo input_info;
  GstVideoInfo output_info;
  GstCaps* input_caps;
  GstCaps* output_caps;
  gboolean input_nvmm;
  gboolean output_nvmm;
  guint batch_size;

  gboolean silent;
  guint gpu_id;
  guint output_buffers;
  gchar* source_crop_string;
  gchar* destination_crop_string;
  DsxCropRect source_crop;
  DsxCropRect destination_crop;
  gboolean block_linear_output;
  gboolean allow_odd_crop;
  guint nvbuf_memory_type;
  guint compute_hw;
  guint interpolation_method;
  guint copy_hw;
  gboolean contiguous_buffers;
  gboolean disable_passthrough;
  guint flip_method;

  DsxVideoConvertBackend* backend;
  GMutex lock;
};

#endif // GST_DSX_VIDEO_CONVERT_H_
