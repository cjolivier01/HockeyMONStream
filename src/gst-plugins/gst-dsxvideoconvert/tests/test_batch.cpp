/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <cuda_runtime_api.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "gst-nvquery.h"
#include "nvbufsurface.h"

namespace {

GstPadProbeReturn answer_batch_query(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
  if (query != nullptr && gst_nvquery_is_batch_size(query)) {
    gst_nvquery_batch_size_set(query, 2);
    return GST_PAD_PROBE_HANDLED;
  }
  return GST_PAD_PROBE_OK;
}

std::uint64_t hash_bytes(std::uint64_t hash, const guint8* data, gsize size) {
  for (gsize index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

std::uint64_t surface_checksum(NvBufSurface* batch, guint index) {
  NvBufSurfaceParams& surface = batch->surfaceList[index];
  const bool mapped = batch->memType == NVBUF_MEM_SURFACE_ARRAY || batch->memType == NVBUF_MEM_HANDLE;
  if (mapped) {
    g_assert_cmpint(NvBufSurfaceMap(batch, static_cast<int>(index), -1, NVBUF_MAP_READ), ==, 0);
    g_assert_cmpint(NvBufSurfaceSyncForCpu(batch, static_cast<int>(index), -1), ==, 0);
  }
  std::uint64_t hash = UINT64_C(1469598103934665603);
  for (guint plane = 0; plane < surface.planeParams.num_planes; ++plane) {
    const guint row_bytes = surface.planeParams.width[plane] * surface.planeParams.bytesPerPix[plane];
    const guint rows = surface.planeParams.height[plane];
    std::vector<guint8> pixels(static_cast<gsize>(row_bytes) * rows);
    const auto* source = mapped ? static_cast<const guint8*>(surface.mappedAddr.addr[plane])
                                : static_cast<const guint8*>(surface.dataPtr) + surface.planeParams.offset[plane];
    if (mapped) {
      for (guint row = 0; row < rows; ++row) {
        std::memcpy(pixels.data() + row * row_bytes, source + row * surface.planeParams.pitch[plane], row_bytes);
      }
    } else {
      g_assert_cmpint(
          cudaMemcpy2D(
              pixels.data(),
              row_bytes,
              source,
              surface.planeParams.pitch[plane],
              row_bytes,
              rows,
              cudaMemcpyDeviceToHost),
          ==,
          cudaSuccess);
    }
    hash = hash_bytes(hash, pixels.data(), pixels.size());
  }
  if (mapped) {
    g_assert_cmpint(NvBufSurfaceUnMap(batch, static_cast<int>(index), -1), ==, 0);
  }
  return hash;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const char* option = argc == 4 ? argv[3] : nullptr;
  if ((argc != 3 && argc != 4) || (std::strcmp(argv[2], "nvmm") != 0 && std::strcmp(argv[2], "raw") != 0) ||
      (option != nullptr && std::strcmp(option, "caps-only") != 0 && std::strcmp(option, "contiguous") != 0 &&
       std::strcmp(option, "block-linear") != 0 && std::strcmp(option, "compute-gpu") != 0 &&
       std::strcmp(option, "copy-gpu") != 0)) {
    std::fprintf(
        stderr,
        "usage: %s ELEMENT nvmm|raw [caps-only|contiguous|block-linear|"
        "compute-gpu|copy-gpu]\n",
        argv[0]);
    return 2;
  }

  NvBufSurfaceCreateParams create{};
  create.gpuId = 0;
  create.width = 320;
  create.height = 240;
  create.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create.layout = NVBUF_LAYOUT_PITCH;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  create.memType = NVBUF_MEM_SURFACE_ARRAY;
#else
  create.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
  NvBufSurface* input_surface = nullptr;
  if (NvBufSurfaceCreate(&input_surface, 2, &create) != 0) {
    std::fprintf(stderr, "failed to allocate input batch\n");
    return 2;
  }
  input_surface->numFilled = 2;
  if (NvBufSurfaceMemSet(input_surface, 0, -1, 17) != 0 || NvBufSurfaceMemSet(input_surface, 1, -1, 203) != 0) {
    std::fprintf(stderr, "failed to initialize input batch\n");
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("batch-parity");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* converter = gst_element_factory_make(argv[1], "converter");
  GstElement* filter = gst_element_factory_make("capsfilter", "filter");
  GstElement* sink = gst_element_factory_make("appsink", "sink");
  if (pipeline == nullptr || source == nullptr || converter == nullptr || filter == nullptr || sink == nullptr) {
    std::fprintf(stderr, "failed to construct batch test pipeline\n");
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstCaps* input_caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=RGBA,width=320,height=240,"
      "framerate=30/1,batch-size=(int)2");
  const bool output_nvmm = std::strcmp(argv[2], "nvmm") == 0;
  GstCaps* output_caps = gst_caps_from_string(
      output_nvmm ? "video/x-raw(memory:NVMM),format=NV12,width=160,height=120,"
                    "framerate=30/1"
                  : "video/x-raw,format=NV12,width=160,height=120,framerate=30/1");
  g_object_set(source, "caps", input_caps, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(filter, "caps", output_caps, nullptr);
  g_object_set(sink, "sync", FALSE, nullptr);
  if (option != nullptr && std::strcmp(option, "contiguous") == 0) {
    g_object_set(converter, "contiguous-buffers", TRUE, nullptr);
  } else if (option != nullptr && std::strcmp(option, "block-linear") == 0) {
    g_object_set(converter, "bl-output", TRUE, nullptr);
  } else if (option != nullptr && std::strcmp(option, "compute-gpu") == 0) {
    g_object_set(converter, "compute-hw", 1, nullptr);
  } else if (option != nullptr && std::strcmp(option, "copy-gpu") == 0) {
    g_object_set(converter, "copy-hw", 1, nullptr);
  }
  gst_caps_unref(input_caps);
  gst_caps_unref(output_caps);

  gst_bin_add_many(GST_BIN(pipeline), source, converter, filter, sink, nullptr);
  if (option == nullptr || std::strcmp(option, "caps-only") != 0) {
    GstPad* source_pad = gst_element_get_static_pad(source, "src");
    gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, answer_batch_query, nullptr, nullptr);
    gst_object_unref(source_pad);
  }
  if (!gst_element_link_many(source, converter, filter, sink, nullptr) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start batch test pipeline\n");
    gst_object_unref(pipeline);
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, sizeof(NvBufSurface), nullptr);
  GstMapInfo input_map = GST_MAP_INFO_INIT;
  gst_buffer_map(buffer, &input_map, GST_MAP_WRITE);
  std::memcpy(input_map.data, input_surface, sizeof(NvBufSurface));
  gst_buffer_unmap(buffer, &input_map);
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
  if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK ||
      gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
    std::fprintf(stderr, "failed to submit batch test buffer\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
  if (sample == nullptr) {
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    if (!output_nvmm && message != nullptr) {
      gst_message_unref(message);
      std::printf("raw-batch-error\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      NvBufSurfaceDestroy(input_surface);
      return 0;
    }
    if (message != nullptr) {
      gst_message_unref(message);
    }
    std::fprintf(stderr, "timed out waiting for batch output\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }
  GstMapInfo output_map = GST_MAP_INFO_INIT;
  gst_buffer_map(gst_sample_get_buffer(sample), &output_map, GST_MAP_READ);
  if (output_nvmm) {
    auto* output_surface = reinterpret_cast<NvBufSurface*>(output_map.data);
    std::printf(
        "batch=%u,filled=%u,gpu=%u,mem=%d,contiguous=%d\n",
        output_surface->batchSize,
        output_surface->numFilled,
        output_surface->gpuId,
        static_cast<int>(output_surface->memType),
        output_surface->isContiguous ? 1 : 0);
    for (guint index = 0; index < output_surface->numFilled; ++index) {
      const NvBufSurfaceParams& params = output_surface->surfaceList[index];
      std::printf(
          "surface=%u,%u,%u,%d,%d,checksum=%016" PRIx64 "\n",
          index,
          params.width,
          params.height,
          static_cast<int>(params.colorFormat),
          static_cast<int>(params.layout),
          surface_checksum(output_surface, index));
    }
  } else {
    constexpr gsize frame_size = 160U * 120U * 3U / 2U;
    std::printf("raw-size=%zu\n", output_map.size);
    for (guint index = 0; index < 2; ++index) {
      g_assert_cmpuint((index + 1) * frame_size, <=, output_map.size);
      const std::uint64_t checksum =
          hash_bytes(UINT64_C(1469598103934665603), output_map.data + index * frame_size, frame_size);
      std::printf("raw=%u,checksum=%016" PRIx64 "\n", index, checksum);
    }
  }
  gst_buffer_unmap(gst_sample_get_buffer(sample), &output_map);
  gst_sample_unref(sample);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  NvBufSurfaceDestroy(input_surface);
  return 0;
}
