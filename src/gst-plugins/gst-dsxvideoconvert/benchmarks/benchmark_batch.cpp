/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda_runtime_api.h>
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

void count_output(GstElement*, GstBuffer*, GstPad*, gpointer user_data) {
  auto* count = static_cast<std::atomic<guint>*>(user_data);
  count->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s ELEMENT FRAMES\n", argv[0]);
    return 2;
  }
  const guint frames = static_cast<guint>(std::strtoul(argv[2], nullptr, 10));
  if (frames == 0) {
    std::fprintf(stderr, "FRAMES must be positive\n");
    return 2;
  }
  const bool identity = std::strcmp(argv[1], "identity") == 0;

  NvBufSurfaceCreateParams create{};
  create.gpuId = 0;
  create.width = 1920;
  create.height = 1080;
  create.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create.layout = NVBUF_LAYOUT_PITCH;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  create.memType = NVBUF_MEM_SURFACE_ARRAY;
#else
  create.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
  NvBufSurface* input_surface = nullptr;
  if (NvBufSurfaceCreate(&input_surface, 2, &create) != 0) {
    std::fprintf(stderr, "failed to allocate benchmark input batch\n");
    return 2;
  }
  input_surface->numFilled = 2;
  if (NvBufSurfaceMemSet(input_surface, 0, -1, 17) != 0 || NvBufSurfaceMemSet(input_surface, 1, -1, 203) != 0) {
    std::fprintf(stderr, "failed to initialize benchmark input batch\n");
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("batch-benchmark");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* candidate = gst_element_factory_make(argv[1], "candidate");
  GstElement* filter = gst_element_factory_make("capsfilter", "filter");
  GstElement* sink = gst_element_factory_make("fakesink", "sink");
  if (pipeline == nullptr || source == nullptr || candidate == nullptr || filter == nullptr || sink == nullptr) {
    std::fprintf(stderr, "failed to construct batch benchmark pipeline\n");
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstCaps* input_caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=RGBA,width=1920,height=1080,"
      "framerate=30/1,batch-size=(int)2");
  GstCaps* output_caps = gst_caps_from_string(
      identity ? "video/x-raw(memory:NVMM),format=RGBA,width=1920,height=1080,"
                 "framerate=30/1,batch-size=(int)2"
               : "video/x-raw(memory:NVMM),format=NV12,width=1280,height=720,"
                 "framerate=30/1,batch-size=(int)2");
  g_object_set(source, "caps", input_caps, "format", GST_FORMAT_TIME, "block", TRUE, nullptr);
  if (!identity) {
    g_object_set(candidate, "interpolation-method", 1, nullptr);
  }
  g_object_set(filter, "caps", output_caps, nullptr);
  std::atomic<guint> output_count{0};
  g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(sink, "handoff", G_CALLBACK(count_output), &output_count);
  gst_caps_unref(input_caps);
  gst_caps_unref(output_caps);

  gst_bin_add_many(GST_BIN(pipeline), source, candidate, filter, sink, nullptr);
  GstPad* source_pad = gst_element_get_static_pad(source, "src");
  gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, answer_batch_query, nullptr, nullptr);
  gst_object_unref(source_pad);
  if (!gst_element_link_many(source, candidate, filter, sink, nullptr) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start batch benchmark pipeline\n");
    gst_object_unref(pipeline);
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  const gint64 start = g_get_monotonic_time();
  for (guint index = 0; index < frames; ++index) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, sizeof(NvBufSurface), nullptr);
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      std::fprintf(stderr, "failed to map benchmark descriptor\n");
      gst_buffer_unref(buffer);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      NvBufSurfaceDestroy(input_surface);
      return 2;
    }
    std::memcpy(map.data, input_surface, sizeof(NvBufSurface));
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = index * GST_SECOND / 30;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK) {
      std::fprintf(stderr, "failed to push benchmark input\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      NvBufSurfaceDestroy(input_surface);
      return 2;
    }
  }
  if (gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
    std::fprintf(stderr, "failed to end benchmark stream\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    NvBufSurfaceDestroy(input_surface);
    return 2;
  }

  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message = gst_bus_timed_pop_filtered(
      bus, 120 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const gint64 stop = g_get_monotonic_time();
  bool success = message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS &&
      output_count.load(std::memory_order_relaxed) == frames;
  if (message != nullptr) {
    gst_message_unref(message);
  }
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  NvBufSurfaceDestroy(input_surface);
  if (!success) {
    std::fprintf(stderr, "batch benchmark failed or timed out\n");
    return 2;
  }
  std::printf("%.6f\n", static_cast<double>(stop - start) / G_USEC_PER_SEC);
  return 0;
}
