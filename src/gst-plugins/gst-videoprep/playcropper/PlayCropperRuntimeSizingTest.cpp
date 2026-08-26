#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <cmath>
#include <iostream>
#include <vector>

#include "nvbufsurface.h"

namespace {

class TestPlayCropperPriv : public hm::playcropper::PlayCropperPriv {
 public:
  using PlayCropperPriv::PlayCropperPriv;

  bool ApplyScoreboardEpoch(const NvDsFrameMeta* frame_meta) {
    return ApplyScoreboardOutputEpoch(frame_meta).ok();
  }
  bool scoreboard_disabled() const {
    return scoreboard_disabled_;
  }
  size_t scoreboard_point_count() const {
    return scoreboard_perspective_polygion_.size();
  }
  cv::Point2f scoreboard_first_point() const {
    return scoreboard_perspective_polygion_.empty() ? cv::Point2f{} : scoreboard_perspective_polygion_.front();
  }
};

int synchronize_calls = 0;
int synchronize_failures_remaining = 0;
cudaStream_t synchronized_stream = nullptr;

cudaError_t fake_stream_synchronize(cudaStream_t stream) {
  ++synchronize_calls;
  synchronized_stream = stream;
  if (synchronize_failures_remaining > 0) {
    --synchronize_failures_remaining;
    return cudaErrorUnknown;
  }
  return cudaSuccess;
}

bool expect_size(
    hm::playcropper::PlayCropperPriv& cropper,
    size_t input_width,
    size_t input_height,
    guint input_batch_capacity,
    guint input_filled,
    size_t expected_width,
    size_t expected_height) {
  std::vector<NvBufSurfaceParams> surface_params(input_batch_capacity);
  for (NvBufSurfaceParams& params : surface_params) {
    params.width = input_width;
    params.height = input_height;
  }

  NvBufSurface surface{};
  surface.batchSize = input_batch_capacity;
  surface.numFilled = input_filled;
  surface.surfaceList = surface_params.data();

  auto size = cropper.PrepareRuntimeOutputSize(nullptr, &surface);
  if (!size.ok()) {
    std::cerr << "PrepareRuntimeOutputSize failed: " << size.status() << std::endl;
    return false;
  }
  if (size->width != expected_width || size->height != expected_height || size->batch_size != input_batch_capacity) {
    std::cerr << "Unexpected runtime size: " << size->width << "x" << size->height << " batch " << size->batch_size
              << ", expected: " << expected_width << "x" << expected_height << " batch " << input_batch_capacity
              << std::endl;
    return false;
  }
  return true;
}

} // namespace

int main() {
  const hm::BBox left_tracking_box(100, 50, 300, 250);
  const hm::playcropper::FrameTransformGeometry no_crop_transform = hm::playcropper::CalculateFrameTransformGeometry(
      /*input_width=*/1000,
      /*input_height=*/500,
      left_tracking_box,
      /*no_crop=*/true,
      /*fixed_edge_rotation_angle_left=*/10.0f,
      /*fixed_edge_rotation_angle_right=*/10.0f);
  if (no_crop_transform.source_rect.left != 0 || no_crop_transform.source_rect.top != 0 ||
      no_crop_transform.source_rect.right != 1000 || no_crop_transform.source_rect.bottom != 500 ||
      no_crop_transform.crop_box.left != 0 || no_crop_transform.crop_box.top != 0 ||
      no_crop_transform.crop_box.right != 1000 || no_crop_transform.crop_box.bottom != 500 ||
      no_crop_transform.anchor_point.x != left_tracking_box.center().x ||
      no_crop_transform.anchor_point.y != left_tracking_box.center().y ||
      std::abs(no_crop_transform.angle - 6.0f) > 1e-6f) {
    std::cerr << "No-crop must preserve full-frame output while rotating around the tracked camera box\n";
    return 1;
  }
  const hm::playcropper::FrameTransformGeometry cropped_transform = hm::playcropper::CalculateFrameTransformGeometry(
      /*input_width=*/1000,
      /*input_height=*/500,
      left_tracking_box,
      /*no_crop=*/false,
      /*fixed_edge_rotation_angle_left=*/10.0f,
      /*fixed_edge_rotation_angle_right=*/10.0f);
  if (cropped_transform.source_rect.left != 100 || cropped_transform.source_rect.right != 300 ||
      cropped_transform.crop_box.left != 0 || cropped_transform.crop_box.right != 200 ||
      cropped_transform.anchor_point.x != 100 || cropped_transform.anchor_point.y != 150 ||
      std::abs(cropped_transform.angle - 6.0f) > 1e-6f) {
    std::cerr << "Cropped transform geometry changed while adding no-crop rotation support\n";
    return 1;
  }

  TestPlayCropperPriv cropper(/*gpu_id=*/0, /*batch_size=*/1);
  if (!expect_size(
          cropper,
          /*input_width=*/12102,
          /*input_height=*/5153,
          /*input_batch_capacity=*/2,
          /*input_filled=*/1,
          9158,
          5152)) {
    return 1;
  }

  if (!cropper.SetProperty({"runtime-output-max-width", "3840"})) {
    return 1;
  }
  if (!cropper.SetProperty({"runtime-output-max-height", "2160"})) {
    return 1;
  }
  if (!cropper.SetProperty({"scoreboard-perspective-polygon", "1,2,3,4,5,6,7,8"}) ||
      cropper.SetProperty({"scoreboard-perspective-polygon", "1,2,3"}) ||
      cropper.SetProperty({"scoreboard-perspective-polygon", "1,2,3,4,5,6,7,nan"}) ||
      !cropper.SetProperty({"scoreboard-perspective-polygon", "0,0,0,0,0,0,0,0"})) {
    std::cerr << "Scoreboard runtime geometry must require four finite points and accept the disabled sentinel"
              << std::endl;
    return 1;
  }
  if (!cropper.SetProperty({"shadow-lift", "75"}) || cropper.SetProperty({"shadow-lift", "-1"}) ||
      cropper.SetProperty({"shadow-lift", "101"}) || cropper.SetProperty({"shadow-lift", "nan"})) {
    std::cerr << "Shadow lift should accept finite percentages from 0 through 100 only" << std::endl;
    return 1;
  }
  if (!cropper.SetProperty({"shadow-lift-black-point", "true"}) ||
      !cropper.SetProperty({"shadow-lift-black-point", "FALSE"}) ||
      !cropper.SetProperty({"shadow-lift-black-point", " 1 "}) ||
      !cropper.SetProperty({"shadow-lift-black-point", "0"}) ||
      cropper.SetProperty({"shadow-lift-black-point", "yes"}) ||
      cropper.SetProperty({"shadow-lift-black-point", "2"}) || cropper.SetProperty({"shadow-lift-black-point", ""})) {
    std::cerr << "Shadow black-point lift should accept only true, false, 1, or 0" << std::endl;
    return 1;
  }
  if (!cropper.SetProperty({"exposure", "0"}) || !cropper.SetProperty({"exposure", "0.3"}) ||
      !cropper.SetProperty({"exposure", "1.3"}) || cropper.SetProperty({"exposure", "-0.01"}) ||
      cropper.SetProperty({"exposure", "1.31"}) || cropper.SetProperty({"exposure", "nan"})) {
    std::cerr << "Exposure should accept finite settings from 0.0 through 1.3 only" << std::endl;
    return 1;
  }
  if (!expect_size(
          cropper,
          /*input_width=*/12102,
          /*input_height=*/5153,
          /*input_batch_capacity=*/2,
          /*input_filled=*/1,
          3838,
          2160)) {
    return 1;
  }

  if (!expect_size(
          cropper,
          /*input_width=*/12102,
          /*input_height=*/5153,
          /*input_batch_capacity=*/2,
          /*input_filled=*/2,
          3838,
          2160)) {
    return 1;
  }

  if (!cropper.SetProperty({"no-crop", "1"})) {
    return 1;
  }
  if (!expect_size(
          cropper,
          /*input_width=*/12102,
          /*input_height=*/5153,
          /*input_batch_capacity=*/2,
          /*input_filled=*/1,
          3840,
          1634)) {
    std::cerr << "No-crop output must preserve the full input aspect ratio\n";
    return 1;
  }
  if (!expect_size(
          cropper,
          /*input_width=*/12102,
          /*input_height=*/5153,
          /*input_batch_capacity=*/2,
          /*input_filled=*/2,
          3840,
          1634)) {
    return 1;
  }

  NvBufSurface empty_surface{};
  auto empty_result = cropper.PrepareRuntimeOutputSize(nullptr, &empty_surface);
  if (empty_result.ok()) {
    std::cerr << "Expected empty input surface to fail" << std::endl;
    return 1;
  }

  NvBufSurfaceParams invalid_params{};
  NvBufSurface invalid_surface{};
  invalid_surface.batchSize = 1;
  invalid_surface.numFilled = 2;
  invalid_surface.surfaceList = &invalid_params;
  auto invalid_result = cropper.PrepareRuntimeOutputSize(nullptr, &invalid_surface);
  if (invalid_result.ok()) {
    std::cerr << "Expected an input fill count larger than capacity to fail" << std::endl;
    return 1;
  }

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame_meta = batch_meta ? nvds_acquire_frame_meta_from_pool(batch_meta) : nullptr;
  if (!batch_meta || !frame_meta) {
    std::cerr << "Could not construct preview metadata safety fixture\n";
    if (batch_meta)
      nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  frame_meta->base_meta.batch_meta = batch_meta;
  if (!hm::stitching::add_stitched_output_generation_meta(
          frame_meta, "generation-b", "authorization-b", "0,0,0,0,0,0,0,0") ||
      !cropper.ApplyScoreboardEpoch(frame_meta) || !cropper.scoreboard_disabled() ||
      cropper.scoreboard_point_count() != 0) {
    std::cerr << "A disabled scoreboard epoch must apply only with its stitched frame metadata\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  NvDsFrameMeta* restored_frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  if (!restored_frame_meta) {
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  restored_frame_meta->base_meta.batch_meta = batch_meta;
  if (!hm::stitching::add_stitched_output_generation_meta(
          restored_frame_meta, "generation-a", "authorization-a", "1,2,3,4,5,6,7,8") ||
      !cropper.ApplyScoreboardEpoch(restored_frame_meta) || cropper.scoreboard_disabled() ||
      cropper.scoreboard_point_count() != 4 || cropper.scoreboard_first_point() != cv::Point2f(1.0f, 2.0f)) {
    std::cerr << "A restored scoreboard epoch must apply only with its stitched frame metadata\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  if (!cropper.SetProperty({"scoreboard-perspective-polygon", "9,10,11,12,13,14,15,16"}) ||
      cropper.scoreboard_first_point() != cv::Point2f(9.0f, 10.0f) ||
      !cropper.ApplyScoreboardEpoch(restored_frame_meta) ||
      cropper.scoreboard_first_point() != cv::Point2f(1.0f, 2.0f)) {
    std::cerr << "A direct scoreboard update must not suppress the authoritative frame epoch\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  const hm::preview_overlay::PlayCropperTransform preview_transform{};
  const cudaStream_t fake_stream = reinterpret_cast<cudaStream_t>(0x1234);
  synchronize_calls = 0;
  synchronized_stream = nullptr;
  {
    hm::playcropper::CudaStreamCompletionFence fence(fake_stream, fake_stream_synchronize);
    fence.MarkSubmitted();
    if (hm::preview_overlay::add_playcropper_transform_meta(
            frame_meta,
            preview_transform,
            hm::preview_overlay::PlayCropperTransformAttachmentInjection::kPoolExhausted)) {
      std::cerr << "Injected user-meta pool exhaustion unexpectedly attached a transform\n";
      nvds_destroy_batch_meta(batch_meta);
      return 1;
    }
    // Models any diagnostic failure after output work has already been queued:
    // scope exit must fence the borrowed output before pool reuse.
  }
  if (synchronize_calls != 1 || synchronized_stream != fake_stream) {
    std::cerr << "Post-submit diagnostic failure did not synchronize before output recycle\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  synchronize_calls = 0;
  {
    hm::playcropper::CudaStreamCompletionFence fence(fake_stream, fake_stream_synchronize);
    fence.MarkSubmitted();
    if (!hm::preview_overlay::add_playcropper_transform_meta(frame_meta, preview_transform) ||
        fence.Synchronize() != cudaSuccess) {
      std::cerr << "Normal preview metadata flow or explicit stream synchronization failed\n";
      nvds_destroy_batch_meta(batch_meta);
      return 1;
    }
  }
  if (synchronize_calls != 1) {
    std::cerr << "Normal stream completion was skipped or synchronized twice\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  synchronize_calls = 0;
  synchronize_failures_remaining = 1;
  {
    hm::playcropper::CudaStreamCompletionFence fence(fake_stream, fake_stream_synchronize);
    fence.MarkSubmitted();
    if (fence.Synchronize() != cudaErrorUnknown) {
      std::cerr << "Injected stream synchronization failure was not reported\n";
      nvds_destroy_batch_meta(batch_meta);
      return 1;
    }
    // The failed explicit synchronization keeps the fence armed; destruction
    // must retry instead of allowing an uncertain output surface to recycle.
  }
  if (synchronize_calls != 2) {
    std::cerr << "Failed stream synchronization did not retain the recycle fence\n";
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }
  nvds_destroy_batch_meta(batch_meta);

  return 0;
}
