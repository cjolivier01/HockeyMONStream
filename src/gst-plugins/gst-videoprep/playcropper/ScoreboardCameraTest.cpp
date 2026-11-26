#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "hstream/src/libs/program/ProgramGpu.h"

#include <iostream>

namespace hm::stitcher {
struct StitcherOutputFailureTestPeer {
  static bool Prepare(StitcherPriv& stitcher, cudaStream_t stream, int width, int height) {
    pano::ControlMasks masks;
    masks.img1_col.create(height, width, CV_16U);
    masks.img1_row.create(height, width, CV_16U);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        masks.img1_col.at<uint16_t>(y, x) = x;
        masks.img1_row.at<uint16_t>(y, x) = y;
      }
    masks.img2_col = masks.img1_col.clone();
    masks.img2_row = masks.img1_row.clone();
    masks.positions = {{0.f, 0.f}, {0.f, 0.f}};
    masks.whole_seam_mask_image = cv::Mat(height, width, CV_8U, cv::Scalar(0));
    masks.whole_seam_mask_image.colRange(0, width / 2).setTo(1);
    stitcher.cuda_stream_ = stream;
    stitcher.process_pass_ = 1;
    stitcher.hugin_generation_id_ = "scoreboard-camera-fixture";
    stitcher.stitcher_fp32_ = std::make_unique<StitcherPriv::STITCHER_FP32>(1, 3, masks, true, false, 0, false);
    return stitcher.stitcher_fp32_->status().ok();
  }
};
} // namespace hm::stitcher
namespace {
using hm::media::test::Check;
void Cuda(cudaError_t value) {
  Check(value == cudaSuccess, cudaGetErrorString(value));
}
template <typename T>
T Take(absl::StatusOr<T> result) {
  Check(result.ok(), result.status().ToString());
  return std::move(*result);
}
class Surface {
 public:
  Surface(unsigned width, unsigned height, unsigned count = 1) : width(width), height(height) {
    NvBufSurfaceCreateParams params{};
    params.width = width;
    params.height = height;
    params.gpuId = 0;
    params.layout = NVBUF_LAYOUT_PITCH;
    params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
#if defined(__aarch64__)
    params.memType = NVBUF_MEM_SURFACE_ARRAY;
#else
    params.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
    Check(NvBufSurfaceCreate(&value, count, &params) == 0, "NvBufSurface fixture allocation failed");
    value->numFilled = count;
  }
  ~Surface() {
    NvBufSurfaceDestroy(value);
  }
  std::vector<unsigned char> Pixels(const std::vector<unsigned char>* upload = nullptr, unsigned index = 0) {
#if defined(__aarch64__)
    hm::surface::EglSurfaceMapper mapper(value, index, upload == nullptr);
    Cuda(mapper.status());
    auto view = mapper.get_surface();
#else
    hm::surface::Surface view(&value->surfaceList[index]);
#endif
    std::vector<unsigned char> pixels(width * height * 4);
    if (upload) {
      Check(upload->size() == pixels.size(), "invalid upload size");
      Cuda(cudaMemcpy2D(
          view.dataptr(), view.pitch(), upload->data(), width * 4, width * 4, height, cudaMemcpyHostToDevice));
    } else {
      Cuda(cudaMemcpy2D(
          pixels.data(), width * 4, view.dataptr(), view.pitch(), width * 4, height, cudaMemcpyDeviceToHost));
    }
    Cuda(cudaDeviceSynchronize()); // Fixture imports retire after upload/readback.
    return pixels;
  }
  NvBufSurface* value{nullptr};
  unsigned width, height;
};

class Cropper : public hm::playcropper::PlayCropperPriv {
 public:
  explicit Cropper(cudaStream_t stream) : PlayCropperPriv(0, 1) {
    cuda_stream_ = stream;
    m_buffer_pool_config.max_buffers = 1;
    Check(SetProperty({"no-crop", "1"}), "no-crop rejected");
    Check(SetProperty({"show-scoreboard", "1"}), "scoreboard enable rejected");
    Check(SetProperty({"scoreboard-projected-width", "80"}), "width rejected");
    Check(SetProperty({"scoreboard-projected-height", "40"}), "height rejected");
    Check(SetProperty({"scoreboard-scale", "1"}), "scale rejected");
  }
};
std::string Encoded(const YAML::Node& node) {
  const auto text = YAML::Dump(node);
  auto* encoded = g_base64_encode(reinterpret_cast<const guchar*>(text.data()), text.size());
  std::string result(encoded);
  g_free(encoded);
  while (!result.empty() && result.back() == '=')
    result.pop_back();
  return result;
}
void Benchmark(
    const std::filesystem::path& recording,
    const std::filesystem::path& scoreboard_recording,
    const std::filesystem::path& baseline,
    cudaStream_t stream) {
  const auto track = Take(hm::media::ReadMp4VideoTrack(recording, 5 * GST_SECOND, {}));
  const auto scoreboard_track = Take(hm::media::ReadMp4VideoTrack(scoreboard_recording, 5 * GST_SECOND, {}));
  YAML::Node prepared;
  prepared["camera"] = YAML::Clone(YAML::LoadFile(baseline.string())["rink"]["scoreboard"]["camera"]);
  prepared["camera"]["enabled"] = true;
  prepared["camera"]["files"].push_back(scoreboard_recording.string());
  prepared["camera"]["source_dimensions"].push_back(scoreboard_track.width);
  prepared["camera"]["source_dimensions"].push_back(scoreboard_track.height);
  prepared["camera"]["polygon"] = YAML::Load("[[96,40],[223,40],[223,103],[96,103]]");
  prepared["game_directory"] = recording.parent_path().string();
  prepared["resize_filter"] = "bilinear";
  prepared["sharpen_amount"] = 0;
  auto settings = *Take(
      hm::scoreboard::ResolveCameraConfig(
          prepared["camera"], recording.parent_path(), hm::scoreboard::CameraDecodeBackend::kNvidia, 0, {}));
  auto main_config = settings.decoder;
  main_config.source_width = track.width;
  main_config.source_height = track.height;
  main_config.codec = track.codec == hm::media::VideoCodec::kHevc ? hm::scoreboard::CameraCodec::kHevc
                                                                  : hm::scoreboard::CameraCodec::kH264;
  main_config.region = {0, 0, track.width, track.height, {}};
  for (bool enabled : {false, true}) {
    Surface stitched(track.width, track.height), output(3840, 2160);
    hm::stitcher::StitcherPriv stitcher(0, 2);
    Check(
        hm::stitcher::StitcherOutputFailureTestPeer::Prepare(stitcher, stream, track.width, track.height),
        "Benchmark stitcher setup failed");
    Cropper cropper(stream);
    Check(cropper.SetProperty({"scoreboard-camera", Encoded(prepared)}), "Benchmark camera config failed");
    Check(
        cropper.SetProperty(
            {"scoreboard-camera-timeline", "0," + std::to_string(track.fps_n) + "," + std::to_string(track.fps_d)}),
        "Benchmark timeline failed");
    Check(cropper.SetProperty({"show-scoreboard", enabled ? "1" : "0"}), "Benchmark enable failed");
    Check(cropper.SetProperty({"scoreboard-projected-width", "320"}), "Benchmark overlay width failed");
    Check(cropper.SetProperty({"scoreboard-projected-height", "160"}), "Benchmark overlay height failed");
    auto left = Take(hm::scoreboard::CameraDecoder::Create(main_config));
    auto right = Take(hm::scoreboard::CameraDecoder::Create(main_config));
    Check(left->OpenFile(recording, 0).ok() && right->OpenFile(recording, 0).ok(), "Benchmark main source open failed");
    auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 120; ++i) {
      if (i == 20)
        begin = std::chrono::steady_clock::now();
      auto l = Take(left->Read()), r = Take(right->Read());
      Check(l && r && l->time == r->time, "Benchmark main inputs lost frame alignment");
      const auto expected_time = gst_util_uint64_scale(i, uint64_t(track.fps_d) * GST_SECOND, track.fps_n);
      Check(l->time == expected_time, "Benchmark main decoder changed exact frame phase");
      auto* lb = gst_sample_get_buffer(l->sample.get());
      auto* rb = gst_sample_get_buffer(r->sample.get());
      GstMapInfo lm{}, rm{};
      Check(
          gst_buffer_map(lb, &lm, GST_MAP_READ) && gst_buffer_map(rb, &rm, GST_MAP_READ),
          "Benchmark NVMM descriptor unavailable");
      auto* ls = reinterpret_cast<NvBufSurface*>(lm.data);
      auto* rs = reinterpret_cast<NvBufSurface*>(rm.data);
      NvBufSurfaceParams surfaces[2]{ls->surfaceList[0], rs->surfaceList[0]};
      NvBufSurface inputs{};
      inputs.batchSize = inputs.numFilled = 2;
      inputs.gpuId = 0;
      inputs.memType = ls->memType;
      inputs.surfaceList = surfaces;
      auto* batch = nvds_create_batch_meta(2);
      for (unsigned camera = 0; camera < 2; ++camera) {
        auto* frame = nvds_acquire_frame_meta_from_pool(batch);
        frame->batch_id = frame->source_id = frame->pad_index = camera;
        frame->frame_num = i;
        frame->buf_pts = expected_time;
        frame->num_surfaces_per_frame = 1;
        frame->source_frame_width = track.width;
        frame->source_frame_height = track.height;
        nvds_add_frame_meta_to_batch(batch, frame);
      }
      const auto joined = stitcher.GenerateOutput(batch, &inputs, stitched.value);
      gst_buffer_unmap(lb, &lm);
      gst_buffer_unmap(rb, &rm);
      Check(joined.ok(), joined.ToString());
      const auto rendered = cropper.GenerateOutput(batch, stitched.value, output.value);
      Check(rendered.ok(), rendered.ToString());
      auto* final = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
      Check(
          batch->num_frames_in_batch == 1 && final->buf_pts == expected_time && final->frame_num == int(i),
          "Benchmark output lost a frame or changed its timestamp");
      nvds_destroy_batch_meta(batch);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "SCOREBOARD_BENCH enabled=" << enabled << " frames=100 width=" << track.width
              << " height=" << track.height << " seconds=" << seconds << " fps=" << 100 / seconds << std::endl;
  }
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  cudaStream_t stream = nullptr;
  try {
    Check(
        argc == 2 || ((argc == 4 || argc == 5) && std::string(argv[2]) == "--benchmark"),
        "Expected baseline [--benchmark recording [scoreboard]]");
    Cuda(cudaSetDevice(0));
    Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    if (argc >= 4) {
      Benchmark(argv[3], argc == 5 ? argv[4] : argv[3], argv[1], stream);
      Cuda(cudaStreamDestroy(stream));
      return 0;
    }
    hm::media::test::Directory directory;
    const auto path = directory.path / "scoreboard.mp4";
    hm::media::test::Ffmpeg(
        {"-f",
         "lavfi",
         "-i",
         "testsrc2=s=384x192:r=25:d=1",
         "-c:v",
         "libx264",
         "-preset",
         "ultrafast",
         "-bf",
         "2",
         "-x264-params",
         "colorprim=bt709:transfer=bt709:colormatrix=bt709:fullrange=off",
         path.string()});
    YAML::Node prepared;
    prepared["resize_filter"] = "bilinear";
    prepared["sharpen_amount"] = 0;
    prepared["camera"] = YAML::Clone(YAML::LoadFile(argv[1])["rink"]["scoreboard"]["camera"]);
    auto camera = prepared["camera"];
    camera["enabled"] = true;
    camera["files"].push_back(path.string());
    camera["source_dimensions"] = YAML::Load("[384,192]");
    camera["polygon"] = YAML::Load("[[96,40],[223,40],[223,103],[96,103]]");
    prepared["game_directory"] = directory.path.string();
    const auto config = *Take(
        hm::scoreboard::ResolveCameraConfig(
            camera, directory.path, hm::scoreboard::CameraDecodeBackend::kNvidia, 0, {}));
    auto decoder = Take(hm::scoreboard::CameraDecoder::Create(config.decoder));
    Check(decoder->OpenFile(path, 0).ok(), "Reference reader failed");
    auto images = Take(hm::scoreboard::CameraImages::Create(0, 128, 64, 25));
    std::vector<std::shared_ptr<const hm::gpu::Image>> reference;
    for (unsigned i = 0; i < 25; ++i) {
      auto frame = Take(decoder->Read());
      Check(frame.has_value(), "Reference ended early");
      reference.push_back(Take(images->Copy(*frame, config.decoder.region)));
    }
    decoder.reset();
    {
      Surface input(320, 192, 2), stitched(320, 192), output(320, 192), expected(320, 192);
      std::vector<unsigned char> background(320 * 192 * 4, 32);
      for (size_t i = 3; i < background.size(); i += 4)
        background[i] = 255;
      input.Pixels(&background);
      input.Pixels(&background, 1);
      hm::stitcher::StitcherPriv stitcher(0, 2);
      Check(hm::stitcher::StitcherOutputFailureTestPeer::Prepare(stitcher, stream, 320, 192), "Stitcher setup failed");
      Cropper cropper(stream);
      Check(cropper.SetProperty({"scoreboard-camera", Encoded(prepared)}), "Camera property rejected");
      const auto plan = Take(
          hm::scoreboard::MakePlan(
              config.decoder.region.polygon, 128, 64, 320, 192, 80, 40, hm::scoreboard::Position::kUpperLeft));
      for (int offset_frames : {0, 5, -5}) {
        prepared["camera"]["start_offset"] = offset_frames > 0 ? "00:00:00.1000"
            : offset_frames < 0                                ? "-00:00:00.1000"
                                                               : "00:00:00.0000";
        Check(cropper.SetProperty({"scoreboard-camera", Encoded(prepared)}), "Offset config rejected");
        for (unsigned origin : {0, 10, 20}) {
          auto* flushing = gst_event_new_flush_start();
          Check(cropper.HandleEvent(flushing), "Cropper flush failed");
          gst_event_unref(flushing);
          auto* resumed = gst_event_new_flush_stop(TRUE);
          Check(stitcher.HandleEvent(resumed), "Stitcher seek reset failed");
          Check(cropper.HandleEvent(resumed), "Cropper seek reset failed");
          gst_event_unref(resumed);
          Check(
              cropper.SetProperty({"scoreboard-camera-timeline", std::to_string(origin) + ",50,1"}),
              "Game seek timeline rejected");
          for (unsigned i = 0; i < 50 - origin - std::max(0, offset_frames); ++i) {
            auto* batch = nvds_create_batch_meta(2);
            for (unsigned camera_index = 0; camera_index < 2; ++camera_index) {
              auto* frame = nvds_acquire_frame_meta_from_pool(batch);
              frame->batch_id = frame->source_id = frame->pad_index = camera_index;
              frame->buf_pts = i * 20 * GST_MSECOND;
              frame->frame_num = i;
              frame->num_surfaces_per_frame = 1;
              frame->source_frame_width = 320;
              frame->source_frame_height = 192;
              nvds_add_frame_meta_to_batch(batch, frame);
            }
            const auto stitched_status = stitcher.GenerateOutput(batch, input.value, stitched.value);
            Check(stitched_status.ok(), stitched_status.ToString());
            Check(
                batch->num_frames_in_batch == 1 && stitched.value->numFilled == 1,
                "Scoreboard camera changed the two-input stitch batch");
            const auto status = cropper.GenerateOutput(batch, stitched.value, output.value);
            nvds_destroy_batch_meta(batch);
            Check(status.ok(), status.ToString());
            Check(cudaStreamQuery(stream) == cudaSuccess, "Cropper released scoreboard before GPU completion");
            expected.Pixels(&background);
#ifdef __aarch64__
            hm::surface::EglSurfaceMapper mapper(expected.value, 0, false);
            Cuda(mapper.status());
            auto view = mapper.get_surface();
#else
            hm::surface::Surface view(&expected.value->surfaceList[0]);
#endif
            const int source_frame = int(origin + i) + offset_frames;
            if (source_frame >= 0)
              Cuda(
                  hm::playcropper::CompositeScoreboard(
                      *reference[source_frame / 2],
                      {view.dataptr(), view.pitch(), 320, 192, hm::gpu::PixelFormat::kRgba8},
                      plan,
                      stream));
            Cuda(cudaStreamSynchronize(stream));
            const auto actual = output.Pixels(), wanted = expected.Pixels();
            for (int y = plan.top; y < plan.top + plan.height; ++y)
              for (int x = plan.left; x < plan.left + plan.width; ++x)
                for (int c = 0; c < 4; ++c)
                  Check(
                      actual[(y * 320 + x) * 4 + c] == wanted[(y * 320 + x) * 4 + c],
                      "Real cropper overlaid the wrong camera frame after playback/seek");
          }
        }
      }
    }
    Cuda(cudaStreamDestroy(stream));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (stream)
      cudaStreamDestroy(stream);
    return 1;
  }
}
