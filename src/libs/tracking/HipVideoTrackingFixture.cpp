// Hardware integration qualifier: HEVC -> VA/P010 -> HIP detector -> native
// tracker. Outputs metadata only; no decoded video pixels enter host memory.
#include "hstream/src/libs/detection/HipDetector.h"
#include "hstream/src/libs/detection/VaDetectorImage.h"
#include "hstream/src/libs/tracking/NativeTracker.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video-info-dma.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "yaml-cpp/yaml.h"

namespace {
void Check(bool condition, const std::string& error) {
  if (!condition)
    throw std::runtime_error(error);
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
struct Resources {
  hipStream_t stream{};
  GstElement* pipeline{};
  GstElement* sink{};
  ~Resources() {
    if (stream)
      (void)hipStreamSynchronize(stream);
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink)
      gst_object_unref(sink);
    if (pipeline)
      gst_object_unref(pipeline);
    if (stream)
      (void)hipStreamDestroy(stream);
  }
};
struct Sample {
  GstSample* value;
  ~Sample() {
    if (value)
      gst_sample_unref(value);
  }
};
} // namespace
int main(int argc, char** argv) {
  if (argc != 10) {
    std::cerr
        << "Usage: hip_video_tracking_fixture MODEL VIDEO INFER_YAML WIDTH HEIGHT IO_BYTES START_SECONDS FRAMES OUTPUT_CSV\n";
    return 2;
  }
  gst_init(&argc, &argv);
  try {
    const auto yaml = YAML::LoadFile(argv[3]);
    const auto property = yaml["property"], attrs = yaml["class-attrs-all"];
    Check(property["model-color-format"].as<int>() == 0, "Fixture requires RGB model");
    hm::detection::DetectorConfig config{
        argv[1],
        "input",
        std::stoi(argv[4]),
        std::stoi(argv[5]),
        property["num-detected-classes"].as<int>(),
        property["gpu-id"].as<int>(),
        attrs["topk"].as<int>(),
        property["net-scale-factor"].as<float>(),
        attrs["pre-cluster-threshold"].as<float>(),
        attrs["nms-iou-threshold"].as<float>(),
        property["maintain-aspect-ratio"].as<int>() != 0,
        property["symmetric-padding"].as<int>() != 0,
        hm::gpu::MigraphxSession::Precision::kFloat16,
        std::stoull(argv[6])};
    const int count = std::stoi(argv[8]);
    const double start = std::stod(argv[7]);
    Check(
        count > 0 && std::isfinite(start) && start >= 0 && start < static_cast<double>(G_MAXINT64 / GST_SECOND),
        "Invalid fixture interval");
    Hip(hipSetDevice(config.device));
    Resources resources;
    Hip(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking));
    auto detector = hm::detection::HipDetector::Create(config);
    Check(detector.ok(), detector.status().ToString());
    // Explicit qualification parameters, not application defaults.
    auto tracker = hm::tracking::NativeTracker::Create({.25, .6, .7, .8, .5, .7, true, 30, 2, 512, 8 * 1024 * 1024});
    Check(tracker.ok(), tracker.status().ToString());
    GError* error = nullptr;
    resources.pipeline = gst_parse_launch(
        "filesrc name=input ! qtdemux ! h265parse ! vah265dec ! vapostproc disable-passthrough=true ! "
        "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=P010 ! appsink name=result sync=false max-buffers=2",
        &error);
    if (error) {
      std::string message(error->message);
      g_error_free(error);
      throw std::runtime_error(message);
    }
    Check(resources.pipeline, "Could not create hardware decode pipeline");
    auto* input = gst_bin_get_by_name(GST_BIN(resources.pipeline), "input");
    g_object_set(input, "location", argv[2], nullptr);
    gst_object_unref(input);
    resources.sink = gst_bin_get_by_name(GST_BIN(resources.pipeline), "result");
    GstAppSinkCallbacks callbacks{};
    callbacks.propose_allocation = [](GstAppSink*, GstQuery* query, gpointer) -> gboolean {
      gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
      return TRUE;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(resources.sink), &callbacks, nullptr, nullptr);
    Check(
        gst_element_set_state(resources.pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE,
        "Could not pause pipeline");
    Check(
        gst_element_get_state(resources.pipeline, nullptr, nullptr, 30 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS,
        "Decoder preroll failed");
    Check(
        gst_element_seek_simple(
            resources.pipeline,
            GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            static_cast<gint64>(start * GST_SECOND)),
        "Could not seek fixture");
    Check(
        gst_element_set_state(resources.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
        "Could not start pipeline");
    std::ofstream output(argv[9]);
    Check(static_cast<bool>(output), "Cannot write metadata");
    output << "frame,pts_ns,detections,track_id,left,top,right,bottom,confidence\n";
    GstClockTime previous = GST_CLOCK_TIME_NONE;
    size_t observations = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (int sequence = 0; sequence < count; ++sequence) {
      Sample sample{gst_app_sink_try_pull_sample(GST_APP_SINK(resources.sink), 30 * GST_SECOND)};
      if (!sample.value) {
        auto* bus = gst_element_get_bus(resources.pipeline);
        auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        std::string detail = "Timed out or reached EOS before requested frame count";
        if (message) {
          gst_message_parse_error(message, &error, nullptr);
          detail = error->message;
          g_error_free(error);
          gst_message_unref(message);
        }
        throw std::runtime_error(detail);
      }
      auto* buffer = gst_sample_get_buffer(sample.value);
      const auto pts = GST_BUFFER_PTS(buffer);
      Check(
          GST_CLOCK_TIME_IS_VALID(pts) && (!GST_CLOCK_TIME_IS_VALID(previous) || pts > previous),
          "Invalid video timestamps");
      previous = pts;
      GstVideoInfoDmaDrm info{};
      Check(gst_video_info_dma_drm_from_caps(&info, gst_sample_get_caps(sample.value)), "Missing DMA-BUF caps");
      auto imported = hm::gpu::VaHipFrame::Import(buffer, resources.stream, hm::gpu::VaHipFrame::Access::kRead);
      Check(imported.ok(), imported.status().ToString());
      const auto image = hm::detection::DescribeVaDetectorImage(**imported, info.vinfo);
      Check(image.ok(), image.status().ToString());
      const auto detections = (*detector)->Detect(*image, resources.stream);
      Check(detections.ok(), detections.status().ToString());
      const auto tracks = (*tracker)->Update(sequence, *detections);
      Check(tracks.ok(), tracks.status().ToString());
      for (const auto& track : *tracks) {
        const auto& p = track.box;
        output << sequence << ',' << pts << ',' << detections->size() << ',' << track.id << ',' << p.left << ','
               << p.top << ',' << p.right << ',' << p.bottom << ',' << p.confidence << '\n';
      }
      observations += tracks->size();
      Check((*imported)->Complete().ok(), "Could not complete imported surface");
      std::cout << "Frame " << sequence << ": " << detections->size() << " detections, " << tracks->size()
                << " tracks\n"
                << std::flush;
    }
    Check(observations > 0, "Fixture produced no confirmed tracks");
    Check(static_cast<bool>(output), "Could not write track metadata");
    std::cout << count << " frames, " << observations << " observed tracks, "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() << " seconds\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
