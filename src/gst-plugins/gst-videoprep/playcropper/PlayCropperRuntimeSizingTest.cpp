#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"

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
