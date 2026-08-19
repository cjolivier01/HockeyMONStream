#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"

#include <cmath>
#include <iostream>
#include <vector>

#include "nvbufsurface.h"

namespace {

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

  hm::playcropper::PlayCropperPriv cropper(/*gpu_id=*/0, /*batch_size=*/1);
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
  if (!cropper.SetProperty({"shadow-lift", "75"}) || cropper.SetProperty({"shadow-lift", "-1"}) ||
      cropper.SetProperty({"shadow-lift", "101"}) || cropper.SetProperty({"shadow-lift", "nan"})) {
    std::cerr << "Shadow lift should accept finite percentages from 0 through 100 only" << std::endl;
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

  return 0;
}
