#include "hstream/src/gst-plugins/gst-fieldmask/dsfieldmask_lib.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

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

  std::string initial_output_generation;
  std::string rotated_output_generation;
  auto hugin_lock = hm::stitching::HuginProject::RecoverAndLock(tmpdir);
  if (!hugin_lock.ok()) {
    std::cerr << "Failed to lock Hugin generation: " << hugin_lock.status() << std::endl;
    return 4;
  }
  auto hugin_generation = hm::stitching::HuginProject::GenerationId(tmpdir, **hugin_lock);
  if (!hugin_generation.ok()) {
    std::cerr << "Failed to identify Hugin generation: " << hugin_generation.status() << std::endl;
    return 5;
  }
  auto initial_generation = hm::stitching::stitched_output_generation_id(*hugin_generation, 0.0);
  auto rotated_generation = hm::stitching::stitched_output_generation_id(*hugin_generation, 5.0);
  if (!initial_generation.ok() || !rotated_generation.ok()) {
    std::cerr << "Failed to identify stitched-output generations" << std::endl;
    return 6;
  }
  initial_output_generation = *initial_generation;
  rotated_output_generation = *rotated_generation;
  hugin_lock->reset();

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

  NvDsFrameMeta stack_frame_meta{};
  NvDsFrameMeta* frame_meta = &stack_frame_meta;
#ifdef HAS_NVDS_CUSTOMUSERMETA
  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(2);
  if (!batch_meta) {
    std::cerr << "Failed to allocate batch metadata" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    return 7;
  }
  frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  frame_meta->base_meta.batch_meta = batch_meta;
  nvds_add_frame_meta_to_batch(batch_meta, frame_meta);
  hm::stitching::StitchedOutputGenerationPayload::create_and_add<hm::stitching::StitchedOutputGenerationPayload>(
      frame_meta, initial_output_generation);
#endif
  frame_meta->source_frame_width = 64;
  frame_meta->source_frame_height = 64;
  frame_meta->bInferDone = 0;
  frame_meta->obj_meta_list = nullptr;

  const absl::Status status = DsFieldMaskProcessFrame(&surface, /*frame_index=*/0, frame_meta, ctx, /*draw=*/false);
  if (!status.ok()) {
    std::cerr << "Expected OK status when loading existing mask, got: " << status << std::endl;
    DsFieldMaskCtxDeinit(ctx);
#ifdef HAS_NVDS_CUSTOMUSERMETA
    nvds_destroy_batch_meta(batch_meta);
#endif
    return 1;
  }

#ifdef HAS_NVDS_CUSTOMUSERMETA
  NvDsFrameMeta* rotated_frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  rotated_frame_meta->base_meta.batch_meta = batch_meta;
  rotated_frame_meta->source_frame_width = 64;
  rotated_frame_meta->source_frame_height = 64;
  nvds_add_frame_meta_to_batch(batch_meta, rotated_frame_meta);
  hm::stitching::StitchedOutputGenerationPayload::create_and_add<hm::stitching::StitchedOutputGenerationPayload>(
      rotated_frame_meta, rotated_output_generation);
  const absl::Status rotated_status =
      DsFieldMaskProcessFrame(nullptr, /*frame_index=*/0, rotated_frame_meta, ctx, /*draw=*/false);
  if (rotated_status.code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Generation change after the first frame must revalidate the mask, got: " << rotated_status
              << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 8;
  }
#endif

  DsFieldMaskCtxDeinit(ctx);
#ifdef HAS_NVDS_CUSTOMUSERMETA
  nvds_destroy_batch_meta(batch_meta);
#endif
  return 0;
}
