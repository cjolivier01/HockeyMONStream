// Hardware qualification of the composed raw-camera Program path. Video stays
// GPU-resident; only detections/tracks and saved calibration artifacts use CPU
// memory. Bounds/tracker tuning come from the baseline; codec/output choices
// and explicit input/model arguments remain qualification controls.
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video-info-dma.h>

#include "cupano/pano/cudaPano.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/detection/HipDetector.h"
#include "hstream/src/libs/detection/VaDetectorImage.h"
#include "hstream/src/libs/gpu/ConvertImage.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/gpu/vaapi/VaVideoReader.h"
#include "hstream/src/libs/program/AmdProgramSettings.h"
#include "hstream/src/libs/program/EncodedColorimetry.h"
#include "hstream/src/libs/program/VaProgramOutput.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/tracking/ProgramTracking.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "yaml-cpp/yaml.h"

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Check(result.ok(), result.status().ToString());
  return std::move(*result);
}
void Status(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}
using Sample = std::unique_ptr<GstSample, decltype(&gst_sample_unref)>;
using Buffer = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>;
struct Pipeline {
  GstElement* value{};
  GstElement* endpoint{};
  ~Pipeline() {
    if (value)
      gst_element_set_state(value, GST_STATE_NULL);
    if (endpoint)
      gst_object_unref(endpoint);
    if (value)
      gst_object_unref(value);
  }
  void Create(const char* graph, const char* endpoint_name) {
    GError* error{};
    value = gst_parse_launch(graph, &error);
    if (error) {
      const std::string message(error->message);
      g_error_free(error);
      throw std::runtime_error(message);
    }
    Check(value, "Pipeline creation failed");
    endpoint = gst_bin_get_by_name(GST_BIN(value), endpoint_name);
    Check(endpoint, "Missing endpoint");
  }
  std::string Error() {
    auto* bus = gst_element_get_bus(value);
    auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!message)
      return {};
    GError* error{};
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    const std::string text = std::string(error->message) + ": " + (debug ? debug : "");
    g_error_free(error);
    g_free(debug);
    gst_message_unref(message);
    return text;
  }
  void Start() {
    Check(
        gst_element_set_state(value, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
        "Pipeline start failed: " + Error());
  }
};
std::unique_ptr<hm::gpu::VaVideoReader> OpenReader(
    const hm::program::AmdProgramSettings& settings,
    const char* path,
    double start) {
  Check(std::isfinite(start) && start >= 0 && start < double(G_MAXINT64 / GST_SECOND), "Invalid seek time");
  char pci[32]{};
  Hip(hipDeviceGetPCIBusId(pci, sizeof(pci), settings.gpu_id));
  auto reader = Take(hm::gpu::VaVideoReader::Create(
      {hm::gpu::VaVideoCodec::kHevc,
       GST_VIDEO_FORMAT_P010_10LE,
       std::string("/dev/dri/by-path/pci-") + pci + "-render",
       settings.reader_queue_frames,
       settings.operation_timeout_ns,
       settings.cancellation_poll_ns}));
  Status(reader->OpenFile(path, static_cast<GstClockTime>(start * GST_SECOND)));
  return reader;
}
Sample Read(hm::gpu::VaVideoReader& reader) {
  auto frame = Take(reader.Read());
  Check(frame.has_value(), "Raw input ended early");
  return std::move(frame->sample);
}
GstVideoInfo Info(GstSample* sample) {
  GstVideoInfoDmaDrm info{};
  Check(gst_video_info_dma_drm_from_caps(&info, gst_sample_get_caps(sample)), "Missing linear DMA-BUF caps");
  Check(info.vinfo.fps_n > 0 && info.vinfo.fps_d > 0, "Missing input frame rate");
  return info.vinfo;
}
struct Compute {
  hipStream_t stream{};
  std::unique_ptr<hm::pano::cuda::CudaStitchPano<half4, half4>> stitcher;
  std::unique_ptr<hm::CudaMat<half4>> left, right, panorama;
  Compute() {
    Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  }
  ~Compute() {
    (void)hipStreamSynchronize(stream);
    panorama.reset();
    right.reset();
    left.reset();
    stitcher.reset();
    (void)hipStreamDestroy(stream);
  }
};
} // namespace

int main(int argc, char** argv) {
  if (argc != 15) {
    std::cerr << "Usage: hip_program_fixture GAME MODEL INFER_YAML CAMERA_YAML LEFT RIGHT "
                 "LEFT_SECONDS RIGHT_SECONDS FRAMES MODEL_WIDTH MODEL_HEIGHT OUTPUT_WIDTH OUTPUT_HEIGHT OUTPUT_MP4\n";
    return 2;
  }
  gst_init(&argc, &argv);
  std::cout << std::unitbuf;
  try {
    const auto baseline = Take(hm::baseline_config::load());
    const auto settings = Take(hm::program::ResolveAmdProgramSettings(baseline.values));
    const std::string game = argv[1];
    Check(!std::filesystem::exists(argv[14]), "Output already exists");
    const int frames = std::stoi(argv[9]), out_width = std::stoi(argv[12]), out_height = std::stoi(argv[13]);
    const double left_start = std::stod(argv[7]), right_start = std::stod(argv[8]);
    Check(frames > 0 && out_width > 0 && out_height > 0, "Invalid fixture dimensions or frame count");
    const auto yaml = YAML::LoadFile(argv[3]), property = yaml["property"], attrs = yaml["class-attrs-all"];
    Check(property["model-color-format"].as<int>() == 0, "Fixture requires RGB model");
    hm::detection::DetectorConfig config{
        argv[2],
        "input",
        std::stoi(argv[10]),
        std::stoi(argv[11]),
        property["num-detected-classes"].as<int>(),
        settings.gpu_id,
        attrs["topk"].as<int>(),
        property["net-scale-factor"].as<float>(),
        attrs["pre-cluster-threshold"].as<float>(),
        attrs["nms-iou-threshold"].as<float>(),
        property["maintain-aspect-ratio"].as<int>() != 0,
        property["symmetric-padding"].as<int>() != 0,
        hm::gpu::MigraphxSession::Precision::kFloat16,
        settings.maximum_inference_io_bytes};
    Hip(hipSetDevice(config.device));
    Compute compute;
    auto artifacts = Take(hm::stitching::lock_stitching_artifacts_for_load(game, 0));
    Check(artifacts.artifact_lock && artifacts.load_snapshot, "Missing compatible saved stitching artifacts");
    hm::pano::ControlMasks masks;
    Check(masks.load(artifacts.load_snapshot->directory().string(), 0), "Cannot load pinned control masks");
    Status(artifacts.load_snapshot->verify());
    Check(
        masks.canvas_width() == artifacts.canvas_size.width && masks.canvas_height() == artifacts.canvas_size.height,
        "Loaded control masks changed validated dimensions");
    const auto generation = Take(hm::stitching::stitched_output_generation_id(
        artifacts.generation_id, 0, masks.canvas_width(), masks.canvas_height()));
    const auto loaded_generation = artifacts.generation_id;
    artifacts.artifact_lock.reset();
    artifacts.load_snapshot.reset();
    Check(
        Take(hm::stitching::current_stitched_output_generation_id(game)) == generation,
        "Fixture requires current unrotated saved geometry");
    auto mask = Take(hm::stitching::load_field_mask_for_loaded_generation(game, generation, loaded_generation));
    auto tracking = Take(hm::tracking::ProgramTracking::Create(
        mask,
        YAML::LoadFile(argv[4])["play-tracker"],
        settings.tracker,
        baseline.values["ice_boundaries"]["raise_bbox_center_by_height_ratio"].as<float>(),
        baseline.values["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"].as<float>()));
    compute.stitcher = std::make_unique<hm::pano::cuda::CudaStitchPano<half4, half4>>(
        1, hm::pano::BlendSettings::Laplacian(11), masks, true, true, 0, settings.compact_stitch_workspace);
    Check(compute.stitcher->status().ok(), compute.stitcher->status().message());
    const int width = compute.stitcher->canvas_width(), height = compute.stitcher->canvas_height();
    Check(width == mask.cols && height == mask.rows, "Stitcher and rink mask geometry differ");
    std::cout << "Validated raw panorama " << width << 'x' << height << " generation=" << generation << '\n';
    auto detector = Take(hm::detection::HipDetector::Create(config));
    auto left = OpenReader(settings, argv[5], left_start);
    auto right = OpenReader(settings, argv[6], right_start);
    Pipeline output;
    auto a = Read(*left), b = Read(*right);
    const auto left_info = Info(a.get()), right_info = Info(b.get());
    Check(
        left_info.fps_n == right_info.fps_n && left_info.fps_d == right_info.fps_d &&
            gst_video_colorimetry_is_equal(&left_info.colorimetry, &right_info.colorimetry),
        "Camera colorimetry or frame rate differs");
    compute.left = std::make_unique<hm::CudaMat<half4>>(1, left_info.width, left_info.height);
    compute.right = std::make_unique<hm::CudaMat<half4>>(1, right_info.width, right_info.height);
    compute.panorama = std::make_unique<hm::CudaMat<half4>>(1, width, height);
    Check(
        compute.left->is_valid() && compute.right->is_valid() && compute.panorama->is_valid(), "GPU allocation failed");
    GstVideoInfo out_info{};
    Check(
        gst_video_info_set_format(&out_info, GST_VIDEO_FORMAT_P010_10LE, out_width, out_height),
        "Invalid output format");
    out_info.fps_n = left_info.fps_n;
    out_info.fps_d = left_info.fps_d;
    out_info.colorimetry = left_info.colorimetry;
    out_info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
    auto pool = Take(hm::gpu::VaOutputPool::Create(config.device, out_info, settings.output_pool_frames));
    output.Create(
        "appsrc name=frames format=time ! vah265enc name=encoder bitrate=20000 b-frames=0 "
        "key-int-max=60 ! h265parse ! mp4mux ! filesink name=file",
        "frames");
    auto* encoder = gst_bin_get_by_name(GST_BIN(output.value), "encoder");
    const auto encoded_color = hm::program::InstallEncodedColorimetry(
        encoder, hm::program::EncodedCodec::kHevc, out_info, settings.maximum_encoded_packet_bytes);
    gst_object_unref(encoder);
    Status(encoded_color);
    pool->SetDisplayContext(output.value);
    gst_app_src_set_caps(GST_APP_SRC(output.endpoint), pool->caps());
    auto* file = gst_bin_get_by_name(GST_BIN(output.value), "file");
    g_object_set(file, "location", argv[14], nullptr);
    gst_object_unref(file);
    output.Start();
    uint64_t observations = 0, crops = 0;
    GstClockTime first = GST_CLOCK_TIME_NONE, previous = GST_CLOCK_TIME_NONE;
    const auto begin = std::chrono::steady_clock::now();
    for (int sequence = 0; sequence < frames; ++sequence) {
      if (sequence) {
        a = Read(*left);
        b = Read(*right);
      }
      const auto ai = Info(a.get()), bi = Info(b.get());
      Check(
          gst_video_info_is_equal(&left_info, &ai) && gst_video_info_is_equal(&right_info, &bi), "Input caps changed");
      auto* ab = gst_sample_get_buffer(a.get());
      auto* bb = gst_sample_get_buffer(b.get());
      const auto pts = GST_BUFFER_PTS(ab), other_pts = GST_BUFFER_PTS(bb);
      Check(
          GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(other_pts) &&
              (!GST_CLOCK_TIME_IS_VALID(previous) || pts > previous),
          "Invalid raw-camera timestamps");
      const double pair_delta = (double(pts) - double(other_pts)) / GST_SECOND - (left_start - right_start);
      Check(
          std::abs(pair_delta) < .5 * left_info.fps_d / left_info.fps_n,
          "Raw-camera pair exceeds half-frame tolerance");
      if (!sequence)
        first = pts;
      previous = pts;
      auto af = Take(hm::gpu::VaHipFrame::Import(ab, compute.stream, hm::gpu::VaHipFrame::Access::kRead));
      auto bf = Take(hm::gpu::VaHipFrame::Import(bb, compute.stream, hm::gpu::VaHipFrame::Access::kRead));
      const auto ai_gpu = Take(hm::detection::DescribeVaDetectorImage(*af, ai));
      const auto bi_gpu = Take(hm::detection::DescribeVaDetectorImage(*bf, bi));
      Hip(hm::gpu::ConvertToRgba16Float(ai_gpu, compute.left->data(), compute.left->pitch(), compute.stream));
      Hip(hm::gpu::ConvertToRgba16Float(bi_gpu, compute.right->data(), compute.right->pitch(), compute.stream));
      auto stitched =
          compute.stitcher->process(*compute.left, *compute.right, compute.stream, std::move(compute.panorama));
      Check(stitched.ok(), stitched.status().message());
      compute.panorama = stitched.ConsumeValueOrDie();
      const hm::gpu::Image image{
          compute.panorama->data(),
          nullptr,
          size_t(compute.panorama->pitch()),
          0,
          width,
          height,
          hm::gpu::PixelFormat::kRgba16Float};
      auto detections = Take(detector->Detect(image, compute.stream));
      auto observed = Take(tracking->Process(sequence, width, height, detections));
      observations += observed.tracks.size();
      crops += observed.crop.has_value();
      // Match the delayed camera initialization with an explicit fixture full-canvas view.
      const auto crop = observed.crop.value_or(hm::BBox(0, 0, width, height));
      const auto transform = hm::playcropper::CalculateFrameTransformGeometry(width, height, crop, false, 0, 0);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(settings.operation_timeout_ns);
      auto acquired = pool->Acquire();
      while (absl::IsResourceExhausted(acquired.status()) && std::chrono::steady_clock::now() < deadline) {
        const auto error = output.Error();
        Check(error.empty(), error);
        std::this_thread::sleep_for(std::chrono::nanoseconds(settings.cancellation_poll_ns));
        acquired = pool->Acquire();
      }
      Buffer buffer(Take(std::move(acquired)), gst_buffer_unref);
      auto destination =
          Take(hm::gpu::VaHipFrame::Import(buffer.get(), compute.stream, hm::gpu::VaHipFrame::Access::kWrite));
      const auto yuv = Take(hm::playcropper::DescribeVaProgramOutput(*destination, out_info));
      Hip(hm::playcropper::RenderProgramYuv(image, yuv, transform, 0, false, 0, compute.stream));
      Status(destination->Complete());
      destination.reset();
      Status(af->Complete());
      Status(bf->Complete());
      GST_BUFFER_PTS(buffer.get()) = pts - first;
      GST_BUFFER_DURATION(buffer.get()) = GST_BUFFER_DURATION(ab);
      Check(
          gst_app_src_push_buffer(GST_APP_SRC(output.endpoint), buffer.release()) == GST_FLOW_OK,
          "Output refused frame");
      std::cout << "Frame " << sequence << " detections=" << detections.size() << " rink=" << observed.detections.size()
                << " tracks=" << observed.tracks.size() << " crop=" << crop.left << ',' << crop.top << ',' << crop.right
                << ',' << crop.bottom << '\n'
                << std::flush;
    }
    Check(observations && crops, "No tracked Program camera output");
    Check(gst_app_src_end_of_stream(GST_APP_SRC(output.endpoint)) == GST_FLOW_OK, "Output refused EOS");
    auto* bus = gst_element_get_bus(output.value);
    auto* message = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
      gst_message_unref(message);
    Check(eos, "Output did not complete mux EOS: " + output.Error());
    std::cout << frames << " raw pairs -> " << observations << " tracks -> " << crops << " Program crops, "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()
              << " seconds; HEVC MP4 closed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
