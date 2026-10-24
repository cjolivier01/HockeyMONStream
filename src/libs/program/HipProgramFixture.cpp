// Hardware qualification of the composed raw-camera Program path. Video stays
// GPU-resident; only detections/tracks and saved calibration artifacts use CPU
// memory. Bounds/tracker tuning come from the baseline; codec/output choices
// and explicit input/model arguments remain qualification controls.
#include <fcntl.h>
#include <gst/video/video-info-dma.h>
#include <unistd.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/gpu/vaapi/VaStereoVideo.h"
#include "hstream/src/libs/gpu/vaapi/VaVideoPlaylist.h"
#include "hstream/src/libs/media/StereoFramePlan.h"
#include "hstream/src/libs/program/AmdProgramProcessor.h"
#include "hstream/src/libs/program/AmdProgramSettings.h"
#include "hstream/src/libs/program/AmdVideoStages.h"
#include "hstream/src/libs/program/ProgramDetectorSettings.h"
#include "hstream/src/libs/program/ProgramLaunchConfig.h"
#include "hstream/src/libs/program/VaProgramSink.h"
#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
hm::gpu::VaVideoReaderConfig ReaderConfig(
    const hm::program::AmdProgramSettings& settings,
    const std::vector<std::filesystem::path>& paths) {
  char pci[32]{};
  Hip(hipDeviceGetPCIBusId(pci, sizeof(pci), settings.gpu_id));
  const auto metadata = Take(hm::media::ReadMp4VideoTrack(paths.at(0), settings.operation_timeout_ns, {}));
  return {
      metadata.codec == hm::media::VideoCodec::kHevc ? hm::gpu::VaVideoCodec::kHevc : hm::gpu::VaVideoCodec::kH264,
      GST_VIDEO_FORMAT_P010_10LE,
      std::string("/dev/dri/by-path/pci-") + pci + "-render",
      settings.reader_queue_frames,
      settings.operation_timeout_ns,
      settings.cancellation_poll_ns};
}
std::unique_ptr<hm::gpu::VaVideoPlaylist> OpenPlaylist(
    const hm::program::AmdProgramSettings& settings,
    const std::vector<std::filesystem::path>& paths,
    GstClockTime start) {
  auto reader = Take(hm::gpu::VaVideoPlaylist::Create(ReaderConfig(settings, paths), paths, {}));
  Status(reader->Seek(start));
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
  const bool configured = argc >= 9 && std::string(argv[1]) == "--configured";
  bool verify_reset = argc == 16 && std::string(argv[15]) == "--verify-reset";
  if (!configured && argc != 15 && !verify_reset) {
    std::cerr
        << "Usage: hip_program_fixture GAME MODEL INFER_YAML CAMERA_YAML LEFT RIGHT "
           "LEFT_SECONDS RIGHT_SECONDS FRAMES MODEL_WIDTH MODEL_HEIGHT OUTPUT_WIDTH OUTPUT_HEIGHT OUTPUT_MP4 [--verify-reset]\n"
        << "Or: hip_program_fixture --configured GAME APP_YAML START_SECONDS FRAMES OUT_WIDTH OUT_HEIGHT OUTPUT_MP4 "
           "[--options=key=value,...] [--verify-reset]\n";
    return 2;
  }
  gst_init(&argc, &argv);
  std::cout << std::unitbuf;
  try {
    auto baseline = Take(hm::baseline_config::load());
    auto settings = Take(hm::program::ResolveAmdProgramSettings(baseline.values));
    std::string game, output_file;
    int frames, out_width, out_height;
    double left_start, right_start;
    std::optional<hm::media::StereoFramePlan> pair_plan;
    std::array<double, 2> frame_offsets{};
    GstClockTime configured_start{};
    std::vector<std::filesystem::path> left_files, right_files;
    YAML::Node camera;
    hm::detection::DetectorConfig config;
    auto blend = hm::pano::BlendSettings::Laplacian(11);
    bool minimize_blend = true;
    if (configured) {
      std::vector<std::string> options;
      for (int i = 9; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--verify-reset")
          verify_reset = true;
        else if (argument.rfind("--options=", 0) == 0)
          options.push_back(argument.substr(10));
        else
          throw std::runtime_error("Unknown configured fixture argument: " + argument);
      }
      const auto launch = Take(hm::program::LoadProgramLaunchConfig(argv[2], argv[3], options));
      const auto detector = Take(hm::program::PrepareProgramDetector(launch.inference_file, launch.inference, {}));
      game = launch.game.string();
      camera = launch.camera;
      baseline.values = launch.configuration.values;
      settings = launch.amd;
      frames = std::stoi(argv[5]);
      out_width = std::stoi(argv[6]);
      out_height = std::stoi(argv[7]);
      output_file = argv[8];
      left_files = launch.chapters[0];
      right_files = launch.chapters[1];
      const auto left_meta = Take(hm::media::ReadMp4VideoTrack(left_files.at(0), settings.operation_timeout_ns, {}));
      const auto right_meta = Take(hm::media::ReadMp4VideoTrack(right_files.at(0), settings.operation_timeout_ns, {}));
      Check(
          int64_t(left_meta.fps_n) * right_meta.fps_d == int64_t(right_meta.fps_n) * left_meta.fps_d,
          "Camera frame rates differ");
      pair_plan = Take(hm::media::PlanStereoFrames(
          std::stod(argv[4]), launch.frame_offsets[0], launch.frame_offsets[1], left_meta.fps_n, left_meta.fps_d));
      configured_start = static_cast<GstClockTime>(std::stod(argv[4]) * GST_SECOND);
      frame_offsets = launch.frame_offsets;
      left_start = double(pair_plan->left_time_ns) / GST_SECOND;
      right_start = double(pair_plan->right_time_ns) / GST_SECOND;
      config = {
          detector.onnx,
          detector.input_name,
          detector.width,
          detector.height,
          detector.classes,
          settings.gpu_id,
          detector.maximum_detections,
          detector.scale,
          detector.confidence_threshold,
          detector.iou_threshold,
          detector.maintain_aspect,
          detector.symmetric_padding,
          detector.fp16 ? hm::gpu::MigraphxSession::Precision::kFloat16 : hm::gpu::MigraphxSession::Precision::kFloat32,
          settings.maximum_inference_io_bytes,
          settings.model_cache};
      const auto& selected_blend = launch.blend;
      switch (Take(hm::stitching::ParseBlendMode(selected_blend.mode))) {
        case hm::stitching::BlendMode::kLaplacian:
          blend = hm::pano::BlendSettings::Laplacian(baseline.values["stitching"]["max_blend_levels"].as<int>());
          break;
        case hm::stitching::BlendMode::kAlpha:
          blend = hm::pano::BlendSettings::Alpha(selected_blend.feather_fraction);
          break;
        case hm::stitching::BlendMode::kHardSeam:
          blend = hm::pano::BlendSettings::HardSeam();
          break;
      }
      minimize_blend = launch.minimize_blend;
    } else {
      game = argv[1];
      output_file = argv[14];
      frames = std::stoi(argv[9]);
      out_width = std::stoi(argv[12]);
      out_height = std::stoi(argv[13]);
      left_start = std::stod(argv[7]);
      right_start = std::stod(argv[8]);
      left_files = {argv[5]};
      right_files = {argv[6]};
      camera = YAML::LoadFile(argv[4])["play-tracker"];
      const auto yaml = YAML::LoadFile(argv[3]), property = yaml["property"], attrs = yaml["class-attrs-all"];
      Check(property["model-color-format"].as<int>() == 0, "Fixture requires RGB model");
      config = {
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
          settings.maximum_inference_io_bytes,
          settings.model_cache};
    }
    Check(!std::filesystem::exists(output_file), "Output already exists");
    Check(frames > 0 && out_width > 0 && out_height > 0, "Invalid fixture dimensions or frame count");
    const hm::program::AmdProgramProcessorConfig processor_config{
        game,
        config,
        camera,
        settings.tracker,
        blend,
        minimize_blend,
        settings.compact_stitch_workspace,
        baseline.values["ice_boundaries"]["raise_bbox_center_by_height_ratio"].as<float>(),
        baseline.values["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"].as<float>()};
    auto processor = Take(hm::program::AmdProgramProcessor::Create(processor_config));
    const hm::program::ProgramRenderSettings render{false, 0, 0, 0, false, 0};
    std::cout << "Validated raw panorama " << processor->width() << 'x' << processor->height()
              << " generation=" << processor->generation() << '\n';
    Check(
        std::isfinite(left_start) && left_start >= 0 && left_start < double(G_MAXINT64 / GST_SECOND) &&
            std::isfinite(right_start) && right_start >= 0 && right_start < double(G_MAXINT64 / GST_SECOND),
        "Invalid seek time");
    const GstClockTime left_seek =
        pair_plan ? pair_plan->left_time_ns : static_cast<GstClockTime>(left_start * GST_SECOND);
    const GstClockTime right_seek =
        pair_plan ? pair_plan->right_time_ns : static_cast<GstClockTime>(right_start * GST_SECOND);
    std::unique_ptr<hm::gpu::VaStereoVideo> stereo;
    std::unique_ptr<hm::gpu::VaVideoPlaylist> left, right;
    if (configured) {
      stereo = Take(hm::gpu::VaStereoVideo::Create(
          {{ReaderConfig(settings, left_files), ReaderConfig(settings, right_files)},
           {left_files, right_files},
           frame_offsets},
          {}));
      Status(stereo->Seek(configured_start));
    } else {
      left = OpenPlaylist(settings, left_files, left_seek);
      right = OpenPlaylist(settings, right_files, right_seek);
    }
    hm::gpu::VaVideoFrame a, b;
    GstClockTime synchronized_time{}, synchronized_duration{};
    const auto read_pair = [&] {
      if (stereo) {
        auto pair = Take(stereo->Read());
        Check(pair.has_value(), "Synchronized input ended early");
        a = std::move(pair->cameras[0]);
        b = std::move(pair->cameras[1]);
        synchronized_time = pair->time;
        synchronized_duration = pair->duration;
      } else {
        a = Read(*left);
        b = Read(*right);
      }
    };
    read_pair();
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
    const int output_fd = open(output_file.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    Check(output_fd >= 0, "Cannot exclusively create fixture output");
    auto created_output = hm::program::VaProgramSink::Create(
        {hm::program::VaProgramSinkKind::kHevcMp4,
         config.device,
         out_info,
         settings.output_pool_frames,
         settings.operation_timeout_ns,
         settings.cancellation_poll_ns,
         false,
         output_fd,
         20000,
         60,
         settings.maximum_encoded_packet_bytes,
         0,
         nullptr});
    close(output_fd);
    auto output = Take(std::move(created_output));
    uint64_t observations = 0, crops = 0, highest_id = 0;
    GstClockTime first = GST_CLOCK_TIME_NONE, previous = GST_CLOCK_TIME_NONE;
    const auto begin = std::chrono::steady_clock::now();
    for (int sequence = 0; sequence < frames; ++sequence) {
      if (sequence) {
        read_pair();
      }
      const auto ai = Info(a.sample.get()), bi = Info(b.sample.get());
      Check(
          gst_video_info_is_equal(&left_info, &ai) && gst_video_info_is_equal(&right_info, &bi), "Input caps changed");
      const auto pts = stereo ? synchronized_time : a.time, other_pts = b.time;
      Check(
          GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(other_pts) &&
              (!GST_CLOCK_TIME_IS_VALID(previous) || pts > previous),
          "Invalid raw-camera timestamps");
      const double pair_delta = (double(a.time) - double(other_pts)) / GST_SECOND - (left_start - right_start);
      Check(
          stereo || std::abs(pair_delta) < .5 * left_info.fps_d / left_info.fps_n,
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
      Buffer buffer(Take(output->Acquire()), gst_buffer_unref);
      Status(processor->Render(buffer.get(), out_info, render));
      Status(output->Push(buffer.release(), pts - first, stereo ? synchronized_duration : a.duration));
      std::cout << "Frame " << sequence << " detections=" << completed.detection_count
                << " rink=" << observed.detections.size() << " tracks=" << observed.tracks.size()
                << " crop=" << crop.left << ',' << crop.top << ',' << crop.right << ',' << crop.bottom << '\n'
                << std::flush;
    }
    Check(observations && crops, "No tracked Program camera output");
    Status(output->Finish());
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << frames << " raw pairs -> " << observations << " tracks -> " << crops << " Program crops, " << seconds
              << " seconds; HEVC MP4 closed\n";
    if (verify_reset) {
      auto pool = Take(hm::gpu::VaOutputPool::Create(config.device, out_info, settings.output_pool_frames));
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
      if (stereo) {
        Status(stereo->Seek(configured_start));
      } else {
        Status(left->Seek(left_seek));
        Status(right->Seek(right_seek));
      }
      uint64_t reset_observations = 0;
      for (unsigned sequence = 0; sequence < 4; ++sequence) {
        read_pair();
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
      processor.reset();
      auto stitcher = Take(hm::program::AmdStitcher::Create(processor_config, 2));
      auto retained = Take(stitcher->Process(a.sample.get(), b.sample.get()));
      uint64_t before{}, after{};
      Check(
          hipMemcpy(&before, retained->image().plane0, sizeof(before), hipMemcpyDeviceToHost) == hipSuccess,
          "Cannot sample retained test panorama");
      read_pair();
      auto next = Take(stitcher->Process(a.sample.get(), b.sample.get()));
      Check(retained->image().plane0 != next->image().plane0, "Stitcher overwrote a retained downstream panorama");
      Check(
          absl::IsResourceExhausted(stitcher->Process(a.sample.get(), b.sample.get()).status()),
          "Stitcher exceeded its panorama allocation bound");
      stitcher.reset();
      Check(
          hipMemcpy(&after, retained->image().plane0, sizeof(after), hipMemcpyDeviceToHost) == hipSuccess &&
              before == after,
          "Downstream panorama did not survive producer destruction");
      std::cout
          << "Retained panoramas, bounded allocation and producer destruction passed (eight bytes read back per test sample)\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
