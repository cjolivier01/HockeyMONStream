#include "hstream/src/gst-plugins/gst-fieldmask/dsfieldmask_lib.h"

#include "absl/status/status.h"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  // If DsFieldMaskProcessFrame ever regresses and unnecessarily calls `create_field_mask`, we want the test to fail
  // quickly instead of invoking a Python toolchain.
  unsetenv("CONDA_PREFIX");
  setenv("PATH", "/does/not/exist", /*overwrite=*/1);

  const fs::path tmpdir = fs::temp_directory_path() / ("dsfieldmask_load_existing_mask_test_" + std::to_string(::getpid()));
  fs::create_directories(tmpdir);

  const fs::path mask_path = tmpdir / "rink_mask_0.png";
  cv::Mat mask(64, 64, CV_8UC1, cv::Scalar(255));
  if (!cv::imwrite(mask_path.string(), mask)) {
    std::cerr << "Failed to write test mask: " << mask_path << std::endl;
    return 2;
  }

  DsFieldMaskInitParams params;
  params.detection_mask_file = mask_path.string();

  DsFieldMaskCtx* ctx = DsFieldMaskCtxInit(&params);
  if (!ctx) {
    std::cerr << "DsFieldMaskCtxInit returned nullptr" << std::endl;
    return 3;
  }

  NvBufSurface surface{};
  surface.numFilled = 1;
  NvBufSurfaceParams surface_params{};
  surface.surfaceList = &surface_params;

  NvDsFrameMeta frame_meta{};
  frame_meta.source_frame_width = 64;
  frame_meta.source_frame_height = 64;
  frame_meta.bInferDone = 0;
  frame_meta.obj_meta_list = nullptr;

  const absl::Status status = DsFieldMaskProcessFrame(&surface, /*frame_index=*/0, &frame_meta, ctx, /*draw=*/false);
  if (!status.ok()) {
    std::cerr << "Expected OK status when loading existing mask, got: " << status << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    return 1;
  }

  DsFieldMaskCtxDeinit(ctx);
  return 0;
}
