/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gstdsxvideoconvert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gst-nvquery.h"
#include "gstnvdsbufferpool.h"
#include "gstnvdsmeta.h"
#include "nvds_version.h"
#include "nvdsmeta.h"

#define PACKAGE "dsxvideoconvert"
#define PACKAGE_NAME "DeepStream open video converter"
#define PACKAGE_VERSION "1.0.0"
#define PACKAGE_LICENSE "Apache-2.0"
#define PACKAGE_ORIGIN "https://github.com/cjolivier01/hstream"

#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
#if NVDS_VERSION_MAJOR >= 9
#define DSX_NVMM_FORMATS                                               \
  "{ I420, NV12, P010_10LE, I420_12LE, BGRx, RGBA, GRAY8, GRAY16_LE, " \
  "RGB, BGR, BGR10A2_LE, UYVP, UYVY, YUY2, YVYU, Y42B, BGRA64_LE }"
#define DSX_RAW_FORMATS                                               \
  "{ I420, NV12, P010_10LE, BGRx, RGBA, GRAY8, GRAY16_LE, RGB, BGR, " \
  "BGR10A2_LE, UYVP, UYVY, YUY2, YVYU, Y42B, BGRA64_LE }"
#else
#define DSX_NVMM_FORMATS                                               \
  "{ I420, NV12, P010_10LE, I420_12LE, BGRx, RGBA, GRAY8, GRAY16_LE, " \
  "RGB, BGR, BGR10A2_LE, UYVP, UYVY, YUY2, YVYU, Y42B }"
#define DSX_RAW_FORMATS                                               \
  "{ I420, NV12, P010_10LE, BGRx, RGBA, GRAY8, GRAY16_LE, RGB, BGR, " \
  "BGR10A2_LE, UYVP, UYVY, YUY2, YVYU, Y42B }"
#endif
#else
#define DSX_NVMM_FORMATS                                              \
  "{ I420, NV12, P010_10LE, I420_12LE, BGRx, RGBA, Y444, Y444_10LE, " \
  "Y444_12LE, GRAY8, GRAY16_LE, GBR, RGB, BGR, BGR10A2_LE, "          \
  "RGB10A2_LE, UYVP, UYVY, BGRA64_LE }"
#define DSX_RAW_FORMATS                                                \
  "{ I420, NV12, P010_10LE, BGRx, RGBA, Y444, GRAY8, GRAY16_LE, GBR, " \
  "RGB, BGR, BGR10A2_LE, RGB10A2_LE, UYVP, UYVY, BGRA64_LE }"
#endif
#define DSX_CAPS                                                                    \
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, DSX_NVMM_FORMATS) \
  ";" GST_VIDEO_CAPS_MAKE(DSX_RAW_FORMATS)

GST_DEBUG_CATEGORY_STATIC(gst_dsx_video_convert_debug);
#define GST_CAT_DEFAULT gst_dsx_video_convert_debug

enum {
  PROP_0,
  PROP_SILENT,
  PROP_GPU_ID,
  PROP_OUTPUT_BUFFERS,
  PROP_SOURCE_CROP,
  PROP_DESTINATION_CROP,
  PROP_BLOCK_LINEAR_OUTPUT,
  PROP_ALLOW_ODD_CROP,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_COMPUTE_HW,
  PROP_INTERPOLATION_METHOD,
  PROP_COPY_HW,
  PROP_CONTIGUOUS_BUFFERS,
  PROP_DISABLE_PASSTHROUGH,
  PROP_FLIP_METHOD,
};

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(DSX_CAPS));
static GstStaticPadTemplate source_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(DSX_CAPS));

G_DEFINE_TYPE(GstDsxVideoConvert, gst_dsx_video_convert, GST_TYPE_BASE_TRANSFORM)

namespace {

GType dsx_flip_method_get_type() {
  static gsize type = 0;
  static const GEnumValue values[] = {
      {0, "Identity (no rotation)", "none"},
      {1, "Rotate counter-clockwise 90 degrees", "counterclockwise"},
      {2, "Rotate 180 degrees", "rotate-180"},
      {3, "Rotate clockwise 90 degrees", "clockwise"},
      {4, "Flip horizontally", "horizontal-flip"},
      {5, "Flip across upper right/lower left diagonal", "upper-right-diagonal"},
      {6, "Flip vertically", "vertical-flip"},
      {7, "Flip across upper left/lower right diagonal", "upper-left-diagonal"},
      {0, nullptr, nullptr},
  };
  if (g_once_init_enter(&type)) {
    const GType registered = g_enum_register_static("DsxVideoFlipMethod", values);
    g_once_init_leave(&type, registered);
  }
  return static_cast<GType>(type);
}

GType dsx_interpolation_method_get_type() {
  static gsize type = 0;
  static const GEnumValue values[] = {
      {0, "Nearest", "Nearest"},
      {1, "Bilinear", "Bilinear"},
      {2, "GPU - Cubic, VIC - 5 Tap", "Algo-1"},
      {3, "GPU - Super, VIC - 10 Tap", "Algo-2"},
      {4, "GPU - LanzoS, VIC - Smart", "Algo-3"},
      {5, "GPU - Ignored, VIC - Nicest", "Algo-4"},
      {6, "GPU - Nearest, VIC - Nearest", "Default"},
      {0, nullptr, nullptr},
  };
  if (g_once_init_enter(&type)) {
    const GType registered = g_enum_register_static("DsxInterpolationMethod", values);
    g_once_init_leave(&type, registered);
  }
  return static_cast<GType>(type);
}

GType dsx_compute_hw_get_type() {
  static gsize type = 0;
  static const GEnumValue values[] = {
      {0, "Default, GPU for dGPU, VIC for Jetson", "Default"},
      {1, "GPU", "GPU"},
      {2, "VIC", "VIC"},
      {0, nullptr, nullptr},
  };
  if (g_once_init_enter(&type)) {
    const GType registered = g_enum_register_static("DsxComputeHWType", values);
    g_once_init_leave(&type, registered);
  }
  return static_cast<GType>(type);
}

GType dsx_memory_type_get_type() {
  static gsize type = 0;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  static const GEnumValue values[] = {
      {0, "Default memory allocated, specific to particular platform", "nvbuf-mem-default"},
      {1, "Allocate Pinned/Host cuda memory", "nvbuf-mem-cuda-pinned"},
      {2, "Allocate Device cuda memory", "nvbuf-mem-cuda-device"},
      {3, "Allocate Unified cuda memory", "nvbuf-mem-cuda-unified"},
      {4, "Allocate Surface Array memory, applicable for Jetson", "nvbuf-mem-surface-array"},
      {0, nullptr, nullptr},
  };
#else
  static const GEnumValue values[] = {
      {0, "Default memory allocated, specific to particular platform", "nvbuf-mem-default"},
      {1, "Allocate Pinned/Host cuda memory", "nvbuf-mem-cuda-pinned"},
      {2, "Allocate Device cuda memory", "nvbuf-mem-cuda-device"},
      {3, "Allocate Unified cuda memory", "nvbuf-mem-cuda-unified"},
      {0, nullptr, nullptr},
  };
#endif
  if (g_once_init_enter(&type)) {
    const GType registered = g_enum_register_static("DsxNvBufMemoryType", values);
    g_once_init_leave(&type, registered);
  }
  return static_cast<GType>(type);
}

GType dsx_copy_hw_get_type() {
  static gsize type = 0;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  static const GEnumValue values[] = {
      {1, "GPU", "GPU"},
      {2, "VIC", "VIC"},
      {0, nullptr, nullptr},
  };
#else
  static const GEnumValue values[] = {
      {1, "GPU", "GPU"},
      {0, nullptr, nullptr},
  };
#endif
  if (g_once_init_enter(&type)) {
    const GType registered = g_enum_register_static("DsxCopyHWType", values);
    g_once_init_leave(&type, registered);
  }
  return static_cast<GType>(type);
}

bool caps_are_nvmm(const GstCaps* caps) {
  if (caps == nullptr || gst_caps_is_empty(caps)) {
    return false;
  }
  const GstCapsFeatures* features = gst_caps_get_features(caps, 0);
  return features != nullptr && gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_NVMM);
}

bool parse_crop(const gchar* text, DsxCropRect* crop) {
  if (text == nullptr || crop == nullptr) {
    return false;
  }
  guint left = 0;
  guint top = 0;
  guint width = 0;
  guint height = 0;
  gchar trailing = '\0';
  if (std::sscanf(text, "%u:%u:%u:%u%c", &left, &top, &width, &height, &trailing) != 4) {
    return false;
  }
  crop->left = left;
  crop->top = top;
  crop->width = width;
  crop->height = height;
  return true;
}

bool is_bgra64_or_uyvp(GstVideoFormat format) {
  return format == GST_VIDEO_FORMAT_BGRA64_LE || format == GST_VIDEO_FORMAT_UYVP;
}

bool is_bgra64_pair_supported(GstVideoFormat input, GstVideoFormat output) {
  if (input != GST_VIDEO_FORMAT_BGRA64_LE && output != GST_VIDEO_FORMAT_BGRA64_LE) {
    return true;
  }
  return is_bgra64_or_uyvp(input) && is_bgra64_or_uyvp(output);
}

bool caps_batch_size(const GstStructure* structure, guint* batch_size) {
  const GValue* value = gst_structure_get_value(structure, "batch-size");
  if (value == nullptr) {
    return false;
  }
  if (G_VALUE_HOLDS_UINT(value)) {
    const guint parsed = g_value_get_uint(value);
    if (parsed > 0) {
      *batch_size = parsed;
      return true;
    }
    return false;
  }
  if (G_VALUE_HOLDS_INT(value)) {
    const gint parsed = g_value_get_int(value);
    if (parsed > 0) {
      *batch_size = static_cast<guint>(parsed);
      return true;
    }
  }
  return false;
}

guint query_batch_size(GstDsxVideoConvert* self, GstCaps* input_caps) {
  const GstStructure* structure = gst_caps_get_structure(input_caps, 0);
  guint batch_size = 1;
  if (caps_batch_size(structure, &batch_size)) {
    return batch_size;
  }

  GstQuery* query = gst_nvquery_batch_size_new();
  if (gst_pad_peer_query(GST_BASE_TRANSFORM_SINK_PAD(self), query)) {
    guint queried = 0;
    if (gst_nvquery_batch_size_parse(query, &queried) && queried > 0) {
      batch_size = queried;
    }
  }
  gst_query_unref(query);
  return batch_size;
}

DsxBackendOptions backend_options(const GstDsxVideoConvert* self) {
  DsxBackendOptions options;
  options.gpu_id = self->gpu_id;
  options.compute_hw = self->compute_hw;
  options.interpolation_method = self->interpolation_method;
  options.flip_method = self->flip_method;
  options.copy_hw = self->copy_hw;
  options.allow_odd_crop = self->allow_odd_crop;
  options.contiguous_buffers = self->contiguous_buffers;
  return options;
}

gfloat scale_coordinate(gfloat value, guint crop_offset, gdouble scale, guint destination_offset) {
  const gdouble shifted = static_cast<gdouble>(value) - crop_offset;
  return static_cast<gfloat>(shifted * scale + destination_offset);
}

void scale_rect(
    NvOSD_RectParams* rect,
    guint source_left,
    guint source_top,
    gdouble scale_x,
    gdouble scale_y,
    guint destination_left,
    guint destination_top) {
  rect->left = scale_coordinate(rect->left, source_left, scale_x, destination_left);
  rect->top = scale_coordinate(rect->top, source_top, scale_y, destination_top);
  rect->width = static_cast<gfloat>(rect->width * scale_x);
  rect->height = static_cast<gfloat>(rect->height * scale_y);
}

void scale_text(
    NvOSD_TextParams* text,
    guint source_left,
    guint source_top,
    gdouble scale_x,
    gdouble scale_y,
    guint destination_left,
    guint destination_top,
    bool scale_font) {
  const gfloat scaled_x = (text->x_offset - source_left) * static_cast<gfloat>(scale_x) + destination_left;
  const gfloat scaled_y = (text->y_offset - source_top) * static_cast<gfloat>(scale_y) + destination_top;
  text->x_offset = static_cast<guint>(scaled_x);
  text->y_offset = static_cast<guint>(scaled_y);
  if (scale_font) {
    text->font_params.font_size = static_cast<guint>(text->font_params.font_size * std::min(scale_x, scale_y));
  }
}

struct MetadataTransform {
  guint input_width = 0;
  guint input_height = 0;
  guint output_width = 0;
  guint output_height = 0;
  DsxCropRect source_crop;
  DsxCropRect destination_crop;
  guint flip_method = 0;
};

MetadataTransform metadata_transform(const GstDsxVideoConvert* self) {
  MetadataTransform transform;
  transform.input_width = GST_VIDEO_INFO_WIDTH(&self->input_info);
  transform.input_height = GST_VIDEO_INFO_HEIGHT(&self->input_info);
  transform.output_width = GST_VIDEO_INFO_WIDTH(&self->output_info);
  transform.output_height = GST_VIDEO_INFO_HEIGHT(&self->output_info);
  transform.source_crop = self->source_crop;
  transform.destination_crop = self->destination_crop;
  transform.flip_method = self->flip_method;
  return transform;
}

void orient_object(NvDsObjectMeta* object, guint method, guint source_width, guint source_height) {
  NvOSD_RectParams& rect = object->rect_params;
  const gfloat left = rect.left;
  const gfloat top = rect.top;
  const gfloat width = rect.width;
  const gfloat height = rect.height;
  switch (method) {
    case 1:
      rect.left = top;
      rect.top = source_width - left - width;
      rect.width = height;
      rect.height = width;
      break;
    case 2:
      rect.left = source_width - left - width;
      rect.top = source_height - top - height;
      break;
    case 3:
      rect.left = source_height - top - height;
      rect.top = left;
      rect.width = height;
      rect.height = width;
      break;
    case 4:
      rect.left = source_width - left - width;
      break;
    case 5:
      rect.left = source_height - top - height;
      rect.top = source_width - left - width;
      rect.width = height;
      rect.height = width;
      break;
    case 6:
      rect.top = source_height - top - height;
      break;
    case 7:
      rect.left = top;
      rect.top = left;
      rect.width = height;
      rect.height = width;
      break;
    default:
      return;
  }

  if (method != 0 && method != 6) {
    object->text_params.x_offset = static_cast<guint>(rect.left);
  }
  if (method != 0 && method != 4) {
    object->text_params.y_offset = static_cast<guint>(rect.top) - object->text_params.font_params.font_size;
  }
}

void scale_metadata(const MetadataTransform& transform, GstBuffer* output) {
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(output);
  if (batch_meta == nullptr) {
    return;
  }

  const guint input_width = transform.input_width;
  const guint input_height = transform.input_height;
  const guint output_width = transform.output_width;
  const guint output_height = transform.output_height;
  const guint source_left = transform.source_crop.enabled() ? std::min(transform.source_crop.left, input_width - 1) : 0;
  const guint source_top = transform.source_crop.enabled() ? std::min(transform.source_crop.top, input_height - 1) : 0;
  const guint source_width =
      transform.source_crop.enabled() ? std::min(transform.source_crop.width, input_width - source_left) : input_width;
  const guint source_height = transform.source_crop.enabled()
      ? std::min(transform.source_crop.height, input_height - source_top)
      : input_height;
  const guint destination_left =
      transform.destination_crop.enabled() ? std::min(transform.destination_crop.left, output_width - 1) : 0;
  const guint destination_top =
      transform.destination_crop.enabled() ? std::min(transform.destination_crop.top, output_height - 1) : 0;
  const guint destination_width = transform.destination_crop.enabled()
      ? std::min(transform.destination_crop.width, output_width - destination_left)
      : output_width;
  const guint destination_height = transform.destination_crop.enabled()
      ? std::min(transform.destination_crop.height, output_height - destination_top)
      : output_height;
  if (source_width == 0 || source_height == 0) {
    return;
  }
  const bool swaps_axes = transform.flip_method == 1 || transform.flip_method == 3 || transform.flip_method == 5 ||
      transform.flip_method == 7;
  const guint scale_source_width = swaps_axes ? source_height : source_width;
  const guint scale_source_height = swaps_axes ? source_width : source_height;
  const gdouble scale_x = static_cast<gdouble>(destination_width) / scale_source_width;
  const gdouble scale_y = static_cast<gdouble>(destination_height) / scale_source_height;
  const gdouble shape_scale = std::min(scale_x, scale_y);
  nvds_acquire_meta_lock(batch_meta);
  for (NvDsMetaList* frame_node = batch_meta->frame_meta_list; frame_node != nullptr; frame_node = frame_node->next) {
    auto* frame = static_cast<NvDsFrameMeta*>(frame_node->data);
    for (NvDsMetaList* object_node = frame->obj_meta_list; object_node != nullptr; object_node = object_node->next) {
      auto* object = static_cast<NvDsObjectMeta*>(object_node->data);
      orient_object(object, transform.flip_method, source_width, source_height);
      scale_rect(&object->rect_params, source_left, source_top, scale_x, scale_y, destination_left, destination_top);
      scale_text(
          &object->text_params, source_left, source_top, scale_x, scale_y, destination_left, destination_top, true);
    }

    for (NvDsMetaList* display_node = frame->display_meta_list; display_node != nullptr;
         display_node = display_node->next) {
      auto* display = static_cast<NvDsDisplayMeta*>(display_node->data);
      for (guint index = 0; index < display->num_rects; ++index) {
        scale_rect(
            &display->rect_params[index], source_left, source_top, scale_x, scale_y, destination_left, destination_top);
      }
      for (guint index = 0; index < display->num_labels; ++index) {
        scale_text(
            &display->text_params[index],
            source_left,
            source_top,
            scale_x,
            scale_y,
            destination_left,
            destination_top,
            false);
      }
      for (guint index = 0; index < display->num_lines; ++index) {
        NvOSD_LineParams& line = display->line_params[index];
        line.x1 = static_cast<guint>(scale_coordinate(line.x1, source_left, scale_x, destination_left));
        line.x2 = static_cast<guint>(scale_coordinate(line.x2, source_left, scale_x, destination_left));
        line.y1 = static_cast<guint>(scale_coordinate(line.y1, source_top, scale_y, destination_top));
        line.y2 = static_cast<guint>(scale_coordinate(line.y2, source_top, scale_y, destination_top));
      }
      for (guint index = 0; index < display->num_arrows; ++index) {
        NvOSD_ArrowParams& arrow = display->arrow_params[index];
        arrow.x1 = static_cast<guint>(scale_coordinate(arrow.x1, source_left, scale_x, destination_left));
        arrow.x2 = static_cast<guint>(scale_coordinate(arrow.x2, source_left, scale_x, destination_left));
        arrow.y1 = static_cast<guint>(scale_coordinate(arrow.y1, source_top, scale_y, destination_top));
        arrow.y2 = static_cast<guint>(scale_coordinate(arrow.y2, source_top, scale_y, destination_top));
      }
      for (guint index = 0; index < display->num_circles; ++index) {
        NvOSD_CircleParams& circle = display->circle_params[index];
        circle.xc = static_cast<guint>(scale_coordinate(circle.xc, source_left, scale_x, destination_left));
        circle.yc = static_cast<guint>(scale_coordinate(circle.yc, source_top, scale_y, destination_top));
        circle.radius = static_cast<guint>(std::lround(circle.radius * shape_scale));
      }
    }
  }
  nvds_release_meta_lock(batch_meta);
}

void copy_fixed_field(GstStructure* destination, const GstStructure* source, const gchar* field) {
  const GValue* value = gst_structure_get_value(source, field);
  if (value != nullptr && gst_value_is_fixed(value)) {
    gst_structure_set_value(destination, field, value);
  }
}

const gchar* memory_type_caps_name(gint memory_type) {
  switch (memory_type) {
    case 1:
      return "nvbuf-mem-cuda-pinned";
    case 3:
      return "nvbuf-mem-cuda-unified";
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    case 0:
    case 4:
      return "nvbuf-mem-surface-array";
#else
    case 0:
#endif
    case 2:
    default:
      return "nvbuf-mem-cuda-device";
  }
}

void add_output_caps_fields(GstStructure* structure, const GstDsxVideoConvert* self) {
  gst_structure_set(
      structure,
      "block-linear",
      G_TYPE_BOOLEAN,
      self->block_linear_output,
      "nvbuf-memory-type",
      G_TYPE_STRING,
      memory_type_caps_name(self->nvbuf_memory_type),
      "gpu-id",
      G_TYPE_INT,
      static_cast<gint>(std::min(self->gpu_id, static_cast<guint>(G_MAXINT))),
      nullptr);
}

} // namespace

static void gst_dsx_video_convert_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* parameter) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(object);

  switch (property_id) {
    case PROP_SILENT:
      g_mutex_lock(&self->lock);
      self->silent = g_value_get_boolean(value);
      g_mutex_unlock(&self->lock);
      break;
    case PROP_GPU_ID:
      g_mutex_lock(&self->lock);
      self->gpu_id = g_value_get_uint(value);
      g_mutex_unlock(&self->lock);
      break;
    case PROP_OUTPUT_BUFFERS:
      g_mutex_lock(&self->lock);
      self->output_buffers = g_value_get_uint(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_SOURCE_CROP: {
      DsxCropRect crop;
      const gchar* text = g_value_get_string(value);
      if (!parse_crop(text, &crop)) {
        GST_WARNING_OBJECT(self, "invalid src-crop value '%s'", text != nullptr ? text : "(null)");
        break;
      }
      g_mutex_lock(&self->lock);
      g_free(self->source_crop_string);
      self->source_crop_string = g_strdup(text);
      self->source_crop = crop;
      g_mutex_unlock(&self->lock);
      gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    }
    case PROP_DESTINATION_CROP: {
      DsxCropRect crop;
      const gchar* text = g_value_get_string(value);
      if (!parse_crop(text, &crop)) {
        GST_WARNING_OBJECT(self, "invalid dest-crop value '%s'", text != nullptr ? text : "(null)");
        break;
      }
      g_mutex_lock(&self->lock);
      g_free(self->destination_crop_string);
      self->destination_crop_string = g_strdup(text);
      self->destination_crop = crop;
      g_mutex_unlock(&self->lock);
      gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    }
    case PROP_BLOCK_LINEAR_OUTPUT:
      g_mutex_lock(&self->lock);
      self->block_linear_output = g_value_get_boolean(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_ALLOW_ODD_CROP:
      g_mutex_lock(&self->lock);
      self->allow_odd_crop = g_value_get_boolean(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_mutex_lock(&self->lock);
      self->nvbuf_memory_type = g_value_get_enum(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_COMPUTE_HW:
      g_mutex_lock(&self->lock);
      self->compute_hw = g_value_get_enum(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_INTERPOLATION_METHOD:
      g_mutex_lock(&self->lock);
      self->interpolation_method = g_value_get_enum(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_COPY_HW:
      g_mutex_lock(&self->lock);
      self->copy_hw = g_value_get_enum(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_CONTIGUOUS_BUFFERS:
      g_mutex_lock(&self->lock);
      self->contiguous_buffers = g_value_get_boolean(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    case PROP_DISABLE_PASSTHROUGH: {
      g_mutex_lock(&self->lock);
      self->disable_passthrough = g_value_get_boolean(value);
      const gboolean disable_passthrough = self->disable_passthrough;
      g_mutex_unlock(&self->lock);
      if (disable_passthrough) {
        gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
      }
      break;
    }
    case PROP_FLIP_METHOD:
      g_mutex_lock(&self->lock);
      self->flip_method = g_value_get_enum(value);
      g_mutex_unlock(&self->lock);
      gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
      gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, parameter);
      break;
  }
}

static void gst_dsx_video_convert_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* parameter) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(object);

  g_mutex_lock(&self->lock);
  switch (property_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, self->silent);
      break;
    case PROP_GPU_ID:
      g_value_set_uint(value, self->gpu_id);
      break;
    case PROP_OUTPUT_BUFFERS:
      g_value_set_uint(value, self->output_buffers);
      break;
    case PROP_SOURCE_CROP:
      g_value_set_string(value, self->source_crop_string);
      break;
    case PROP_DESTINATION_CROP:
      g_value_set_string(value, self->destination_crop_string);
      break;
    case PROP_BLOCK_LINEAR_OUTPUT:
      g_value_set_boolean(value, self->block_linear_output);
      break;
    case PROP_ALLOW_ODD_CROP:
      g_value_set_boolean(value, self->allow_odd_crop);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_value_set_enum(value, self->nvbuf_memory_type);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum(value, self->compute_hw);
      break;
    case PROP_INTERPOLATION_METHOD:
      g_value_set_enum(value, self->interpolation_method);
      break;
    case PROP_COPY_HW:
      g_value_set_enum(value, self->copy_hw);
      break;
    case PROP_CONTIGUOUS_BUFFERS:
      g_value_set_boolean(value, self->contiguous_buffers);
      break;
    case PROP_DISABLE_PASSTHROUGH:
      g_value_set_boolean(value, self->disable_passthrough);
      break;
    case PROP_FLIP_METHOD:
      g_value_set_enum(value, self->flip_method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, parameter);
      break;
  }
  g_mutex_unlock(&self->lock);
}

static gboolean gst_dsx_video_convert_start(GstBaseTransform* transform) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  gchar* error_message = nullptr;
  g_mutex_lock(&self->lock);
  const bool started = self->backend->start(self->gpu_id, &error_message);
  g_mutex_unlock(&self->lock);
  if (!started) {
    GST_ELEMENT_ERROR(
        self,
        RESOURCE,
        FAILED,
        ("Failed to initialize GPU converter"),
        ("%s", error_message != nullptr ? error_message : "unknown error"));
    g_free(error_message);
    return FALSE;
  }
  return TRUE;
}

static gboolean gst_dsx_video_convert_stop(GstBaseTransform* transform) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  g_mutex_lock(&self->lock);
  self->backend->stop();
  gst_clear_caps(&self->input_caps);
  gst_clear_caps(&self->output_caps);
  g_mutex_unlock(&self->lock);
  return TRUE;
}

static gboolean gst_dsx_video_convert_set_caps(GstBaseTransform* transform, GstCaps* input_caps, GstCaps* output_caps) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  GstCaps* effective_output_caps = gst_caps_copy(output_caps);
  effective_output_caps = gst_caps_make_writable(effective_output_caps);
  const GstStructure* input_structure = gst_caps_get_structure(input_caps, 0);
  GstStructure* output_structure = gst_caps_get_structure(effective_output_caps, 0);
  copy_fixed_field(output_structure, input_structure, "framerate");
  copy_fixed_field(output_structure, input_structure, "pixel-aspect-ratio");
  copy_fixed_field(output_structure, input_structure, "interlace-mode");
  copy_fixed_field(output_structure, input_structure, "multiview-mode");
  copy_fixed_field(output_structure, input_structure, "colorimetry");
  copy_fixed_field(output_structure, input_structure, "chroma-site");
  copy_fixed_field(output_structure, input_structure, "batch-size");
  copy_fixed_field(output_structure, input_structure, "num-surfaces-per-frame");
  g_mutex_lock(&self->lock);
  add_output_caps_fields(output_structure, self);
  g_mutex_unlock(&self->lock);
  GstVideoInfo input_info;
  GstVideoInfo output_info;
  gst_video_info_init(&input_info);
  gst_video_info_init(&output_info);
  if (!gst_video_info_from_caps(&input_info, input_caps) ||
      !gst_video_info_from_caps(&output_info, effective_output_caps)) {
    GST_ERROR_OBJECT(self, "invalid negotiated video caps");
    gst_caps_unref(effective_output_caps);
    return FALSE;
  }
  if (!is_bgra64_pair_supported(GST_VIDEO_INFO_FORMAT(&input_info), GST_VIDEO_INFO_FORMAT(&output_info))) {
    GST_ERROR_OBJECT(self, "BGRA64_LE is supported only with BGRA64_LE or UYVP");
    gst_caps_unref(effective_output_caps);
    return FALSE;
  }

  const gboolean input_nvmm = caps_are_nvmm(input_caps);
  const gboolean output_nvmm = caps_are_nvmm(effective_output_caps);
  const guint batch_size = input_nvmm ? query_batch_size(self, input_caps) : 1;
  const gboolean same_caps = gst_caps_is_equal(input_caps, output_caps);
  gchar* error_message = nullptr;
  g_mutex_lock(&self->lock);
  self->input_info = input_info;
  self->output_info = output_info;
  self->input_nvmm = input_nvmm;
  self->output_nvmm = output_nvmm;
  self->batch_size = batch_size;
  gst_caps_replace(&self->input_caps, input_caps);
  gst_caps_replace(&self->output_caps, effective_output_caps);
  const gboolean needs_transform = self->disable_passthrough || self->source_crop.enabled() ||
      self->destination_crop.enabled() || self->flip_method != 0 || !same_caps;
  const bool configured = !needs_transform ||
      self->backend->configure(
          self->input_info,
          self->output_info,
          self->input_nvmm,
          self->output_nvmm,
          self->batch_size,
          backend_options(self),
          &error_message);
  g_mutex_unlock(&self->lock);
  const gboolean caps_updated = gst_caps_is_equal(output_caps, effective_output_caps) ||
      gst_base_transform_update_src_caps(transform, effective_output_caps);
  gst_caps_unref(effective_output_caps);
  gst_base_transform_set_passthrough(transform, !needs_transform);
  if (!caps_updated) {
    GST_ERROR_OBJECT(self, "failed to publish enriched output caps");
    g_free(error_message);
    return FALSE;
  }
  if (!configured) {
    GST_ERROR_OBJECT(
        self, "converter configuration failed: %s", error_message != nullptr ? error_message : "unknown error");
    g_free(error_message);
    return FALSE;
  }
  return TRUE;
}

static GstCaps* gst_dsx_video_convert_transform_caps(
    GstBaseTransform* transform,
    GstPadDirection direction,
    GstCaps* caps,
    GstCaps* filter) {
  GstPadTemplate* target_template = direction == GST_PAD_SINK
      ? gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(transform), "src")
      : gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(transform), "sink");
  GstCaps* result = gst_pad_template_get_caps(target_template);
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  if (direction == GST_PAD_SINK) {
    g_mutex_lock(&self->lock);
  }
  if (!gst_caps_is_empty(caps) && gst_caps_get_size(caps) > 0) {
    const GstStructure* source = gst_caps_get_structure(caps, 0);
    result = gst_caps_make_writable(result);
    for (guint index = 0; index < gst_caps_get_size(result); ++index) {
      GstStructure* destination = gst_caps_get_structure(result, index);
      copy_fixed_field(destination, source, "framerate");
      copy_fixed_field(destination, source, "pixel-aspect-ratio");
      copy_fixed_field(destination, source, "interlace-mode");
      copy_fixed_field(destination, source, "multiview-mode");
      copy_fixed_field(destination, source, "colorimetry");
      copy_fixed_field(destination, source, "chroma-site");
      copy_fixed_field(destination, source, "batch-size");
      copy_fixed_field(destination, source, "num-surfaces-per-frame");
      if (direction == GST_PAD_SINK) {
        add_output_caps_fields(destination, self);
      }
    }
  }
  if (direction == GST_PAD_SINK) {
    g_mutex_unlock(&self->lock);
  }
  if (filter != nullptr) {
    GstCaps* intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = intersection;
  }
  GST_LOG_OBJECT(transform, "transformed caps %" GST_PTR_FORMAT, result);
  return result;
}

static gboolean gst_dsx_video_convert_accept_caps(
    GstBaseTransform* transform,
    GstPadDirection direction,
    GstCaps* caps) {
  GstPadTemplate* pad_template = direction == GST_PAD_SINK
      ? gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(transform), "sink")
      : gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(transform), "src");
  GstCaps* template_caps = gst_pad_template_get_caps(pad_template);
  const gboolean accepted = gst_caps_can_intersect(caps, template_caps);
  gst_caps_unref(template_caps);
  return accepted;
}

static GstCaps* gst_dsx_video_convert_fixate_caps(
    GstBaseTransform* transform,
    GstPadDirection direction,
    GstCaps* caps,
    GstCaps* other_caps) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  g_mutex_lock(&self->lock);
  const gint flip_method = self->flip_method;
  g_mutex_unlock(&self->lock);
  other_caps = gst_caps_truncate(other_caps);
  other_caps = gst_caps_make_writable(other_caps);
  if (gst_caps_is_empty(caps) || gst_caps_is_empty(other_caps)) {
    return other_caps;
  }

  const GstStructure* source = gst_caps_get_structure(caps, 0);
  GstStructure* destination = gst_caps_get_structure(other_caps, 0);
  const gchar* format = gst_structure_get_string(source, "format");
  if (format != nullptr) {
    gst_structure_fixate_field_string(destination, "format", format);
  }

  gint width = 0;
  gint height = 0;
  gst_structure_get_int(source, "width", &width);
  gst_structure_get_int(source, "height", &height);
  if (flip_method == 1 || flip_method == 3 || flip_method == 5 || flip_method == 7) {
    std::swap(width, height);
  }
  if (width > 0) {
    gst_structure_fixate_field_nearest_int(destination, "width", width);
  }
  if (height > 0) {
    gst_structure_fixate_field_nearest_int(destination, "height", height);
  }
  copy_fixed_field(destination, source, "framerate");
  copy_fixed_field(destination, source, "pixel-aspect-ratio");
  copy_fixed_field(destination, source, "interlace-mode");
  copy_fixed_field(destination, source, "multiview-mode");
  copy_fixed_field(destination, source, "colorimetry");
  copy_fixed_field(destination, source, "chroma-site");
  copy_fixed_field(destination, source, "batch-size");
  copy_fixed_field(destination, source, "num-surfaces-per-frame");
  if (direction == GST_PAD_SINK) {
    g_mutex_lock(&self->lock);
    add_output_caps_fields(destination, self);
    g_mutex_unlock(&self->lock);
  }

  other_caps = gst_caps_fixate(other_caps);
  GST_LOG_OBJECT(
      transform,
      "fixated %" GST_PTR_FORMAT " against %" GST_PTR_FORMAT " in direction %s",
      other_caps,
      caps,
      direction == GST_PAD_SINK ? "sink" : "src");
  return other_caps;
}

static gboolean gst_dsx_video_convert_get_unit_size(GstBaseTransform*, GstCaps* caps, gsize* size) {
  if (caps_are_nvmm(caps)) {
    *size = sizeof(NvBufSurface);
    return TRUE;
  }
  GstVideoInfo info;
  gst_video_info_init(&info);
  if (!gst_video_info_from_caps(&info, caps)) {
    return FALSE;
  }
  const GstStructure* structure = gst_caps_get_structure(caps, 0);
  guint batch_size = 1;
  caps_batch_size(structure, &batch_size);
  if (info.size > G_MAXSIZE / batch_size) {
    return FALSE;
  }
  *size = info.size * batch_size;
  return TRUE;
}

static gboolean gst_dsx_video_convert_transform_size(
    GstBaseTransform* transform,
    GstPadDirection,
    GstCaps*,
    gsize,
    GstCaps* other_caps,
    gsize* other_size) {
  return gst_dsx_video_convert_get_unit_size(transform, other_caps, other_size);
}

static gboolean gst_dsx_video_convert_transform_meta(GstBaseTransform*, GstBuffer*, GstMeta* meta, GstBuffer*) {
  return meta->info->api == NVDS_META_API_TYPE;
}

static gboolean gst_dsx_video_convert_decide_allocation(GstBaseTransform* transform, GstQuery* query) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  g_mutex_lock(&self->lock);
  const gboolean output_nvmm = self->output_nvmm;
  const guint output_buffers = self->output_buffers;
  const guint memory_type = self->nvbuf_memory_type;
  const guint gpu_id = self->gpu_id;
  const guint batch_size = self->batch_size;
  const gboolean block_linear_output = self->block_linear_output;
  const gboolean contiguous_buffers = self->contiguous_buffers;
  g_mutex_unlock(&self->lock);
  if (!output_nvmm) {
    return GST_BASE_TRANSFORM_CLASS(gst_dsx_video_convert_parent_class)->decide_allocation(transform, query);
  }

  GstCaps* caps = nullptr;
  gboolean need_pool = FALSE;
  gst_query_parse_allocation(query, &caps, &need_pool);
  if (caps == nullptr) {
    GST_ERROR_OBJECT(self, "allocation query has no caps");
    return FALSE;
  }

  GstBufferPool* pool = gst_nvds_buffer_pool_new();
  GstStructure* configuration = gst_buffer_pool_get_config(pool);
  const gsize buffer_size = sizeof(NvBufSurface);
  gst_buffer_pool_config_set_params(configuration, caps, buffer_size, output_buffers, output_buffers);
  gst_structure_set(
      configuration,
      "memtype",
      G_TYPE_UINT,
      memory_type,
      "gpu-id",
      G_TYPE_UINT,
      gpu_id,
      "batch-size",
      G_TYPE_UINT,
      batch_size,
      "bl-output",
      G_TYPE_UINT,
      block_linear_output ? 1U : 0U,
      "contiguous-alloc",
      G_TYPE_BOOLEAN,
      contiguous_buffers,
      "clear-chroma",
      G_TYPE_BOOLEAN,
      TRUE,
      nullptr);
  if (!gst_buffer_pool_set_config(pool, configuration)) {
    GST_ERROR_OBJECT(self, "failed to configure output buffer pool");
    gst_object_unref(pool);
    return FALSE;
  }

  if (gst_query_get_n_allocation_pools(query) > 0) {
    gst_query_set_nth_allocation_pool(query, 0, pool, buffer_size, output_buffers, output_buffers);
    while (gst_query_get_n_allocation_pools(query) > 1) {
      gst_query_remove_nth_allocation_pool(query, 1);
    }
  } else if (need_pool || output_nvmm) {
    gst_query_add_allocation_pool(query, pool, buffer_size, output_buffers, output_buffers);
  }
  gst_object_unref(pool);

  return GST_BASE_TRANSFORM_CLASS(gst_dsx_video_convert_parent_class)->decide_allocation(transform, query);
}

static GstFlowReturn gst_dsx_video_convert_prepare_output_buffer(
    GstBaseTransform* transform,
    GstBuffer* input,
    GstBuffer** output) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  g_mutex_lock(&self->lock);
  const gboolean needs_batched_raw = !self->output_nvmm && self->batch_size > 1;
  const gsize output_size = self->output_info.size * self->batch_size;
  g_mutex_unlock(&self->lock);
  if (!needs_batched_raw) {
    return GST_BASE_TRANSFORM_CLASS(gst_dsx_video_convert_parent_class)
        ->prepare_output_buffer(transform, input, output);
  }
  *output = gst_buffer_new_allocate(nullptr, output_size, nullptr);
  return *output != nullptr ? GST_FLOW_OK : GST_FLOW_ERROR;
}

static GstFlowReturn gst_dsx_video_convert_transform(GstBaseTransform* transform, GstBuffer* input, GstBuffer* output) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(transform);
  gchar* error_message = nullptr;

  g_mutex_lock(&self->lock);
  const bool success =
      self->backend->transform(input, output, self->source_crop, self->destination_crop, &error_message);
  const MetadataTransform metadata = metadata_transform(self);
  const gboolean silent = self->silent;
  g_mutex_unlock(&self->lock);
  if (!success) {
    GST_ELEMENT_ERROR(
        self,
        STREAM,
        FAILED,
        ("video conversion failed"),
        ("%s", error_message != nullptr ? error_message : "unknown error"));
    g_free(error_message);
    return GST_FLOW_ERROR;
  }
  scale_metadata(metadata, output);
  if (!silent) {
    GST_INFO_OBJECT(self, "transformed buffer %p into %p", input, output);
  }
  return GST_FLOW_OK;
}

static void gst_dsx_video_convert_finalize(GObject* object) {
  GstDsxVideoConvert* self = GST_DSX_VIDEO_CONVERT(object);
  delete self->backend;
  self->backend = nullptr;
  gst_clear_caps(&self->input_caps);
  gst_clear_caps(&self->output_caps);
  g_clear_pointer(&self->source_crop_string, g_free);
  g_clear_pointer(&self->destination_crop_string, g_free);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(gst_dsx_video_convert_parent_class)->finalize(object);
}

static void gst_dsx_video_convert_class_init(GstDsxVideoConvertClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
  GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_CLASS(klass);

  object_class->set_property = gst_dsx_video_convert_set_property;
  object_class->get_property = gst_dsx_video_convert_get_property;
  object_class->finalize = gst_dsx_video_convert_finalize;

  g_object_class_install_property(
      object_class,
      PROP_SILENT,
      g_param_spec_boolean(
          "silent",
          "Silent",
          "Produce verbose output ?",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_GPU_ID,
      g_param_spec_uint(
          "gpu-id",
          "GPU ID",
          "Set GPU Device ID for operation",
          0,
          G_MAXUINT,
          0,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_OUTPUT_BUFFERS,
      g_param_spec_uint(
          "output-buffers",
          "Output-Buffers",
          "number of output buffers",
          1,
          G_MAXUINT,
          4,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_SOURCE_CROP,
      g_param_spec_string(
          "src-crop",
          "Src-Crop",
          "Pixel location left:top:width:height",
          "0:0:0:0",
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_DESTINATION_CROP,
      g_param_spec_string(
          "dest-crop",
          "Dest-Crop",
          "Pixel location left:top:width:height",
          "0:0:0:0",
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_BLOCK_LINEAR_OUTPUT,
      g_param_spec_boolean(
          "bl-output",
          "Blocklinear output",
          "Blocklinear output, applicable only for memory:NVMM NV12 output",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_ALLOW_ODD_CROP,
      g_param_spec_boolean(
          "allow-odd-crop",
          "Allow Odd Crop",
          "Allow the odd dimensions for source and destination crop rectangle",
          TRUE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_NVBUF_MEMORY_TYPE,
      g_param_spec_enum(
          "nvbuf-memory-type",
          "NvBuf Memory Type",
          "Type of NvBufSurface Memory to be allocated for output buffers",
          dsx_memory_type_get_type(),
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
          0,
#else
          2,
#endif
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_COMPUTE_HW,
      g_param_spec_enum(
          "compute-hw",
          "Compute Scaling HW",
          "Compute Scaling HW",
          dsx_compute_hw_get_type(),
          0,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_INTERPOLATION_METHOD,
      g_param_spec_enum(
          "interpolation-method",
          "Interpolation-method",
          "Set interpolation methods",
          dsx_interpolation_method_get_type(),
          6,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_COPY_HW,
      g_param_spec_enum(
          "copy-hw",
          "Copy HW",
          "Select hardware used for surface copies.",
          dsx_copy_hw_get_type(),
          1,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_CONTIGUOUS_BUFFERS,
      g_param_spec_boolean(
          "contiguous-buffers",
          "Contiguous Buffers",
          "Transformed output buffers in a batch are contiguous in memory.",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_DISABLE_PASSTHROUGH,
      g_param_spec_boolean(
          "disable-passthrough",
          "Disable Passthrough",
          "Disable passthrough mode at init time",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      object_class,
      PROP_FLIP_METHOD,
      g_param_spec_enum(
          "flip-method",
          "Video flip methods",
          "video flip methods",
          dsx_flip_method_get_type(),
          0,
          static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &source_template);
  gst_element_class_set_static_metadata(
      element_class,
      "DSX Video Convert Plugin",
      "Filter/Converter/Video/Scaler",
      "Converts video colorspace, size, crop, and orientation using "
      "NvBufSurfTransform",
      "NVIDIA Corporation");

  transform_class->start = gst_dsx_video_convert_start;
  transform_class->stop = gst_dsx_video_convert_stop;
  transform_class->set_caps = gst_dsx_video_convert_set_caps;
  transform_class->transform_caps = gst_dsx_video_convert_transform_caps;
  transform_class->accept_caps = gst_dsx_video_convert_accept_caps;
  transform_class->fixate_caps = gst_dsx_video_convert_fixate_caps;
  transform_class->get_unit_size = gst_dsx_video_convert_get_unit_size;
  transform_class->transform_size = gst_dsx_video_convert_transform_size;
  transform_class->decide_allocation = gst_dsx_video_convert_decide_allocation;
  transform_class->transform_meta = gst_dsx_video_convert_transform_meta;
  transform_class->prepare_output_buffer = gst_dsx_video_convert_prepare_output_buffer;
  transform_class->transform = gst_dsx_video_convert_transform;
}

static void gst_dsx_video_convert_init(GstDsxVideoConvert* self) {
  gst_video_info_init(&self->input_info);
  gst_video_info_init(&self->output_info);
  self->input_caps = nullptr;
  self->output_caps = nullptr;
  self->input_nvmm = FALSE;
  self->output_nvmm = FALSE;
  self->batch_size = 1;
  self->silent = FALSE;
  self->gpu_id = 0;
  self->output_buffers = 4;
  self->source_crop_string = g_strdup("0:0:0:0");
  self->destination_crop_string = g_strdup("0:0:0:0");
  self->source_crop = {};
  self->destination_crop = {};
  self->block_linear_output = FALSE;
  self->allow_odd_crop = TRUE;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  self->nvbuf_memory_type = 0;
#else
  self->nvbuf_memory_type = 2;
#endif
  self->compute_hw = 0;
  self->interpolation_method = 6;
  self->copy_hw = 1;
  self->contiguous_buffers = FALSE;
  self->disable_passthrough = FALSE;
  self->flip_method = 0;
  self->backend = new DsxVideoConvertBackend;
  g_mutex_init(&self->lock);

  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), FALSE);
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
  gst_base_transform_set_qos_enabled(GST_BASE_TRANSFORM(self), FALSE);
}

static gboolean plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_dsx_video_convert_debug, "dsxvideoconvert", 0, "DSX video converter");
  return gst_element_register(plugin, "dsxvideoconvert", GST_RANK_PRIMARY, GST_TYPE_DSX_VIDEO_CONVERT);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dsxvideoconvert,
    "Open DeepStream video colorspace converter and scaler",
    plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE_NAME,
    PACKAGE_ORIGIN)
