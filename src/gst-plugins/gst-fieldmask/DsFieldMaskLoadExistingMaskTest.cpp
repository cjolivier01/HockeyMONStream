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
#include <iterator>
#include <string>
#include <vector>

#include <tiffio.h>

namespace fs = std::filesystem;

namespace {

bool write_mapping_tiff(const fs::path& path, uint32_t width, uint32_t height, float x_position) {
  TIFF* tiff = TIFFOpen(path.c_str(), "w");
  if (!tiff)
    return false;
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 32);
  TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
  TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_XRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_YRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_XPOSITION, x_position);
  TIFFSetField(tiff, TIFFTAG_YPOSITION, 0.0f);
  std::vector<float> row(width, 0.0f);
  bool ok = true;
  for (uint32_t y = 0; y < height; ++y)
    ok = ok && TIFFWriteScanline(tiff, row.data(), y, 0) >= 0;
  TIFFClose(tiff);
  return ok;
}

} // namespace

int main() {
  // If DsFieldMaskProcessFrame ever regresses and unnecessarily calls `create_field_mask`, we want the test to fail
  // quickly instead of invoking a Python toolchain.
  unsetenv("CONDA_PREFIX");
  unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  setenv("PATH", "/does/not/exist", /*overwrite=*/1);

  const fs::path tmpdir =
      fs::temp_directory_path() / ("dsfieldmask_load_existing_mask_test_" + std::to_string(::getpid()));
  fs::create_directories(tmpdir);

  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"}) {
    std::ofstream(tmpdir / name) << "generation-test-artifact\n";
  }
  if (!write_mapping_tiff(tmpdir / "mapping_0000.tif", 32, 64, 0.0f) ||
      !write_mapping_tiff(tmpdir / "mapping_0001.tif", 32, 64, 32.0f)) {
    std::cerr << "Failed to write mapping placement TIFF fixtures" << std::endl;
    return 15;
  }
  for (const char* name : {
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    cv::imwrite((tmpdir / name).string(), cv::Mat(64, 32, CV_32FC1, cv::Scalar(0.0f)));
  }

  cv::Mat seam(64, 64, CV_8UC1, cv::Scalar(0));
  seam.colRange(32, seam.cols).setTo(255);
  cv::imwrite((tmpdir / "seam_file.png").string(), seam);
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
  auto initial_mask = hm::stitching::load_field_mask(tmpdir.string(), initial_output_generation);
  if (!initial_mask.ok()) {
    std::cerr << "Failed to load the initial generation fixture directly: " << initial_mask.status() << std::endl;
    return 14;
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

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(2);
  if (!batch_meta) {
    std::cerr << "Failed to allocate batch metadata" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    return 7;
  }
  NvDsFrameMeta* frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  if (!frame_meta) {
    std::cerr << "Failed to allocate initial frame metadata" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 13;
  }
  frame_meta->base_meta.batch_meta = batch_meta;
  nvds_add_frame_meta_to_batch(batch_meta, frame_meta);
  if (!hm::stitching::add_stitched_output_generation_meta(frame_meta, initial_output_generation)) {
    std::cerr << "Failed to attach initial stitched-output generation metadata" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 11;
  }
  frame_meta->source_frame_width = 64;
  frame_meta->source_frame_height = 64;
  frame_meta->bInferDone = 0;
  frame_meta->obj_meta_list = nullptr;

  const absl::Status status = DsFieldMaskProcessFrame(&surface, /*frame_index=*/0, frame_meta, ctx, /*draw=*/false);
  if (!status.ok()) {
    std::cerr << "Expected OK status when loading existing mask, got: " << status << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 1;
  }

  NvDsFrameMeta* rotated_frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  rotated_frame_meta->base_meta.batch_meta = batch_meta;
  rotated_frame_meta->source_frame_width = 64;
  rotated_frame_meta->source_frame_height = 64;
  nvds_add_frame_meta_to_batch(batch_meta, rotated_frame_meta);
  if (!hm::stitching::add_stitched_output_generation_meta(rotated_frame_meta, rotated_output_generation)) {
    std::cerr << "Failed to attach rotated stitched-output generation metadata" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 12;
  }
  const absl::Status rotated_status =
      DsFieldMaskProcessFrame(nullptr, /*frame_index=*/0, rotated_frame_meta, ctx, /*draw=*/false);
  if (rotated_status.code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Generation change after the first frame must revalidate the mask, got: " << rotated_status
              << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 8;
  }

  {
    std::ofstream config(tmpdir / "config.yaml");
    config << "hstream_ui:\n"
              "  stitching_calibration:\n"
              "    status: pending\n"
              "    artifacts_invalidated: false\n"
              "    invalidation_id: newer-fieldmask-run\n";
  }
  fs::remove(mask_path);
  setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", "stale-fieldmask-run", /*overwrite=*/1);
  DsFieldMaskCtx* stale_ctx = DsFieldMaskCtxInit(&params);
  unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  if (!stale_ctx) {
    std::cerr << "DsFieldMaskCtxInit returned nullptr for supersession test" << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 9;
  }
  const absl::Status superseded_status =
      DsFieldMaskProcessFrame(&surface, /*frame_index=*/0, frame_meta, stale_ctx, /*draw=*/false);
  const absl::Status repeated_superseded_status =
      DsFieldMaskProcessFrame(nullptr, /*frame_index=*/0, frame_meta, stale_ctx, /*draw=*/false);
  DsFieldMaskCtxDeinit(stale_ctx);
  const auto superseded_config = [&]() {
    std::ifstream input(tmpdir / "config.yaml");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  if (!superseded_status.ok() || !repeated_superseded_status.ok() || fs::exists(mask_path) ||
      superseded_config.find("invalidation_id: newer-fieldmask-run") == std::string::npos) {
    std::cerr << "Superseded field-mask fallback must wait for a newer frame generation without publishing rink "
                 "artifacts: "
              << superseded_status << ", repeated: " << repeated_superseded_status << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    nvds_destroy_batch_meta(batch_meta);
    return 10;
  }

  DsFieldMaskCtxDeinit(ctx);
  nvds_destroy_batch_meta(batch_meta);
  return 0;
}
