/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace {

using VideoMetaMap = gboolean (*)(GstVideoMeta*, guint, GstMapInfo*, gpointer*, gint*, GstMapFlags);
using VideoMetaUnmap = gboolean (*)(GstVideoMeta*, guint, GstMapInfo*);

VideoMetaMap default_video_meta_map = nullptr;
VideoMetaUnmap default_video_meta_unmap = nullptr;
guint custom_map_calls = 0;
guint custom_unmap_calls = 0;

gboolean counting_video_meta_map(
    GstVideoMeta* meta,
    guint plane,
    GstMapInfo* info,
    gpointer* data,
    gint* stride,
    GstMapFlags flags) {
  ++custom_map_calls;
  return default_video_meta_map(meta, plane, info, data, stride, flags);
}

gboolean counting_video_meta_unmap(GstVideoMeta* meta, guint plane, GstMapInfo* info) {
  ++custom_unmap_calls;
  return default_video_meta_unmap(meta, plane, info);
}

gboolean check_output_video_meta(GstBuffer*, GstMeta** meta, gpointer user_data) {
  if ((*meta)->info->api != GST_VIDEO_META_API_TYPE) {
    return TRUE;
  }
  auto* count = static_cast<guint*>(user_data);
  ++*count;
  auto* video_meta = reinterpret_cast<GstVideoMeta*>(*meta);
  g_assert_cmpint(video_meta->format, ==, GST_VIDEO_FORMAT_NV12);
  g_assert_cmpuint(video_meta->width, ==, 160);
  g_assert_cmpuint(video_meta->height, ==, 120);
  g_assert_cmpuint(video_meta->n_planes, ==, 2);
  return TRUE;
}

std::uint64_t checksum(const guint8* data, gsize size) {
  std::uint64_t hash = UINT64_C(1469598103934665603);
  for (gsize index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 3 ||
      (std::strcmp(argv[2], "compact") != 0 && std::strcmp(argv[2], "padded") != 0 &&
       std::strcmp(argv[2], "custom") != 0 && std::strcmp(argv[2], "undersized-positive") != 0 &&
       std::strcmp(argv[2], "undersized-negative") != 0)) {
    std::fprintf(
        stderr,
        "usage: %s ELEMENT compact|padded|custom|undersized-positive|"
        "undersized-negative\n",
        argv[0]);
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("stride-parity");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* converter = gst_element_factory_make(argv[1], "converter");
  GstElement* filter = gst_element_factory_make("capsfilter", "filter");
  GstElement* sink = gst_element_factory_make("appsink", "sink");
  if (pipeline == nullptr || source == nullptr || converter == nullptr || filter == nullptr || sink == nullptr) {
    std::fprintf(stderr, "failed to construct stride test pipeline\n");
    return 2;
  }

  GstCaps* input_caps = gst_caps_from_string("video/x-raw,format=RGBA,width=320,height=240,framerate=30/1");
  GstCaps* output_caps = gst_caps_from_string("video/x-raw,format=NV12,width=160,height=120,framerate=30/1");
  g_object_set(source, "caps", input_caps, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(filter, "caps", output_caps, nullptr);
  g_object_set(sink, "sync", FALSE, nullptr);
  gst_caps_unref(input_caps);
  gst_caps_unref(output_caps);
  gst_bin_add_many(GST_BIN(pipeline), source, converter, filter, sink, nullptr);
  if (!gst_element_link_many(source, converter, filter, sink, nullptr) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start stride test pipeline\n");
    gst_object_unref(pipeline);
    return 2;
  }

  const bool padded = std::strcmp(argv[2], "padded") == 0;
  const bool custom = std::strcmp(argv[2], "custom") == 0;
  const bool undersized_positive = std::strcmp(argv[2], "undersized-positive") == 0;
  const bool undersized_negative = std::strcmp(argv[2], "undersized-negative") == 0;
  const bool expect_rejection = undersized_positive || undersized_negative;
  const gint stride = padded ? 320 * 4 + 64 : (expect_rejection ? (undersized_negative ? -64 : 64) : 320 * 4);
  const gsize offset = padded ? 37U : (undersized_negative ? 239U * 64U : 0U);
  const gsize size = padded ? offset + static_cast<gsize>(stride) * 240 + 19 : 320U * 240U * 4U;
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
  GstMapInfo map = GST_MAP_INFO_INIT;
  g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
  std::memset(map.data, 0x5a, size);
  for (guint y = 0; y < 240; ++y) {
    guint8* row = map.data + static_cast<gint64>(offset) + static_cast<gint64>(y) * stride;
    for (guint x = 0; x < 320; ++x) {
      row[x * 4 + 0] = static_cast<guint8>((x * 3 + y) & 0xff);
      row[x * 4 + 1] = static_cast<guint8>((x + y * 5) & 0xff);
      row[x * 4 + 2] = static_cast<guint8>((x * 7 + y * 11) & 0xff);
      row[x * 4 + 3] = 255;
    }
  }
  gst_buffer_unmap(buffer, &map);
  gsize offsets[GST_VIDEO_MAX_PLANES] = {offset, 0, 0, 0};
  gint strides[GST_VIDEO_MAX_PLANES] = {stride, 0, 0, 0};
  GstVideoMeta* video_meta = gst_buffer_add_video_meta_full(
      buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA, 320, 240, 1, offsets, strides);
  g_assert_nonnull(video_meta);
  if (custom) {
    default_video_meta_map = video_meta->map;
    default_video_meta_unmap = video_meta->unmap;
    video_meta->map = counting_video_meta_map;
    video_meta->unmap = counting_video_meta_unmap;
  }
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
  g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), buffer), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(source)), ==, GST_FLOW_OK);

  GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
  if (expect_rejection) {
    if (sample != nullptr) {
      gst_sample_unref(sample);
      std::fprintf(stderr, "undersized stride unexpectedly produced output\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 2;
    }
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    if (message == nullptr) {
      std::fprintf(stderr, "undersized stride did not report an error\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 2;
    }
    gst_message_unref(message);
    std::printf("rejected\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
  }
  if (sample == nullptr) {
    std::fprintf(stderr, "timed out waiting for stride test output\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }
  guint output_video_meta_count = 0;
  gst_buffer_foreach_meta(gst_sample_get_buffer(sample), check_output_video_meta, &output_video_meta_count);
  g_assert_cmpuint(output_video_meta_count, <=, 1);
  if (custom) {
    g_assert_cmpuint(custom_map_calls, >, 0);
    g_assert_cmpuint(custom_unmap_calls, ==, custom_map_calls);
  }
  GstMapInfo output_map = GST_MAP_INFO_INIT;
  g_assert_true(gst_buffer_map(gst_sample_get_buffer(sample), &output_map, GST_MAP_READ));
  std::printf("size=%zu,checksum=%016" PRIx64 "\n", output_map.size, checksum(output_map.data, output_map.size));
  gst_buffer_unmap(gst_sample_get_buffer(sample), &output_map);
  gst_sample_unref(sample);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
