#include "hstream/src/libs/program/AmdProgramProcessor.h"

#include <gst/video/video-info-dma.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "cupano/pano/cudaPano.h"
#include "hstream/src/libs/detection/VaDetectorImage.h"
#include "hstream/src/libs/gpu/ConvertImage.h"
#include "hstream/src/libs/program/VaProgramOutput.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"

namespace hm::program {
namespace {
class Failure : public std::exception {
 public:
  explicit Failure(absl::Status status) : status(std::move(status)) {}
  absl::Status status;
};
void Require(bool condition, absl::Status status) {
  if (!condition)
    throw Failure(std::move(status));
}
void Status(absl::Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Status(result.status());
  return std::move(*result);
}
void Hip(hipError_t status) {
  if (status != hipSuccess)
    throw Failure(absl::InternalError(hipGetErrorString(status)));
}
void Cleanup(hipError_t status) noexcept {
  if (status != hipSuccess)
    std::fprintf(stderr, "AMD Program cleanup: %s\n", hipGetErrorString(status));
}
class DeviceScope {
 public:
  explicit DeviceScope(int device) {
    Hip(hipGetDevice(&previous_));
    Hip(hipSetDevice(device));
  }
  ~DeviceScope() {
    Cleanup(hipSetDevice(previous_));
  }

 private:
  int previous_;
};
GstVideoInfo Info(GstSample* sample) {
  GstVideoInfoDmaDrm info{};
  Require(
      sample && gst_sample_get_buffer(sample) && gst_sample_get_caps(sample),
      absl::InvalidArgumentError("Program requires two video samples"));
  Require(
      gst_video_info_dma_drm_from_caps(&info, gst_sample_get_caps(sample)),
      absl::InvalidArgumentError("Program requires linear DMA-BUF caps"));
  Require(
      info.vinfo.width > 0 && info.vinfo.height > 0 && info.vinfo.fps_n > 0 && info.vinfo.fps_d > 0,
      absl::InvalidArgumentError("Program requires positive input dimensions and frame rate"));
  return info.vinfo;
}
} // namespace

struct AmdProgramProcessor::Impl {
  int device, width{}, height{};
  std::string generation;
  hipStream_t stream{};
  std::unique_ptr<pano::cuda::CudaStitchPano<half4, half4>> stitcher;
  std::unique_ptr<CudaMat<half4>> left, right, panorama;
  std::unique_ptr<detection::HipDetector> detector;
  std::unique_ptr<tracking::ProgramTracking> tracking;
  std::unique_ptr<draw_display::analytics::Compositor> preview_compositor;
  GstVideoInfo left_info{}, right_info{};
  hm::BBox crop;
  bool inputs_bound{false}, has_frame{false}, retired{false};

  explicit Impl(int device) : device(device) {}
  ~Impl() {
    // Members must be destroyed on their owning device, after all work and
    // before destroying the stream retained by the detector.
    int previous = -1;
    Cleanup(hipGetDevice(&previous));
    Cleanup(hipSetDevice(device));
    if (stream)
      Cleanup(hipStreamSynchronize(stream));
    preview_compositor.reset();
    detector.reset();
    panorama.reset();
    right.reset();
    left.reset();
    stitcher.reset();
    if (stream)
      Cleanup(hipStreamDestroy(stream));
    if (previous >= 0)
      Cleanup(hipSetDevice(previous));
  }
  gpu::Image Image() const {
    return {panorama->data(), nullptr, size_t(panorama->pitch()), 0, width, height, gpu::PixelFormat::kRgba16Float};
  }
};

AmdProgramProcessor::AmdProgramProcessor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AmdProgramProcessor::~AmdProgramProcessor() = default;

absl::StatusOr<std::unique_ptr<AmdProgramProcessor>> AmdProgramProcessor::Create(
    const AmdProgramProcessorConfig& config) {
  try {
    // Validate/pin the complete saved artifact set before creating GPU state.
    auto artifacts = Take(stitching::lock_stitching_artifacts_for_load(config.game_directory, 0));
    Require(
        artifacts.artifact_lock && artifacts.load_snapshot,
        absl::FailedPreconditionError("Program requires compatible saved stitching artifacts"));
    Status(ValidateLockedProgramCalibration(config.game_directory, config.calibration, *artifacts.artifact_lock));
    pano::ControlMasks masks;
    Require(
        masks.load(artifacts.load_snapshot->directory().string(), 0),
        absl::FailedPreconditionError("Cannot load pinned Program control masks"));
    Status(artifacts.load_snapshot->verify());
    Require(
        masks.canvas_width() == artifacts.canvas_size.width && masks.canvas_height() == artifacts.canvas_size.height,
        absl::FailedPreconditionError("Loaded Program control masks changed validated geometry"));
    auto generation = Take(stitching::stitched_output_generation_id(
        artifacts.generation_id, 0, masks.canvas_width(), masks.canvas_height()));
    const auto loaded_generation = artifacts.generation_id;
    artifacts.artifact_lock.reset();
    artifacts.load_snapshot.reset();
    Require(
        Take(stitching::current_stitched_output_generation_id(config.game_directory)) == generation,
        absl::FailedPreconditionError("AMD Program requires current unrotated saved geometry"));
    const auto mask = Take(stitching::load_field_mask_for_loaded_generation(
        config.game_directory, generation, loaded_generation, config.calibration.invalidation_id.value_or("")));
    auto tracking = Take(tracking::ProgramTracking::Create(
        mask, config.camera, config.tracker, config.raise_center_ratio, config.lower_bottom_ratio));
    DeviceScope device(config.detector.device);
    auto impl = std::make_unique<Impl>(config.detector.device);
    impl->generation = std::move(generation);
    impl->tracking = std::move(tracking);
    Hip(hipStreamCreateWithFlags(&impl->stream, hipStreamNonBlocking));
    impl->stitcher = std::make_unique<pano::cuda::CudaStitchPano<half4, half4>>(
        1, config.blend, masks, true, config.minimize_blend, 0, config.compact_workspace);
    Require(impl->stitcher->status().ok(), absl::InternalError(impl->stitcher->status().message()));
    impl->width = impl->stitcher->canvas_width();
    impl->height = impl->stitcher->canvas_height();
    Require(
        impl->width == mask.cols && impl->height == mask.rows,
        absl::FailedPreconditionError("Program stitcher and rink mask geometry differ"));
    impl->detector = Take(detection::HipDetector::Create(config.detector));
    impl->panorama = std::make_unique<CudaMat<half4>>(1, impl->width, impl->height);
    Require(impl->panorama->is_valid(), absl::ResourceExhaustedError("Cannot allocate Program panorama"));
    return std::unique_ptr<AmdProgramProcessor>(new AmdProgramProcessor(std::move(impl)));
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("AMD Program preparation: ") + error.what());
  }
}

absl::StatusOr<AmdProgramFrame> AmdProgramProcessor::Process(uint64_t sequence, GstSample* left, GstSample* right) {
  auto& impl = *impl_;
  impl.has_frame = false;
  if (impl.retired)
    return absl::FailedPreconditionError("AMD Program retired after processing failure");
  // A failed partial update must not be continued with old camera state or an
  // incomplete panorama. Reset is for healthy timeline discontinuities only.
  impl.retired = true;
  try {
    DeviceScope device(impl.device);
    const auto li = Info(left), ri = Info(right);
    Require(
        li.fps_n == ri.fps_n && li.fps_d == ri.fps_d &&
            gst_video_colorimetry_is_equal(&li.colorimetry, &ri.colorimetry),
        absl::InvalidArgumentError("Program camera frame rate or colorimetry differs"));
    if (impl.inputs_bound) {
      Require(
          gst_video_info_is_equal(&li, &impl.left_info) && gst_video_info_is_equal(&ri, &impl.right_info),
          absl::InvalidArgumentError("Program input caps changed; reconstruct its calibrated processor"));
    } else {
      impl.left = std::make_unique<CudaMat<half4>>(1, li.width, li.height);
      impl.right = std::make_unique<CudaMat<half4>>(1, ri.width, ri.height);
      Require(
          impl.left->is_valid() && impl.right->is_valid(),
          absl::ResourceExhaustedError("Cannot allocate Program camera workspace"));
      impl.left_info = li;
      impl.right_info = ri;
      impl.inputs_bound = true;
    }
    // Imports drain the stream on every exit, including conversion/stitch errors.
    auto a = Take(gpu::VaHipFrame::Import(gst_sample_get_buffer(left), impl.stream, gpu::VaHipFrame::Access::kRead));
    auto b = Take(gpu::VaHipFrame::Import(gst_sample_get_buffer(right), impl.stream, gpu::VaHipFrame::Access::kRead));
    const auto ai = Take(detection::DescribeVaDetectorImage(*a, li));
    const auto bi = Take(detection::DescribeVaDetectorImage(*b, ri));
    Hip(gpu::ConvertToRgba16Float(ai, impl.left->data(), impl.left->pitch(), impl.stream));
    Hip(gpu::ConvertToRgba16Float(bi, impl.right->data(), impl.right->pitch(), impl.stream));
    auto stitched = impl.stitcher->process(*impl.left, *impl.right, impl.stream, std::move(impl.panorama));
    Require(stitched.ok(), absl::InternalError(stitched.status().message()));
    impl.panorama = stitched.ConsumeValueOrDie();
    auto detected = Take(impl.detector->Detect(impl.Image(), impl.stream));
    auto observed = Take(impl.tracking->Process(sequence, impl.width, impl.height, detected));
    // Before the first confirmed player initializes the native camera, show
    // the complete calibrated panorama instead of an arbitrary crop.
    impl.crop = observed.crop.value_or(hm::BBox(0, 0, impl.width, impl.height));
    Status(a->Complete());
    Status(b->Complete());
    impl.has_frame = true;
    impl.retired = false;
    return AmdProgramFrame{detected.size(), std::move(observed), impl.crop};
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("AMD Program processing: ") + error.what());
  }
}

absl::Status AmdProgramProcessor::Render(
    GstBuffer* output,
    const GstVideoInfo& info,
    const ProgramRenderSettings& settings,
    ProgramPreviewBoxes* preview) {
  auto& impl = *impl_;
  if (impl.retired || !impl.has_frame)
    return absl::FailedPreconditionError("AMD Program has no completed frame to render");
  if (!output || !std::isfinite(settings.fixed_edge_rotation_angle_left) ||
      !std::isfinite(settings.fixed_edge_rotation_angle_right) || !std::isfinite(settings.shadow_lift_percent) ||
      settings.shadow_lift_percent < 0 || settings.shadow_lift_percent > 100 || !std::isfinite(settings.exposure) ||
      settings.exposure < 0 || settings.exposure > 1.3f)
    return absl::InvalidArgumentError("Invalid Program output or rendering settings");
  if (info.colorimetry.primaries != impl.left_info.colorimetry.primaries ||
      info.colorimetry.transfer != impl.left_info.colorimetry.transfer)
    return absl::InvalidArgumentError("Program output must preserve source color primaries and transfer");
  impl.retired = true;
  try {
    DeviceScope device(impl.device);
    const auto transform = playcropper::CalculateFrameTransformGeometry(
        impl.width,
        impl.height,
        impl.crop,
        settings.no_crop,
        settings.fixed_edge_rotation_angle_left,
        settings.fixed_edge_rotation_angle_right);
    auto destination = Take(gpu::VaHipFrame::Import(output, impl.stream, gpu::VaHipFrame::Access::kWrite));
    const auto yuv = Take(playcropper::DescribeVaProgramOutput(*destination, info));
    Hip(playcropper::RenderProgramYuv(
        impl.Image(),
        yuv,
        transform,
        settings.shadow_lift_percent,
        settings.lift_shadow_black_point,
        settings.exposure,
        impl.stream,
        settings.scoreboard ? &*settings.scoreboard : nullptr));
    if (preview) {
      namespace a = draw_display::analytics;
      const auto& commands = preview->Build(transform, yuv.width, yuv.height);
      if (!commands.empty()) {
        Require(yuv.format == gpu::PixelFormat::kP010, absl::InvalidArgumentError("Preview boxes require P010 output"));
        if (!impl.preview_compositor)
          impl.preview_compositor = std::make_unique<a::Compositor>();
        a::ImageView image{yuv.y, yuv.y_pitch, unsigned(yuv.width), unsigned(yuv.height), a::PixelFormat::kP010};
        image.chroma = yuv.uv;
        image.chroma_pitch = yuv.uv_pitch;
        image.explicit_yuv_encoding = true;
        image.yuv_encoding = yuv.color;
        const auto drawn = impl.preview_compositor->Render(image, commands, impl.stream);
        if (drawn.status == a::RenderStatus::kCudaError)
          Hip(drawn.cuda_error);
        Require(
            drawn.status != a::RenderStatus::kInvalidArgument,
            absl::InternalError("Invalid Program preview overlay geometry"));
        if (drawn.status != a::RenderStatus::kOk)
          g_printerr(
              "HSTREAM_PREVIEW_OVERLAY_DIAGNOSTIC layer=players status=suppressed reason=%s\n",
              a::ToString(drawn.status));
      }
    }
    Status(destination->Complete());
    impl.retired = false;
    return absl::OkStatus();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("AMD Program rendering: ") + error.what());
  }
}

absl::Status AmdProgramProcessor::Reset() {
  auto& impl = *impl_;
  impl.has_frame = false;
  if (impl.retired)
    return absl::FailedPreconditionError("Reconstruct AMD Program after processing failure");
  const auto status = impl.tracking->Reset();
  impl.retired = !status.ok();
  return status;
}
int AmdProgramProcessor::width() const {
  return impl_->width;
}
int AmdProgramProcessor::height() const {
  return impl_->height;
}
const std::string& AmdProgramProcessor::generation() const {
  return impl_->generation;
}
} // namespace hm::program
