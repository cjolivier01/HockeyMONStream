/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

namespace {

bool link_many(GstElement* first, GstElement* second, GstElement* third, GstElement* fourth) {
  return gst_element_link(first, second) && gst_element_link(second, third) && gst_element_link(third, fourth);
}

void attach_metadata(GstBuffer* buffer) {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  g_assert_nonnull(batch);
  NvDsMeta* meta =
      gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  g_assert_nonnull(meta);
  meta->meta_type = NVDS_BATCH_GST_META;
  batch->base_meta.batch_meta = batch;
  batch->base_meta.copy_func = nvds_batch_meta_copy_func;
  batch->base_meta.release_func = nvds_batch_meta_release_func;
  batch->max_frames_in_batch = 1;

  NvDsFrameMeta* frame = nvds_acquire_frame_meta_from_pool(batch);
  g_assert_nonnull(frame);
  nvds_add_frame_meta_to_batch(batch, frame);
  frame->source_frame_width = 320;
  frame->source_frame_height = 240;
  frame->pipeline_width = 320;
  frame->pipeline_height = 240;

  NvDsObjectMeta* object = nvds_acquire_obj_meta_from_pool(batch);
  g_assert_nonnull(object);
  object->rect_params.left = 31.0F;
  object->rect_params.top = 47.0F;
  object->rect_params.width = 53.0F;
  object->rect_params.height = 29.0F;
  object->rect_params.border_width = 4;
  object->text_params.x_offset = 38;
  object->text_params.y_offset = 52;
  object->text_params.font_params.font_size = 16;
  nvds_add_obj_meta_to_frame(frame, object, nullptr);

  NvDsObjectMeta* outside_object = nvds_acquire_obj_meta_from_pool(batch);
  g_assert_nonnull(outside_object);
  outside_object->rect_params.left = 0.0F;
  outside_object->rect_params.top = 0.0F;
  outside_object->rect_params.width = 10.0F;
  outside_object->rect_params.height = 10.0F;
  outside_object->rect_params.border_width = 2;
  outside_object->text_params.x_offset = 2;
  outside_object->text_params.y_offset = 3;
  outside_object->text_params.font_params.font_size = 10;
  nvds_add_obj_meta_to_frame(frame, outside_object, nullptr);

  NvDsDisplayMeta* display = nvds_acquire_display_meta_from_pool(batch);
  g_assert_nonnull(display);
  display->num_rects = 1;
  display->rect_params[0].left = 60.0F;
  display->rect_params[0].top = 50.0F;
  display->rect_params[0].width = 20.0F;
  display->rect_params[0].height = 10.0F;
  display->num_lines = 1;
  display->line_params[0].x1 = 20;
  display->line_params[0].y1 = 10;
  display->line_params[0].x2 = 220;
  display->line_params[0].y2 = 110;
  display->line_params[0].line_width = 6;
  display->num_labels = 1;
  display->text_params[0].x_offset = 80;
  display->text_params[0].y_offset = 40;
  display->text_params[0].font_params.font_size = 10;
  display->num_arrows = 1;
  display->arrow_params[0].x1 = 30;
  display->arrow_params[0].y1 = 20;
  display->arrow_params[0].x2 = 130;
  display->arrow_params[0].y2 = 70;
  display->arrow_params[0].arrow_width = 8;
  display->num_circles = 1;
  display->circle_params[0].xc = 100;
  display->circle_params[0].yc = 60;
  display->circle_params[0].radius = 12;
  display->circle_params[0].circle_width = 4;
  nvds_add_display_meta_to_frame(frame, display);
}

void print_metadata(GstBuffer* buffer) {
  NvDsBatchMeta* batch = gst_buffer_get_nvds_batch_meta(buffer);
  g_assert_nonnull(batch);
  g_assert_nonnull(batch->frame_meta_list);
  auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
  g_assert_nonnull(frame->obj_meta_list);
  auto* object = static_cast<NvDsObjectMeta*>(frame->obj_meta_list->data);
  g_assert_nonnull(frame->display_meta_list);
  auto* display = static_cast<NvDsDisplayMeta*>(frame->display_meta_list->data);

  std::printf(
      "frame=%u,%u,%u,%u\n",
      frame->source_frame_width,
      frame->source_frame_height,
      frame->pipeline_width,
      frame->pipeline_height);
  std::printf(
      "object=%.3f,%.3f,%.3f,%.3f,%u\n",
      object->rect_params.left,
      object->rect_params.top,
      object->rect_params.width,
      object->rect_params.height,
      object->rect_params.border_width);
  std::printf(
      "text=%u,%u,%u\n",
      object->text_params.x_offset,
      object->text_params.y_offset,
      object->text_params.font_params.font_size);
  g_assert_nonnull(frame->obj_meta_list->next);
  auto* outside_object = static_cast<NvDsObjectMeta*>(frame->obj_meta_list->next->data);
  std::printf(
      "outside-object=%.3f,%.3f,%.3f,%.3f,%u\n",
      outside_object->rect_params.left,
      outside_object->rect_params.top,
      outside_object->rect_params.width,
      outside_object->rect_params.height,
      outside_object->rect_params.border_width);
  std::printf(
      "outside-text=%u,%u,%u\n",
      outside_object->text_params.x_offset,
      outside_object->text_params.y_offset,
      outside_object->text_params.font_params.font_size);
  std::printf(
      "display=%.3f,%.3f,%.3f,%.3f\n",
      display->rect_params[0].left,
      display->rect_params[0].top,
      display->rect_params[0].width,
      display->rect_params[0].height);
  std::printf(
      "line=%u,%u,%u,%u,%u\n",
      display->line_params[0].x1,
      display->line_params[0].y1,
      display->line_params[0].x2,
      display->line_params[0].y2,
      display->line_params[0].line_width);
  std::printf(
      "label=%u,%u,%u\n",
      display->text_params[0].x_offset,
      display->text_params[0].y_offset,
      display->text_params[0].font_params.font_size);
  std::printf(
      "arrow=%u,%u,%u,%u,%u\n",
      display->arrow_params[0].x1,
      display->arrow_params[0].y1,
      display->arrow_params[0].x2,
      display->arrow_params[0].y2,
      display->arrow_params[0].arrow_width);
  std::printf(
      "circle=%u,%u,%u,%u\n",
      display->circle_params[0].xc,
      display->circle_params[0].yc,
      display->circle_params[0].radius,
      display->circle_params[0].circle_width);
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s ELEMENT [FLIP-METHOD]\n", argv[0]);
    return 2;
  }
  const gint flip_method = argc == 3 ? std::atoi(argv[2]) : 0;
  if (flip_method < 0 || flip_method > 7) {
    std::fprintf(stderr, "FLIP-METHOD must be between 0 and 7\n");
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("metadata-parity");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* converter = gst_element_factory_make(argv[1], "converter");
  GstElement* filter = gst_element_factory_make("capsfilter", "filter");
  GstElement* sink = gst_element_factory_make("appsink", "sink");
  if (pipeline == nullptr || source == nullptr || converter == nullptr || filter == nullptr || sink == nullptr) {
    std::fprintf(stderr, "failed to construct metadata test pipeline\n");
    return 2;
  }

  GstCaps* input_caps = gst_caps_from_string("video/x-raw,format=RGBA,width=320,height=240,framerate=30/1");
  GstCaps* output_caps = gst_caps_from_string("video/x-raw,format=RGBA,width=160,height=120,framerate=30/1");
  g_object_set(source, "caps", input_caps, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(converter, "src-crop", "20:10:200:100", "dest-crop", "5:7:100:50", "flip-method", flip_method, nullptr);
  g_object_set(filter, "caps", output_caps, nullptr);
  g_object_set(sink, "sync", FALSE, nullptr);
  gst_caps_unref(input_caps);
  gst_caps_unref(output_caps);

  gst_bin_add_many(GST_BIN(pipeline), source, converter, filter, sink, nullptr);
  if (!link_many(source, converter, filter, sink)) {
    std::fprintf(stderr, "failed to link metadata test pipeline\n");
    gst_object_unref(pipeline);
    return 2;
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start metadata test pipeline\n");
    gst_object_unref(pipeline);
    return 2;
  }

  GstVideoInfo info;
  gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 320, 240);
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, info.size, nullptr);
  gst_buffer_memset(buffer, 0, 0, info.size);
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
  attach_metadata(buffer);

  if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK ||
      gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
    std::fprintf(stderr, "failed to submit metadata test buffer\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }
  GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
  if (sample == nullptr) {
    std::fprintf(stderr, "timed out waiting for metadata test output\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 2;
  }
  print_metadata(gst_sample_get_buffer(sample));
  gst_sample_unref(sample);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
