#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"

#include <iostream>

#include "nvbufsurface.h"

namespace {

bool expect_size(
    hm::playcropper::PlayCropperPriv& cropper,
    size_t input_width,
    size_t input_height,
    size_t expected_width,
    size_t expected_height) {
  NvBufSurfaceParams surface_params{};
  surface_params.width = input_width;
  surface_params.height = input_height;

  NvBufSurface surface{};
  surface.numFilled = 1;
  surface.surfaceList = &surface_params;

  auto size = cropper.PrepareRuntimeOutputSize(nullptr, &surface);
  if (!size.ok()) {
    std::cerr << "PrepareRuntimeOutputSize failed: " << size.status() << std::endl;
    return false;
  }
  if (size->width != expected_width || size->height != expected_height) {
    std::cerr << "Unexpected runtime size: " << size->width << "x" << size->height
              << ", expected: " << expected_width << "x" << expected_height << std::endl;
    return false;
  }
  return true;
}

}  // namespace

int main() {
  hm::playcropper::PlayCropperPriv cropper(/*gpu_id=*/0, /*batch_size=*/1);
  if (!expect_size(cropper, /*input_width=*/12102, /*input_height=*/5153, 9158, 5152)) {
    return 1;
  }

  if (!cropper.SetProperty({"runtime-output-max-width", "3840"})) {
    return 1;
  }
  if (!cropper.SetProperty({"runtime-output-max-height", "2160"})) {
    return 1;
  }
  if (!expect_size(cropper, /*input_width=*/12102, /*input_height=*/5153, 3838, 2160)) {
    return 1;
  }

  NvBufSurface empty_surface{};
  auto empty_result = cropper.PrepareRuntimeOutputSize(nullptr, &empty_surface);
  if (empty_result.ok()) {
    std::cerr << "Expected empty input surface to fail" << std::endl;
    return 1;
  }

  return 0;
}
