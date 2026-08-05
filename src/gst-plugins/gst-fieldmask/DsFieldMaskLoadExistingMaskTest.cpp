#include "hstream/src/gst-plugins/gst-fieldmask/dsfieldmask_lib.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include "absl/status/status.h"

#include <opencv2/imgcodecs.hpp>

#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
  // If DsFieldMaskProcessFrame ever regresses and unnecessarily calls `create_field_mask`, we want the test to fail
  // quickly instead of invoking a Python toolchain.
  unsetenv("CONDA_PREFIX");
  setenv("PATH", "/does/not/exist", /*overwrite=*/1);

  const fs::path tmpdir =
      fs::temp_directory_path() / ("dsfieldmask_load_existing_mask_test_" + std::to_string(::getpid()));
  fs::create_directories(tmpdir);

  for (const char* name : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    std::ofstream(tmpdir / name) << "generation-test-artifact\n";
  }

  cv::Mat mask(64, 64, CV_8UC1, cv::Scalar(255));
  hm::stitching::RinkProfile profile;
  profile.masks = {mask};
  profile.centroid = {32.0, 32.0};
  profile.combined_bbox = {0.0, 0.0, 64.0, 64.0};
  const absl::Status saved = hm::stitching::save_rink_profile(tmpdir.string(), profile);
  if (!saved.ok()) {
    std::cerr << "Failed to publish test mask: " << saved << std::endl;
    return 2;
  }
  const fs::path mask_path = tmpdir / "rink_mask_0.png";

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
