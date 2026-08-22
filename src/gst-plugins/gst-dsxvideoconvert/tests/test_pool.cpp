/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstdio>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace {

GstFlowReturn count_sample(GstElement*, gpointer user_data) {
  auto* count = static_cast<std::atomic<guint>*>(user_data);
  count->fetch_add(1, std::memory_order_relaxed);
  return GST_FLOW_OK;
}

bool wait_for_count(const std::atomic<guint>& count, guint minimum) {
  const gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time() < deadline) {
    if (count.load(std::memory_order_relaxed) >= minimum) {
      return true;
    }
    g_usleep(1000);
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s ELEMENT\n", argv[0]);
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("pool-depth");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* converter = gst_element_factory_make(argv[1], "converter");
  GstElement* filter = gst_element_factory_make("capsfilter", "filter");
  GstElement* sink = gst_element_factory_make("appsink", "sink");
  if (pipeline == nullptr || source == nullptr || converter == nullptr || filter == nullptr || sink == nullptr) {
    std::fprintf(stderr, "failed to construct output-pool test pipeline\n");
    return 2;
  }

  GstCaps* input_caps = gst_caps_from_string("video/x-raw,format=RGBA,width=64,height=48,framerate=30/1");
  GstCaps* output_caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=NV12,width=32,height=24,"
      "framerate=30/1");
  g_object_set(source, "caps", input_caps, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(converter, "output-buffers", 2U, nullptr);
  g_object_set(filter, "caps", output_caps, nullptr);
  std::atomic<guint> sample_count{0};
  g_object_set(sink, "sync", FALSE, "max-buffers", 0U, "emit-signals", TRUE, "enable-last-sample", FALSE, nullptr);
  g_signal_connect(sink, "new-sample", G_CALLBACK(count_sample), &sample_count);
  gst_caps_unref(input_caps);
  gst_caps_unref(output_caps);

  gst_bin_add_many(GST_BIN(pipeline), source, converter, filter, sink, nullptr);
  if (!gst_element_link_many(source, converter, filter, sink, nullptr) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start output-pool test pipeline\n");
    gst_object_unref(pipeline);
    return 2;
  }

  constexpr gsize input_size = 64U * 48U * 4U;
  for (guint index = 0; index < 3; ++index) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, input_size, nullptr);
    gst_buffer_memset(buffer, 0, static_cast<guint8>(index * 71), input_size);
    GST_BUFFER_PTS(buffer) = index * GST_SECOND / 30;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK) {
      std::fprintf(stderr, "failed to push output-pool test buffer\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 2;
    }
  }
  if (gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
    std::fprintf(stderr, "failed to end output-pool test stream\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }

  if (!wait_for_count(sample_count, 2)) {
    std::fprintf(stderr, "two output buffers were not produced\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }
  g_usleep(200 * 1000);
  if (sample_count.load(std::memory_order_relaxed) != 2) {
    std::fprintf(stderr, "output pool exceeded its configured depth\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }

  for (guint index = 0; index < 2; ++index) {
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND);
    if (sample == nullptr) {
      std::fprintf(stderr, "failed to release an output pool buffer\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 2;
    }
    gst_sample_unref(sample);
  }
  if (!wait_for_count(sample_count, 3)) {
    std::fprintf(stderr, "pool did not resume after a buffer was released\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }

  std::printf("depth=2,resumed=1\n");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
