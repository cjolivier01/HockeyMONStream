// Hardware qualification of the composed raw-camera Program path. Video stays
// GPU-resident; only detections/tracks and saved calibration artifacts use CPU
// memory. Bounds/tracker tuning come from the baseline; codec/output choices
// and explicit input/model arguments remain qualification controls.
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video-info-dma.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"
#include "hstream/src/libs/program/AmdProgramProcessor.h"
#include "hstream/src/libs/program/AmdProgramSettings.h"
#include "hstream/src/libs/program/EncodedColorimetry.h"

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
std::unique_ptr<hm::gpu::VaVideoPlaylist> OpenPlaylist(
    const hm::program::AmdProgramSettings& settings,
    const char* path,
    double start) {
  Check(std::isfinite(start) && start >= 0 && start < double(G_MAXINT64 / GST_SECOND), "Invalid seek time");
  char pci[32]{};
  Hip(hipDeviceGetPCIBusId(pci, sizeof(pci), settings.gpu_id));
  auto reader = Take(hm::gpu::VaVideoPlaylist::Create(
      {hm::gpu::VaVideoCodec::kHevc,
       GST_VIDEO_FORMAT_P010_10LE,
       std::string("/dev/dri/by-path/pci-") + pci + "-render",
       settings.reader_queue_frames,
       settings.operation_timeout_ns,
       settings.cancellation_poll_ns},
      {path},
      {}));
  Status(reader->Seek(static_cast<GstClockTime>(start * GST_SECOND)));
  return reader;
}
hm::gpu::VaVideoFrame Read(hm::gpu::VaVideoPlaylist& reader) {
  auto frame = Take(reader.Read());
  Check(frame.has_value(), "Raw input ended early");
  return std::move(*frame);
}
GstVideoInfo Info(GstSample* sample) {
  GstVideoInfoDmaDrm info{};
  Check(gst_video_info_dma_drm_from_caps(&info, gst_sample_get_caps(sample)), "Missing linear DMA-BUF caps");
  Check(info.vinfo.fps_n > 0 && info.vinfo.fps_d > 0, "Missing input frame rate");
  return info.vinfo;
}
} // namespace

int main(int argc, char** argv) {
  const bool verify_reset = argc == 16 && std::string(argv[15]) == "--verify-reset";
  if (argc != 15 && !verify_reset) {
    std::cerr
        << "Usage: hip_program_fixture GAME MODEL INFER_YAML CAMERA_YAML LEFT RIGHT "
           "LEFT_SECONDS RIGHT_SECONDS FRAMES MODEL_WIDTH MODEL_HEIGHT OUTPUT_WIDTH OUTPUT_HEIGHT OUTPUT_MP4 [--verify-reset]\n";
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
    auto processor = Take(hm::program::AmdProgramProcessor::Create(
        {game,
         config,
         YAML::LoadFile(argv[4])["play-tracker"],
         settings.tracker,
         hm::pano::BlendSettings::Laplacian(11),
         true,
         settings.compact_stitch_workspace,
         baseline.values["ice_boundaries"]["raise_bbox_center_by_height_ratio"].as<float>(),
         baseline.values["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"].as<float>()}));
    const hm::program::ProgramRenderSettings render{false, 0, 0, 0, false, 0};
    std::cout << "Validated raw panorama " << processor->width() << 'x' << processor->height()
              << " generation=" << processor->generation() << '\n';
    auto left = OpenPlaylist(settings, argv[5], left_start);
    auto right = OpenPlaylist(settings, argv[6], right_start);
    Pipeline output;
    auto a = Read(*left), b = Read(*right);
    const auto left_info = Info(a.sample.get()), right_info = Info(b.sample.get());
    Check(
        left_info.fps_n == right_info.fps_n && left_info.fps_d == right_info.fps_d &&
            gst_video_colorimetry_is_equal(&left_info.colorimetry, &right_info.colorimetry),
        "Camera colorimetry or frame rate differs");
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
    uint64_t observations = 0, crops = 0, highest_id = 0;
    GstClockTime first = GST_CLOCK_TIME_NONE, previous = GST_CLOCK_TIME_NONE;
    const auto begin = std::chrono::steady_clock::now();
    for (int sequence = 0; sequence < frames; ++sequence) {
      if (sequence) {
        a = Read(*left);
        b = Read(*right);
      }
      const auto ai = Info(a.sample.get()), bi = Info(b.sample.get());
      Check(
          gst_video_info_is_equal(&left_info, &ai) && gst_video_info_is_equal(&right_info, &bi), "Input caps changed");
      const auto pts = a.time, other_pts = b.time;
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
      auto completed = Take(processor->Process(sequence, a.sample.get(), b.sample.get()));
      const auto& observed = completed.observation;
      const auto& crop = completed.crop;
      observations += observed.tracks.size();
      crops += observed.crop.has_value();
      for (const auto& track : observed.tracks)
        highest_id = std::max(highest_id, track.id);
      const auto now = std::chrono::steady_clock::now();
      const auto timeout = std::chrono::nanoseconds(settings.operation_timeout_ns);
      const auto deadline = timeout > std::chrono::steady_clock::time_point::max() - now
          ? std::chrono::steady_clock::time_point::max()
          : now + timeout;
      auto acquired = pool->Acquire();
      while (absl::IsResourceExhausted(acquired.status()) && std::chrono::steady_clock::now() < deadline) {
        const auto error = output.Error();
        Check(error.empty(), error);
        std::this_thread::sleep_for(std::chrono::nanoseconds(settings.cancellation_poll_ns));
        acquired = pool->Acquire();
      }
      Buffer buffer(Take(std::move(acquired)), gst_buffer_unref);
      Status(processor->Render(buffer.get(), out_info, render));
      GST_BUFFER_PTS(buffer.get()) = pts - first;
      GST_BUFFER_DURATION(buffer.get()) = a.duration;
      Check(
          gst_app_src_push_buffer(GST_APP_SRC(output.endpoint), buffer.release()) == GST_FLOW_OK,
          "Output refused frame");
      std::cout << "Frame " << sequence << " detections=" << completed.detection_count
                << " rink=" << observed.detections.size() << " tracks=" << observed.tracks.size()
                << " crop=" << crop.left << ',' << crop.top << ',' << crop.right << ',' << crop.bottom << '\n'
                << std::flush;
    }
    Check(observations && crops, "No tracked Program camera output");
    Check(gst_app_src_end_of_stream(GST_APP_SRC(output.endpoint)) == GST_FLOW_OK, "Output refused EOS");
    auto* bus = gst_element_get_bus(output.value);
    auto* message = gst_bus_timed_pop_filtered(
        bus, settings.operation_timeout_ns, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
      gst_message_unref(message);
    Check(eos, "Output did not complete mux EOS: " + output.Error());
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << frames << " raw pairs -> " << observations << " tracks -> " << crops << " Program crops, " << seconds
              << " seconds; HEVC MP4 closed\n";
    if (verify_reset) {
      Buffer output_frame(Take(pool->Acquire()), gst_buffer_unref);
      auto wrong_color = out_info;
      wrong_color.colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
      Check(
          absl::IsInvalidArgument(processor->Render(output_frame.get(), wrong_color, render)),
          "Output transfer mismatch was accepted");
      Status(processor->Render(output_frame.get(), out_info, render));
      Status(processor->Reset());
      Check(
          absl::IsFailedPrecondition(processor->Render(output_frame.get(), out_info, render)),
          "Reset retained a stale panorama");
      Status(left->Seek(static_cast<GstClockTime>(left_start * GST_SECOND)));
      Status(right->Seek(static_cast<GstClockTime>(right_start * GST_SECOND)));
      uint64_t reset_observations = 0;
      for (unsigned sequence = 0; sequence < 4; ++sequence) {
        a = Read(*left);
        b = Read(*right);
        const auto completed = Take(processor->Process(sequence, a.sample.get(), b.sample.get()));
        for (const auto& track : completed.observation.tracks) {
          Check(track.id > highest_id, "Seek reused an object ID");
          ++reset_observations;
        }
        Status(processor->Render(output_frame.get(), out_info, render));
        auto full = render;
        full.no_crop = true;
        Status(processor->Render(output_frame.get(), out_info, full));
      }
      Check(reset_observations, "No confirmed tracks after processor reset/reader seek");
      Check(!processor->Process(4, nullptr, b.sample.get()).ok(), "Invalid input was accepted");
      Check(
          !processor->Reset().ok() && !processor->Render(output_frame.get(), out_info, render).ok(),
          "Processing failure did not retire the processor");
      std::cout << "Processor seek/reset, repeated Program/Stitched render, monotonic IDs and retirement passed\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
