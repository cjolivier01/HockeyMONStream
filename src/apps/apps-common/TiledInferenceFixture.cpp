// Offline qualification: upload one still once, then exercise the production
// nvinfer boundary repeatedly. No video readback; only detections return.
#include "hstream/src/apps/apps-common/TiledInference.h"

#include <cuda_runtime_api.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gstnvdsinfer.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Owner {
  GstElement* pipeline{};
  NvBufSurface* surface{};
  ~Owner() {
    if (pipeline) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
    if (surface)
      NvBufSurfaceDestroy(surface);
  }
};
struct Batches {
  unsigned calls{}, size{};
};
void Raw(GstBuffer*, NvDsInferNetworkInfo*, NvDsInferLayerInfo*, guint, guint batch, gpointer data) {
  auto& result = *static_cast<Batches*>(data);
  ++result.calls;
  result.size = batch;
}
std::string Error(GstElement* pipeline) {
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (!message)
    return "Timed out waiting for detector output";
  GError* error{};
  gchar* debug{};
  gst_message_parse_error(message, &error, &debug);
  const std::string text = std::string(error->message) + " " + (debug ? debug : "");
  g_error_free(error);
  g_free(debug);
  gst_message_unref(message);
  return text;
}
} // namespace
int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "Usage: tiled_inference_fixture INFER_YAML IMAGE TILED_0_OR_1 OVERLAP MERGE_IOU RUNS\n";
    return 2;
  }
  gst_init(&argc, &argv);
  try {
    const bool tiled = std::stoi(argv[3]) == 1;
    const hm::detection::TileInferenceSettings settings{std::stod(argv[4]), std::stof(argv[5])};
    const int runs = std::stoi(argv[6]);
    Check(runs > 0, "Positive run count required");
    auto bgr = cv::imread(argv[2]);
    Check(!bgr.empty(), "Cannot read fixture image");
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    Owner owner;
    NvBufSurfaceCreateParams allocation{};
    allocation.width = rgba.cols;
    allocation.height = rgba.rows;
    allocation.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    allocation.layout = NVBUF_LAYOUT_PITCH;
    allocation.memType = NVBUF_MEM_CUDA_DEVICE;
    Check(NvBufSurfaceCreate(&owner.surface, 1, &allocation) == 0, "GPU surface allocation failed");
    owner.surface->numFilled = 1;
    auto& pixels = owner.surface->surfaceList[0];
    Check(
        cudaMemcpy2D(
            pixels.dataPtr, pixels.pitch, rgba.data, rgba.step, rgba.cols * 4, rgba.rows, cudaMemcpyHostToDevice) ==
            cudaSuccess,
        "Fixture upload failed");
    owner.pipeline = gst_pipeline_new(nullptr);
    auto* source = gst_element_factory_make("appsrc", "source");
    auto* infer = gst_element_factory_make("nvinfer", "primary_gie");
    auto* sink = gst_element_factory_make("appsink", "sink");
    Check(source && infer && sink, "Missing inference fixture elements");
    gst_bin_add_many(GST_BIN(owner.pipeline), source, infer, sink, nullptr);
    auto* caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "RGBA",
        "width",
        G_TYPE_INT,
        rgba.cols,
        "height",
        G_TYPE_INT,
        rgba.rows,
        "framerate",
        GST_TYPE_FRACTION,
        30,
        1,
        nullptr);
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
    g_object_set(source, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
    gst_caps_unref(caps);
    g_object_set(infer, "config-file-path", argv[1], "batch-size", 2u, "process-mode", 1, nullptr);
    Batches batches;
    g_object_set(infer, "raw-output-generated-callback", Raw, "raw-output-generated-userdata", &batches, nullptr);
    if (tiled) {
      const auto status = hm::pipeline::InstallTiledInference(infer, argv[1], settings);
      Check(status.ok(), status.ToString());
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    Check(gst_element_link_many(source, infer, sink, nullptr), "Cannot link inference fixture");
    if (gst_element_set_state(owner.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      throw std::runtime_error(Error(owner.pipeline));
    for (int index = 0; index < runs; ++index) {
      auto* buffer = gst_buffer_new_wrapped_full(
          GST_MEMORY_FLAG_READONLY, owner.surface, sizeof(NvBufSurface), 0, sizeof(NvBufSurface), nullptr, nullptr);
      GST_BUFFER_PTS(buffer) = index * GST_SECOND / 30;
      auto* batch = nvds_create_batch_meta(1);
      batch->max_frames_in_batch = 1;
      auto* frame = nvds_acquire_frame_meta_from_pool(batch);
      frame->frame_num = index;
      frame->buf_pts = GST_BUFFER_PTS(buffer);
      frame->source_frame_width = rgba.cols;
      frame->source_frame_height = rgba.rows;
      nvds_add_frame_meta_to_batch(batch, frame);
      auto* meta =
          gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
      meta->meta_type = NVDS_BATCH_GST_META;
      const auto start = std::chrono::steady_clock::now();
      const unsigned before = batches.calls;
      if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
        throw std::runtime_error(Error(owner.pipeline));
      auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 60 * GST_SECOND);
      if (!sample)
        throw std::runtime_error(Error(owner.pipeline));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      auto* output = gst_buffer_get_nvds_batch_meta(gst_sample_get_buffer(sample));
      Check(output && output->num_frames_in_batch == 1, "Lost original frame");
      auto* result = static_cast<NvDsFrameMeta*>(output->frame_meta_list->data);
      Check(
          result->frame_num == index && result->buf_pts == index * GST_SECOND / 30 && result->bInferDone,
          "Lost frame completion/identity");
      Check(batches.calls == before + 1 && batches.size == (tiled ? 2 : 1), "Expected exactly one inference batch");
      for (auto* item = result->obj_meta_list; item; item = item->next) {
        const auto* object = static_cast<NvDsObjectMeta*>(item->data);
        const auto& r = object->rect_params;
        Check(!object->parent && object->unique_component_id == 1, "ROI metadata escaped inference");
        Check(
            r.left >= 0 && r.top >= 0 && r.left + r.width <= rgba.cols + 1 && r.top + r.height <= rgba.rows + 1,
            "Detection outside stitched coordinates");
      }
      std::cout << "frame=" << index << " batch=" << batches.size << " detections=" << result->num_obj_meta
                << " ms=" << ms << '\n'
                << std::flush;
      gst_sample_unref(sample);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
