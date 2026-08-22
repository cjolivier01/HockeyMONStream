/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "dsxvideoconvert_backend.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nvds_version.h"

namespace {

void set_error(gchar** error_message, const gchar* format, ...) {
  if (error_message == nullptr) {
    return;
  }

  va_list arguments;
  va_start(arguments, format);
  *error_message = g_strdup_vprintf(format, arguments);
  va_end(arguments);
}

bool validate_nvmm_descriptor(
    const GstMapInfo& map,
    const gchar* role,
    bool require_filled,
    guint negotiated_batch_size,
    guint* filled,
    gchar** error_message) {
  if (map.data == nullptr || map.size < sizeof(NvBufSurface)) {
    set_error(
        error_message,
        "%s NVMM buffer has %" G_GSIZE_FORMAT " bytes; need at least %zu for an NvBufSurface descriptor",
        role,
        map.size,
        sizeof(NvBufSurface));
    return false;
  }

  const auto* surface = reinterpret_cast<const NvBufSurface*>(map.data);
  if (surface->surfaceList == nullptr) {
    set_error(error_message, "%s NvBufSurface has no surface list", role);
    return false;
  }
  if (surface->batchSize == 0) {
    set_error(error_message, "%s NvBufSurface has a zero batch size", role);
    return false;
  }
  if (surface->numFilled > surface->batchSize) {
    set_error(
        error_message,
        "%s NvBufSurface has %u filled surfaces but capacity %u",
        role,
        surface->numFilled,
        surface->batchSize);
    return false;
  }
  if (require_filled && surface->numFilled == 0) {
    set_error(error_message, "%s NvBufSurface contains no filled surfaces", role);
    return false;
  }
  if (require_filled && surface->numFilled > negotiated_batch_size) {
    set_error(
        error_message,
        "%s NvBufSurface has %u filled surfaces but negotiated batch "
        "capacity is %u",
        role,
        surface->numFilled,
        negotiated_batch_size);
    return false;
  }
  if (filled != nullptr) {
    *filled = surface->numFilled;
  }
  return true;
}

bool is_full_range(const GstVideoInfo& info) {
  return info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
}

bool is_bt709(const GstVideoInfo& info) {
  return info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709;
}

bool is_bt2020(const GstVideoInfo& info) {
  return info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020;
}

bool is_planar_yuv(const GstVideoInfo& info) {
  switch (GST_VIDEO_INFO_FORMAT(&info)) {
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_Y42B:
      return true;
    default:
      return false;
  }
}

NvBufSurfaceColorFormat yuv420_format(const GstVideoInfo& info) {
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_YUV420_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV420_709_ER : NVBUF_COLOR_FORMAT_YUV420_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV420_ER : NVBUF_COLOR_FORMAT_YUV420;
}

NvBufSurfaceColorFormat nv12_format(const GstVideoInfo& info) {
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_NV12_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_709_ER : NVBUF_COLOR_FORMAT_NV12_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_ER : NVBUF_COLOR_FORMAT_NV12;
}

NvBufSurfaceColorFormat p010_format(const GstVideoInfo& info) {
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_NV12_10LE_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_10LE_709_ER : NVBUF_COLOR_FORMAT_NV12_10LE_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_10LE_ER : NVBUF_COLOR_FORMAT_NV12_10LE;
}

NvBufSurfaceColorFormat nv12_12_format(const GstVideoInfo& info) {
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_NV12_12LE_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_12LE_709_ER : NVBUF_COLOR_FORMAT_NV12_12LE_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_NV12_12LE_ER : NVBUF_COLOR_FORMAT_NV12_12LE;
}

NvBufSurfaceColorFormat y444_format(const GstVideoInfo& info, guint bits) {
  if (bits == 10) {
    if (is_bt2020(info)) {
      return NVBUF_COLOR_FORMAT_YUV444_10LE_2020;
    }
    if (is_bt709(info)) {
      return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_10LE_709_ER : NVBUF_COLOR_FORMAT_YUV444_10LE_709;
    }
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_10LE_ER : NVBUF_COLOR_FORMAT_YUV444_10LE;
  }
  if (bits == 12) {
    if (is_bt2020(info)) {
      return NVBUF_COLOR_FORMAT_YUV444_12LE_2020;
    }
    if (is_bt709(info)) {
      return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_12LE_709_ER : NVBUF_COLOR_FORMAT_YUV444_12LE_709;
    }
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_12LE_ER : NVBUF_COLOR_FORMAT_YUV444_12LE;
  }
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_YUV444_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_709_ER : NVBUF_COLOR_FORMAT_YUV444_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUV444_ER : NVBUF_COLOR_FORMAT_YUV444;
}

NvBufSurfaceColorFormat uyvy_format(const GstVideoInfo& info) {
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_UYVY_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_UYVY_709_ER : NVBUF_COLOR_FORMAT_UYVY_709;
  }
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_UYVY_ER : NVBUF_COLOR_FORMAT_UYVY;
}

NvBufSurfaceColorFormat uyvp_format(const GstVideoInfo& info) {
#if NVDS_VERSION_MAJOR >= 9
  if (is_bt2020(info)) {
    return NVBUF_COLOR_FORMAT_UYVP_2020;
  }
  if (is_bt709(info)) {
    return is_full_range(info) ? NVBUF_COLOR_FORMAT_UYVP_709_ER : NVBUF_COLOR_FORMAT_UYVP_709;
  }
#endif
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_UYVP_ER : NVBUF_COLOR_FORMAT_UYVP;
}

NvBufSurfaceColorFormat yuy2_format(const GstVideoInfo& info) {
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_YUYV_ER : NVBUF_COLOR_FORMAT_YUYV;
}

NvBufSurfaceColorFormat yvyu_format(const GstVideoInfo& info) {
  return is_full_range(info) ? NVBUF_COLOR_FORMAT_YVYU_ER : NVBUF_COLOR_FORMAT_YVYU;
}

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
NvBufSurfaceMemType jetson_staging_memory_type(const DsxBackendOptions& options) {
  return options.compute_hw == 1 && options.copy_hw != 2 ? NVBUF_MEM_CUDA_DEVICE : NVBUF_MEM_SURFACE_ARRAY;
}

NvBufSurfaceMemType jetson_copy_memory_type(const DsxBackendOptions& options) {
  return options.copy_hw == 2 ? NVBUF_MEM_SURFACE_ARRAY : NVBUF_MEM_CUDA_DEVICE;
}
#endif

NvBufSurfTransform_Flip flip_method(guint method) {
  switch (method) {
    case 1:
      return NvBufSurfTransform_Rotate90;
    case 2:
      return NvBufSurfTransform_Rotate180;
    case 3:
      return NvBufSurfTransform_Rotate270;
    case 4:
      return NvBufSurfTransform_FlipX;
    case 5:
      return NvBufSurfTransform_InvTranspose;
    case 6:
      return NvBufSurfTransform_FlipY;
    case 7:
      return NvBufSurfTransform_Transpose;
    default:
      return NvBufSurfTransform_None;
  }
}

guint raw_plane_rows(const GstVideoInfo& info, guint plane) {
  for (guint component = 0; component < GST_VIDEO_INFO_N_COMPONENTS(&info); ++component) {
    if (GST_VIDEO_INFO_COMP_PLANE(&info, component) == plane) {
      return GST_VIDEO_INFO_COMP_HEIGHT(&info, component);
    }
  }
  return 0;
}

guint raw_visible_row_bytes(const GstVideoInfo& info, guint plane) {
  if (GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_UYVP) {
    return plane == 0 ? GST_VIDEO_INFO_WIDTH(&info) * 5U / 2U : 0;
  }
  for (guint component = 0; component < GST_VIDEO_INFO_N_COMPONENTS(&info); ++component) {
    if (GST_VIDEO_INFO_COMP_PLANE(&info, component) == plane) {
      return GST_VIDEO_INFO_COMP_WIDTH(&info, component) * GST_VIDEO_INFO_COMP_PSTRIDE(&info, component);
    }
  }
  return 0;
}

bool raw_plane_in_bounds(gsize size, gsize frame_offset, gsize plane_offset, gint stride, guint rows, guint row_bytes) {
  if (rows == 0 || row_bytes == 0 || frame_offset > size || plane_offset > size - frame_offset) {
    return false;
  }
  const gint64 first = static_cast<gint64>(frame_offset + plane_offset);
  const gint64 last = first + static_cast<gint64>(rows - 1) * stride;
  const gint64 lowest = std::min(first, last);
  const gint64 highest = std::max(first, last);
  return lowest >= 0 && static_cast<guint64>(highest) + row_bytes <= size;
}

bool mapped_plane_in_bounds(const GstMapInfo& map, const guint8* data, gint stride, guint rows, guint row_bytes) {
  if (map.data == nullptr || data == nullptr || rows == 0 || row_bytes == 0 ||
      map.size > static_cast<gsize>(G_MAXINT64)) {
    return false;
  }
  const std::uintptr_t map_address = reinterpret_cast<std::uintptr_t>(map.data);
  const std::uintptr_t data_address = reinterpret_cast<std::uintptr_t>(data);
  if (data_address < map_address || data_address - map_address > map.size) {
    return false;
  }
  const gint64 first = static_cast<gint64>(data_address - map_address);
  const gint64 last = first + static_cast<gint64>(rows - 1) * stride;
  const gint64 lowest = std::min(first, last);
  const gint64 highest = std::max(first, last);
  return lowest >= 0 && static_cast<guint64>(highest) + row_bytes <= map.size;
}

class RawBufferMapping {
 public:
  ~RawBufferMapping() {
    unmap();
  }

  RawBufferMapping(const RawBufferMapping&) = delete;
  RawBufferMapping& operator=(const RawBufferMapping&) = delete;

  RawBufferMapping() = default;

  bool map(
      GstBuffer* buffer,
      const GstVideoInfo& info,
      guint batch_size,
      GstMapFlags flags,
      const gchar* role,
      gchar** error_message) {
    info_ = &info;
    buffer_ = buffer;
    video_meta_ = batch_size == 1 ? gst_buffer_get_video_meta(buffer) : nullptr;
    if (video_meta_ != nullptr) {
      if (video_meta_->format != GST_VIDEO_INFO_FORMAT(&info) ||
          video_meta_->width != static_cast<guint>(GST_VIDEO_INFO_WIDTH(&info)) ||
          video_meta_->height != static_cast<guint>(GST_VIDEO_INFO_HEIGHT(&info)) ||
          video_meta_->n_planes < GST_VIDEO_INFO_N_PLANES(&info)) {
        set_error(error_message, "%s GstVideoMeta does not match negotiated video layout", role);
        return false;
      }
      for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane) {
        plane_maps_[plane] = GST_MAP_INFO_INIT;
        gpointer data = nullptr;
        gint stride = 0;
        if (!gst_video_meta_map(video_meta_, plane, &plane_maps_[plane], &data, &stride, flags)) {
          set_error(error_message, "failed to map %s video plane %u", role, plane);
          return false;
        }
        plane_mapped_[plane] = true;
        plane_data_[plane] = static_cast<guint8*>(data);
        plane_stride_[plane] = stride;
      }
      return true;
    }

    buffer_map_ = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(buffer, &buffer_map_, flags)) {
      set_error(error_message, "failed to map %s buffer", role);
      return false;
    }
    buffer_mapped_ = true;
    if (info.size == 0 || batch_size > G_MAXSIZE / info.size || buffer_map_.size < info.size * batch_size) {
      set_error(
          error_message,
          "%s buffer has %" G_GSIZE_FORMAT " bytes; need at least %" G_GSIZE_FORMAT,
          role,
          buffer_map_.size,
          batch_size > G_MAXSIZE / std::max<gsize>(1, info.size) ? G_MAXSIZE : info.size * batch_size);
      return false;
    }
    return true;
  }

  guint8* plane_data(guint batch, guint plane) const {
    if (video_meta_ != nullptr) {
      return plane_data_[plane];
    }
    return buffer_map_.data + batch * info_->size + GST_VIDEO_INFO_PLANE_OFFSET(info_, plane);
  }

  gint plane_stride(guint plane) const {
    return video_meta_ != nullptr ? plane_stride_[plane] : GST_VIDEO_INFO_PLANE_STRIDE(info_, plane);
  }

  bool plane_in_bounds(guint batch, guint plane, guint rows, guint row_bytes) const {
    const gint stride = plane_stride(plane);
    if (video_meta_ != nullptr) {
      return mapped_plane_in_bounds(plane_maps_[plane], plane_data_[plane], stride, rows, row_bytes);
    }
    return raw_plane_in_bounds(
        buffer_map_.size, batch * info_->size, GST_VIDEO_INFO_PLANE_OFFSET(info_, plane), stride, rows, row_bytes);
  }

 private:
  void unmap() {
    if (video_meta_ != nullptr) {
      for (guint plane = 0; plane < GST_VIDEO_MAX_PLANES; ++plane) {
        if (plane_mapped_[plane]) {
          gst_video_meta_unmap(video_meta_, plane, &plane_maps_[plane]);
          plane_mapped_[plane] = false;
        }
      }
    }
    if (buffer_mapped_) {
      gst_buffer_unmap(buffer_, &buffer_map_);
      buffer_mapped_ = false;
    }
  }

  const GstVideoInfo* info_ = nullptr;
  GstBuffer* buffer_ = nullptr;
  GstVideoMeta* video_meta_ = nullptr;
  GstMapInfo buffer_map_ = GST_MAP_INFO_INIT;
  bool buffer_mapped_ = false;
  std::array<GstMapInfo, GST_VIDEO_MAX_PLANES> plane_maps_{};
  std::array<guint8*, GST_VIDEO_MAX_PLANES> plane_data_{};
  std::array<gint, GST_VIDEO_MAX_PLANES> plane_stride_{};
  std::array<bool, GST_VIDEO_MAX_PLANES> plane_mapped_{};
};

bool copy_raw_to_surface(
    const GstVideoInfo& info,
    GstBuffer* buffer,
    NvBufSurface* surface,
    guint batch_size,
    guint copy_hw,
    gchar** error_message) {
  RawBufferMapping raw;
  if (!raw.map(buffer, info, batch_size, GST_MAP_READ, "raw input", error_message)) {
    return false;
  }

  const guint raw_planes = GST_VIDEO_INFO_N_PLANES(&info);
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  const bool use_nvbuf_raw_copy = copy_hw == 2;
#else
  static_cast<void>(copy_hw);
  const bool use_nvbuf_raw_copy = false;
#endif
  const bool map_surface =
      !use_nvbuf_raw_copy && (surface->memType == NVBUF_MEM_SURFACE_ARRAY || surface->memType == NVBUF_MEM_HANDLE);
  surface->numFilled = batch_size;
  for (guint batch = 0; batch < batch_size; ++batch) {
    NvBufSurfaceParams& params = surface->surfaceList[batch];
    bool batch_success = true;
    if (map_surface && NvBufSurfaceMap(surface, batch, -1, NVBUF_MAP_WRITE) != 0) {
      set_error(error_message, "failed to map raw-input staging surface");
      return false;
    }
    for (guint raw_plane = 0; raw_plane < raw_planes; ++raw_plane) {
      const guint surface_plane = raw_plane;
      if (surface_plane >= params.planeParams.num_planes) {
        set_error(error_message, "plane mapping exceeds NvBufSurface layout");
        batch_success = false;
        break;
      }
      const gint raw_stride = raw.plane_stride(raw_plane);
      const guint absolute_stride = static_cast<guint>(std::abs(static_cast<gint64>(raw_stride)));
      const guint visible_row_bytes = raw_visible_row_bytes(info, raw_plane);
      const guint rows = raw_plane_rows(info, raw_plane);
      const guint surface_pitch = params.planeParams.pitch[surface_plane];
      if (raw_stride == 0 || absolute_stride < visible_row_bytes) {
        set_error(
            error_message,
            "raw input plane %u stride %d is smaller than %u bytes",
            raw_plane,
            raw_stride,
            visible_row_bytes);
        batch_success = false;
        break;
      }
      if (params.planeParams.height[surface_plane] < rows || surface_pitch < visible_row_bytes) {
        set_error(error_message, "raw input plane %u exceeds the staging surface layout", raw_plane);
        batch_success = false;
        break;
      }
      if (!raw.plane_in_bounds(batch, raw_plane, rows, visible_row_bytes)) {
        set_error(error_message, "raw input plane exceeds buffer bounds");
        batch_success = false;
        break;
      }
      const guint8* source = raw.plane_data(batch, raw_plane);
      if (use_nvbuf_raw_copy) {
        std::vector<guint8> packed;
        if (raw_stride != static_cast<gint>(visible_row_bytes)) {
          packed.resize(static_cast<gsize>(visible_row_bytes) * rows);
          for (guint row = 0; row < rows; ++row) {
            std::memcpy(
                packed.data() + static_cast<gsize>(row) * visible_row_bytes,
                source + static_cast<gint64>(row) * raw_stride,
                visible_row_bytes);
          }
          source = packed.data();
        }
        if (Raw2NvBufSurface(
                const_cast<guint8*>(source),
                batch,
                surface_plane,
                params.planeParams.width[surface_plane],
                rows,
                surface) != 0) {
          set_error(error_message, "VIC raw upload failed for plane %u", raw_plane);
          batch_success = false;
          break;
        }
        continue;
      }
      guint8* destination = map_surface
          ? static_cast<guint8*>(params.mappedAddr.addr[surface_plane])
          : static_cast<guint8*>(params.dataPtr) + params.planeParams.offset[surface_plane];
      if (destination == nullptr) {
        set_error(error_message, "raw-input staging plane is not addressable");
        batch_success = false;
        break;
      }
      if (map_surface || raw_stride < 0) {
        for (guint row = 0; row < rows; ++row) {
          const guint8* source_row = source + static_cast<gint64>(row) * raw_stride;
          if (map_surface) {
            std::memcpy(destination + row * surface_pitch, source_row, visible_row_bytes);
          } else {
            const cudaError_t copy_error =
                cudaMemcpy(destination + row * surface_pitch, source_row, visible_row_bytes, cudaMemcpyHostToDevice);
            if (copy_error != cudaSuccess) {
              set_error(error_message, "raw upload failed: %s", cudaGetErrorString(copy_error));
              batch_success = false;
              break;
            }
          }
        }
      } else {
        const cudaError_t copy_error = cudaMemcpy2D(
            destination, surface_pitch, source, raw_stride, visible_row_bytes, rows, cudaMemcpyHostToDevice);
        if (copy_error != cudaSuccess) {
          set_error(error_message, "raw upload failed: %s", cudaGetErrorString(copy_error));
          batch_success = false;
          break;
        }
      }
    }
    if (map_surface && batch_success) {
      for (guint plane = 0; plane < params.planeParams.num_planes; ++plane) {
        if (NvBufSurfaceSyncForDevice(surface, batch, static_cast<int>(plane)) != 0) {
          set_error(error_message, "failed to sync raw-input staging surface plane %u", plane);
          batch_success = false;
          break;
        }
      }
    }
    if (map_surface) {
      NvBufSurfaceUnMap(surface, batch, -1);
    }
    if (!batch_success) {
      return false;
    }
  }
  return true;
}

bool copy_surface_to_raw(
    const NvBufSurface* surface,
    const GstVideoInfo& info,
    GstBuffer* buffer,
    guint batch_size,
    guint copy_hw,
    gchar** error_message) {
  RawBufferMapping raw;
  if (!raw.map(buffer, info, batch_size, GST_MAP_WRITE, "raw output", error_message)) {
    return false;
  }

  const guint raw_planes = GST_VIDEO_INFO_N_PLANES(&info);
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  const bool use_nvbuf_raw_copy = copy_hw == 2;
#else
  static_cast<void>(copy_hw);
  const bool use_nvbuf_raw_copy = false;
#endif
  const bool map_surface =
      !use_nvbuf_raw_copy && (surface->memType == NVBUF_MEM_SURFACE_ARRAY || surface->memType == NVBUF_MEM_HANDLE);
  for (guint batch = 0; batch < batch_size; ++batch) {
    const NvBufSurfaceParams& params = surface->surfaceList[batch];
    bool batch_success = true;
    NvBufSurface* writable_surface = const_cast<NvBufSurface*>(surface);
    if (map_surface && NvBufSurfaceMap(writable_surface, batch, -1, NVBUF_MAP_READ) != 0) {
      set_error(error_message, "failed to map raw-output staging surface");
      return false;
    }
    if (map_surface) {
      for (guint plane = 0; plane < params.planeParams.num_planes; ++plane) {
        if (NvBufSurfaceSyncForCpu(writable_surface, batch, static_cast<int>(plane)) != 0) {
          NvBufSurfaceUnMap(writable_surface, batch, -1);
          set_error(error_message, "failed to sync raw-output staging surface plane %u", plane);
          return false;
        }
      }
    }
    for (guint raw_plane = 0; raw_plane < raw_planes; ++raw_plane) {
      const guint surface_plane = raw_plane;
      if (surface_plane >= params.planeParams.num_planes) {
        set_error(error_message, "plane mapping exceeds NvBufSurface layout");
        batch_success = false;
        break;
      }
      const gint raw_stride = raw.plane_stride(raw_plane);
      const guint absolute_stride = static_cast<guint>(std::abs(static_cast<gint64>(raw_stride)));
      const guint visible_row_bytes = raw_visible_row_bytes(info, raw_plane);
      const guint rows = raw_plane_rows(info, raw_plane);
      const guint surface_pitch = params.planeParams.pitch[surface_plane];
      if (raw_stride == 0 || absolute_stride < visible_row_bytes) {
        set_error(
            error_message,
            "raw output plane %u stride %d is smaller than %u bytes",
            raw_plane,
            raw_stride,
            visible_row_bytes);
        batch_success = false;
        break;
      }
      if (params.planeParams.height[surface_plane] < rows || surface_pitch < visible_row_bytes) {
        set_error(error_message, "raw output plane %u exceeds the staging surface layout", raw_plane);
        batch_success = false;
        break;
      }
      if (!raw.plane_in_bounds(batch, raw_plane, rows, absolute_stride)) {
        set_error(error_message, "raw output plane exceeds buffer bounds");
        batch_success = false;
        break;
      }
      guint8* destination = raw.plane_data(batch, raw_plane);
      for (guint row = 0; row < rows; ++row) {
        guint8* destination_row = destination + static_cast<gint64>(row) * raw_stride;
        std::memset(destination_row, 0, absolute_stride);
      }
      if (use_nvbuf_raw_copy) {
        std::vector<guint8> packed;
        guint8* packed_destination = destination;
        if (raw_stride != static_cast<gint>(visible_row_bytes)) {
          packed.resize(static_cast<gsize>(visible_row_bytes) * rows);
          packed_destination = packed.data();
        }
        if (NvBufSurface2Raw(
                writable_surface,
                batch,
                surface_plane,
                params.planeParams.width[surface_plane],
                rows,
                packed_destination) != 0) {
          set_error(error_message, "VIC raw download failed for plane %u", raw_plane);
          batch_success = false;
          break;
        }
        if (!packed.empty()) {
          for (guint row = 0; row < rows; ++row) {
            std::memcpy(
                destination + static_cast<gint64>(row) * raw_stride,
                packed.data() + static_cast<gsize>(row) * visible_row_bytes,
                visible_row_bytes);
          }
        }
        continue;
      }
      const guint8* source = map_surface
          ? static_cast<const guint8*>(params.mappedAddr.addr[surface_plane])
          : static_cast<const guint8*>(params.dataPtr) + params.planeParams.offset[surface_plane];
      if (source == nullptr) {
        set_error(error_message, "raw-output staging plane is not addressable");
        batch_success = false;
        break;
      }
      if (map_surface || raw_stride < 0) {
        for (guint row = 0; row < rows; ++row) {
          guint8* destination_row = destination + static_cast<gint64>(row) * raw_stride;
          if (map_surface) {
            std::memcpy(destination_row, source + row * surface_pitch, visible_row_bytes);
          } else {
            const cudaError_t copy_error =
                cudaMemcpy(destination_row, source + row * surface_pitch, visible_row_bytes, cudaMemcpyDeviceToHost);
            if (copy_error != cudaSuccess) {
              set_error(error_message, "raw download failed: %s", cudaGetErrorString(copy_error));
              batch_success = false;
              break;
            }
          }
        }
      } else {
        const cudaError_t copy_error = cudaMemcpy2D(
            destination, raw_stride, source, surface_pitch, visible_row_bytes, rows, cudaMemcpyDeviceToHost);
        if (copy_error != cudaSuccess) {
          set_error(error_message, "raw download failed: %s", cudaGetErrorString(copy_error));
          batch_success = false;
          break;
        }
      }
    }
    if (map_surface) {
      NvBufSurfaceUnMap(writable_surface, batch, -1);
    }
    if (!batch_success) {
      return false;
    }
  }
  return true;
}

} // namespace

NvBufSurfaceColorFormat dsx_video_info_to_nvbuf_format(const GstVideoInfo& info) {
  switch (GST_VIDEO_INFO_FORMAT(&info)) {
    case GST_VIDEO_FORMAT_I420:
      return yuv420_format(info);
    case GST_VIDEO_FORMAT_NV12:
      return nv12_format(info);
    case GST_VIDEO_FORMAT_P010_10LE:
      return p010_format(info);
    case GST_VIDEO_FORMAT_I420_12LE:
      return nv12_12_format(info);
    case GST_VIDEO_FORMAT_BGRx:
      return NVBUF_COLOR_FORMAT_BGRx;
    case GST_VIDEO_FORMAT_RGBA:
      return NVBUF_COLOR_FORMAT_RGBA;
    case GST_VIDEO_FORMAT_Y444:
      return y444_format(info, 8);
    case GST_VIDEO_FORMAT_Y444_10LE:
      return y444_format(info, 10);
    case GST_VIDEO_FORMAT_Y444_12LE:
      return y444_format(info, 12);
    case GST_VIDEO_FORMAT_GRAY8:
      return is_full_range(info) ? NVBUF_COLOR_FORMAT_GRAY8_ER : NVBUF_COLOR_FORMAT_GRAY8;
    case GST_VIDEO_FORMAT_GRAY16_LE:
      return NVBUF_COLOR_FORMAT_GRAY16_LE;
    case GST_VIDEO_FORMAT_GBR:
      return NVBUF_COLOR_FORMAT_R8_G8_B8;
    case GST_VIDEO_FORMAT_RGB:
      return NVBUF_COLOR_FORMAT_RGB;
    case GST_VIDEO_FORMAT_BGR:
      return NVBUF_COLOR_FORMAT_BGR;
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      return is_bt2020(info) ? NVBUF_COLOR_FORMAT_BGRA_10_10_10_2_2020 : NVBUF_COLOR_FORMAT_BGRA_10_10_10_2_709;
    case GST_VIDEO_FORMAT_RGB10A2_LE:
      return is_bt2020(info) ? NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020 : NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709;
    case GST_VIDEO_FORMAT_UYVP:
      return uyvp_format(info);
    case GST_VIDEO_FORMAT_UYVY:
      return uyvy_format(info);
    case GST_VIDEO_FORMAT_YUY2:
      return yuy2_format(info);
    case GST_VIDEO_FORMAT_YVYU:
      return yvyu_format(info);
    case GST_VIDEO_FORMAT_Y42B:
      return NVBUF_COLOR_FORMAT_YUV422;
    case GST_VIDEO_FORMAT_BGRA64_LE:
#if NVDS_VERSION_MAJOR >= 9
      return NVBUF_COLOR_FORMAT_BGRA64_LE;
#else
      return NVBUF_COLOR_FORMAT_INVALID;
#endif
    default:
      return NVBUF_COLOR_FORMAT_INVALID;
  }
}

DsxVideoConvertBackend::DsxVideoConvertBackend() {
  gst_video_info_init(&input_info_);
  gst_video_info_init(&output_info_);
}

DsxVideoConvertBackend::~DsxVideoConvertBackend() {
  stop();
}

bool DsxVideoConvertBackend::start(guint gpu_id, gchar** error_message) {
  if (started_ && gpu_id != gpu_id_) {
    set_error(error_message, "cannot change GPU from %u to %u while the backend is active", gpu_id_, gpu_id);
    return false;
  }
  const cudaError_t select_error = cudaSetDevice(static_cast<int>(gpu_id));
  if (select_error != cudaSuccess) {
    set_error(error_message, "cudaSetDevice(%u) failed: %s", gpu_id, cudaGetErrorString(select_error));
    return false;
  }
  if (started_) {
    return true;
  }
  gpu_id_ = gpu_id;
  const cudaError_t cuda_error = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
  if (cuda_error != cudaSuccess) {
    set_error(error_message, "CUDA stream creation failed: %s", cudaGetErrorString(cuda_error));
    stream_ = nullptr;
    return false;
  }
  started_ = true;
  return true;
}

void DsxVideoConvertBackend::stop() {
  if (started_) {
    cudaSetDevice(static_cast<int>(gpu_id_));
  }
  if (input_staging_ != nullptr) {
    NvBufSurfaceDestroy(input_staging_);
    input_staging_ = nullptr;
  }
  if (input_copy_staging_ != nullptr) {
    NvBufSurfaceDestroy(input_copy_staging_);
    input_copy_staging_ = nullptr;
  }
  if (output_staging_ != nullptr) {
    NvBufSurfaceDestroy(output_staging_);
    output_staging_ = nullptr;
  }
  if (output_copy_staging_ != nullptr) {
    NvBufSurfaceDestroy(output_copy_staging_);
    output_copy_staging_ = nullptr;
  }
  if (stream_ != nullptr) {
    cudaStreamDestroy(stream_);
    stream_ = nullptr;
  }
  started_ = false;
}

bool DsxVideoConvertBackend::configure(
    const GstVideoInfo& input_info,
    const GstVideoInfo& output_info,
    bool input_nvmm,
    bool output_nvmm,
    guint batch_size,
    const DsxBackendOptions& options,
    gchar** error_message) {
  if (!start(options.gpu_id, error_message)) {
    return false;
  }
  if (input_staging_ != nullptr) {
    NvBufSurfaceDestroy(input_staging_);
    input_staging_ = nullptr;
  }
  if (input_copy_staging_ != nullptr) {
    NvBufSurfaceDestroy(input_copy_staging_);
    input_copy_staging_ = nullptr;
  }
  if (output_staging_ != nullptr) {
    NvBufSurfaceDestroy(output_staging_);
    output_staging_ = nullptr;
  }
  if (output_copy_staging_ != nullptr) {
    NvBufSurfaceDestroy(output_copy_staging_);
    output_copy_staging_ = nullptr;
  }

  input_info_ = input_info;
  output_info_ = output_info;
  input_nvmm_ = input_nvmm;
  output_nvmm_ = output_nvmm;
  batch_size_ = std::max(1U, batch_size);
  options_ = options;
  input_format_ = dsx_video_info_to_nvbuf_format(input_info_);
  output_format_ = dsx_video_info_to_nvbuf_format(output_info_);
  if (input_format_ == NVBUF_COLOR_FORMAT_INVALID || output_format_ == NVBUF_COLOR_FORMAT_INVALID) {
    set_error(error_message, "unsupported negotiated video format");
    return false;
  }
  NvBufSurfTransformConfigParams session{};
  session.compute_mode = static_cast<NvBufSurfTransform_Compute>(options_.compute_hw);
  session.gpu_id = static_cast<int32_t>(options_.gpu_id);
  session.cuda_stream = stream_;
  if (NvBufSurfTransformSetSessionParams(&session) != NvBufSurfTransformError_Success) {
    set_error(error_message, "NvBufSurfTransformSetSessionParams failed");
    return false;
  }

  return ensure_staging_surfaces(batch_size_, error_message);
}

bool DsxVideoConvertBackend::ensure_staging_surfaces(guint batch_size, gchar** error_message) {
  const cudaError_t select_error = cudaSetDevice(static_cast<int>(options_.gpu_id));
  if (select_error != cudaSuccess) {
    set_error(error_message, "cudaSetDevice(%u) failed: %s", options_.gpu_id, cudaGetErrorString(select_error));
    return false;
  }
  if (!input_nvmm_ && (input_staging_ == nullptr || input_staging_->batchSize != batch_size)) {
    if (input_staging_ != nullptr) {
      NvBufSurfaceDestroy(input_staging_);
      input_staging_ = nullptr;
    }
    if (input_copy_staging_ != nullptr) {
      NvBufSurfaceDestroy(input_copy_staging_);
      input_copy_staging_ = nullptr;
    }
    NvBufSurfaceCreateParams params{};
    params.gpuId = options_.gpu_id;
    params.width = GST_VIDEO_INFO_WIDTH(&input_info_);
    params.height = GST_VIDEO_INFO_HEIGHT(&input_info_);
    params.isContiguous = options_.contiguous_buffers;
    params.colorFormat = input_format_;
    params.layout = NVBUF_LAYOUT_PITCH;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    params.memType = jetson_staging_memory_type(options_);
#else
    params.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
    if (NvBufSurfaceCreate(&input_staging_, batch_size, &params) != 0) {
      set_error(error_message, "failed to allocate raw-input staging surface");
      return false;
    }
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    if (jetson_copy_memory_type(options_) != params.memType) {
      params.memType = jetson_copy_memory_type(options_);
      if (NvBufSurfaceCreate(&input_copy_staging_, batch_size, &params) != 0) {
        set_error(error_message, "failed to allocate raw-input copy surface");
        return false;
      }
    }
#endif
  }

  if (!output_nvmm_ && (output_staging_ == nullptr || output_staging_->batchSize != batch_size)) {
    if (output_staging_ != nullptr) {
      NvBufSurfaceDestroy(output_staging_);
      output_staging_ = nullptr;
    }
    if (output_copy_staging_ != nullptr) {
      NvBufSurfaceDestroy(output_copy_staging_);
      output_copy_staging_ = nullptr;
    }
    NvBufSurfaceCreateParams params{};
    params.gpuId = options_.gpu_id;
    params.width = GST_VIDEO_INFO_WIDTH(&output_info_);
    params.height = GST_VIDEO_INFO_HEIGHT(&output_info_);
    params.isContiguous = options_.contiguous_buffers;
    params.colorFormat = output_format_;
    params.layout = NVBUF_LAYOUT_PITCH;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    params.memType = jetson_staging_memory_type(options_);
#else
    params.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
    if (NvBufSurfaceCreate(&output_staging_, batch_size, &params) != 0) {
      set_error(error_message, "failed to allocate raw-output staging surface");
      return false;
    }
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    if (jetson_copy_memory_type(options_) != params.memType) {
      params.memType = jetson_copy_memory_type(options_);
      if (NvBufSurfaceCreate(&output_copy_staging_, batch_size, &params) != 0) {
        set_error(error_message, "failed to allocate raw-output copy surface");
        return false;
      }
    }
#endif
  }
  return true;
}

bool DsxVideoConvertBackend::upload_raw(
    GstBuffer* buffer,
    NvBufSurface* surface,
    guint batch_size,
    gchar** error_message) const {
  return copy_raw_to_surface(input_info_, buffer, surface, batch_size, options_.copy_hw, error_message);
}

bool DsxVideoConvertBackend::download_raw(
    const NvBufSurface* surface,
    GstBuffer* buffer,
    guint batch_size,
    gchar** error_message) const {
  return copy_surface_to_raw(surface, output_info_, buffer, batch_size, options_.copy_hw, error_message);
}

bool DsxVideoConvertBackend::copy_staging_surface(
    NvBufSurface* source,
    NvBufSurface* destination,
    guint batch_size,
    gchar** error_message) const {
  source->numFilled = batch_size;
  destination->numFilled = batch_size;
  NvBufSurfTransformConfigParams session{};
  session.compute_mode = static_cast<NvBufSurfTransform_Compute>(options_.copy_hw);
  session.gpu_id = static_cast<int32_t>(options_.gpu_id);
  session.cuda_stream = stream_;
  if (NvBufSurfTransformSetSessionParams(&session) != NvBufSurfTransformError_Success) {
    set_error(error_message, "failed to select copy-hw=%u", options_.copy_hw);
    return false;
  }
  NvBufSurfTransformParams params{};
  const NvBufSurfTransform_Error result = NvBufSurfTransform(source, destination, &params);
  if (result != NvBufSurfTransformError_Success) {
    set_error(
        error_message, "copy-hw=%u surface copy failed with error %d", options_.copy_hw, static_cast<int>(result));
    return false;
  }
  return true;
}

bool DsxVideoConvertBackend::transform(
    GstBuffer* input,
    GstBuffer* output,
    const DsxCropRect& source_crop,
    const DsxCropRect& destination_crop,
    gchar** error_message) {
  const cudaError_t select_error = cudaSetDevice(static_cast<int>(options_.gpu_id));
  if (select_error != cudaSuccess) {
    set_error(error_message, "cudaSetDevice(%u) failed: %s", options_.gpu_id, cudaGetErrorString(select_error));
    return false;
  }
  GstMapInfo input_map = GST_MAP_INFO_INIT;
  GstMapInfo output_map = GST_MAP_INFO_INIT;
  bool input_mapped = false;
  bool output_mapped = false;
  if (input_nvmm_ && !gst_buffer_map(input, &input_map, GST_MAP_READ)) {
    set_error(error_message, "failed to map input buffer");
    return false;
  }
  input_mapped = input_nvmm_;
  if (output_nvmm_ && !gst_buffer_map(output, &output_map, GST_MAP_READWRITE)) {
    if (input_mapped) {
      gst_buffer_unmap(input, &input_map);
    }
    set_error(error_message, "failed to map output buffer");
    return false;
  }
  output_mapped = output_nvmm_;

  guint batch_size = batch_size_;
  bool success = true;
  if (input_nvmm_) {
    success = validate_nvmm_descriptor(input_map, "input", true, batch_size_, &batch_size, error_message);
  }

  NvBufSurface* input_surface = input_nvmm_ ? reinterpret_cast<NvBufSurface*>(input_map.data) : nullptr;
  if (success && input_nvmm_ &&
      (input_surface->memType == NVBUF_MEM_DEFAULT || input_surface->memType == NVBUF_MEM_CUDA_DEVICE) &&
      input_surface->gpuId != options_.gpu_id) {
    set_error(
        error_message, "input surface GPU %u does not match configured GPU %u", input_surface->gpuId, options_.gpu_id);
    if (output_mapped) {
      gst_buffer_unmap(output, &output_map);
    }
    if (input_mapped) {
      gst_buffer_unmap(input, &input_map);
    }
    return false;
  }

  if (success) {
    success = ensure_staging_surfaces(batch_size, error_message);
  }
  if (!input_nvmm_) {
    input_surface = input_staging_;
  }
  if (success && !input_nvmm_) {
    NvBufSurface* copy_surface = input_copy_staging_ != nullptr ? input_copy_staging_ : input_surface;
    success = upload_raw(input, copy_surface, batch_size, error_message);
    if (success && input_copy_staging_ != nullptr) {
      success = copy_staging_surface(input_copy_staging_, input_surface, batch_size, error_message);
    }
  }

  NvBufSurface* output_surface = output_nvmm_ ? reinterpret_cast<NvBufSurface*>(output_map.data) : output_staging_;
  if (success && output_nvmm_) {
    success = validate_nvmm_descriptor(output_map, "output", false, batch_size_, nullptr, error_message);
  }
  if (success && output_surface->batchSize < batch_size) {
    set_error(error_message, "output surface batch size %u is smaller than %u", output_surface->batchSize, batch_size);
    success = false;
  }

  if (success) {
    output_surface->numFilled = batch_size;
    if (destination_crop.enabled()) {
      if (NvBufSurfaceMemSet(output_surface, -1, -1, 0) != 0) {
        set_error(error_message, "failed to clear destination surface");
        success = false;
      } else if (is_planar_yuv(output_info_)) {
        const guint planes = output_surface->surfaceList[0].planeParams.num_planes;
        for (guint plane = 1; plane < planes; ++plane) {
          if (NvBufSurfaceMemSet(output_surface, -1, static_cast<int>(plane), 128) != 0) {
            set_error(error_message, "failed to initialize destination chroma plane");
            success = false;
            break;
          }
        }
      }
    }

    std::vector<NvBufSurfTransformRect> source_rectangles(batch_size);
    std::vector<NvBufSurfTransformRect> destination_rectangles(batch_size);
    const guint input_width = GST_VIDEO_INFO_WIDTH(&input_info_);
    const guint input_height = GST_VIDEO_INFO_HEIGHT(&input_info_);
    const guint output_width = GST_VIDEO_INFO_WIDTH(&output_info_);
    const guint output_height = GST_VIDEO_INFO_HEIGHT(&output_info_);

    const guint source_left = std::min(source_crop.left, input_width - 1);
    const guint source_top = std::min(source_crop.top, input_height - 1);
    guint source_width = source_crop.enabled() ? std::min(source_crop.width, input_width - source_left)
                                               : input_surface->surfaceList[0].planeParams.width[0];
    guint source_height = source_crop.enabled() ? std::min(source_crop.height, input_height - source_top)
                                                : input_surface->surfaceList[0].planeParams.height[0];
    const guint destination_left = std::min(destination_crop.left, output_width - 1);
    const guint destination_top = std::min(destination_crop.top, output_height - 1);
    guint destination_width = destination_crop.enabled()
        ? std::min(destination_crop.width, output_width - destination_left)
        : output_surface->surfaceList[0].planeParams.width[0];
    guint destination_height = destination_crop.enabled()
        ? std::min(destination_crop.height, output_height - destination_top)
        : output_surface->surfaceList[0].planeParams.height[0];

    if (!options_.allow_odd_crop) {
      source_width &= ~1U;
      source_height &= ~1U;
      destination_width &= ~1U;
      destination_height &= ~1U;
    }
    if (source_width == 0 || source_height == 0 || destination_width == 0 || destination_height == 0) {
      set_error(error_message, "crop rectangle resolves to zero area");
      success = false;
    } else {
      for (guint index = 0; index < batch_size; ++index) {
        source_rectangles[index].top = source_top;
        source_rectangles[index].left = source_left;
        source_rectangles[index].width = source_width;
        source_rectangles[index].height = source_height;
        destination_rectangles[index].top = destination_top;
        destination_rectangles[index].left = destination_left;
        destination_rectangles[index].width = destination_width;
        destination_rectangles[index].height = destination_height;
      }

      NvBufSurfTransformParams transform_params{};
      transform_params.transform_flag =
          NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST | NVBUFSURF_TRANSFORM_FILTER;
      if (options_.allow_odd_crop) {
        transform_params.transform_flag |= NVBUFSURF_TRANSFORM_ALLOW_ODD_CROP;
      }
      if (options_.flip_method != 0) {
        transform_params.transform_flag |= NVBUFSURF_TRANSFORM_FLIP;
      }
      transform_params.transform_flip = flip_method(options_.flip_method);
      transform_params.transform_filter = static_cast<NvBufSurfTransform_Inter>(options_.interpolation_method);
      transform_params.src_rect = source_rectangles.data();
      transform_params.dst_rect = destination_rectangles.data();

      NvBufSurfTransformConfigParams session{};
      session.compute_mode = static_cast<NvBufSurfTransform_Compute>(options_.compute_hw);
      session.gpu_id = static_cast<int32_t>(options_.gpu_id);
      session.cuda_stream = stream_;
      if (NvBufSurfTransformSetSessionParams(&session) != NvBufSurfTransformError_Success) {
        set_error(error_message, "NvBufSurfTransformSetSessionParams failed");
        success = false;
      } else {
        const NvBufSurfTransform_Error result = NvBufSurfTransform(input_surface, output_surface, &transform_params);
        if (result != NvBufSurfTransformError_Success) {
          set_error(error_message, "NvBufSurfTransform failed with error %d", static_cast<int>(result));
          success = false;
        }
      }
    }
  }

  if (success && !output_nvmm_) {
    NvBufSurface* copy_surface = output_copy_staging_ != nullptr ? output_copy_staging_ : output_surface;
    if (output_copy_staging_ != nullptr) {
      success = copy_staging_surface(output_surface, output_copy_staging_, batch_size, error_message);
    }
    if (success) {
      success = download_raw(copy_surface, output, batch_size, error_message);
    }
  }

  if (output_mapped) {
    gst_buffer_unmap(output, &output_map);
  }
  if (input_mapped) {
    gst_buffer_unmap(input, &input_map);
  }
  return success;
}
