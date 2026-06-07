#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

#include "absl/status/status.h"
#include "gst-nvevent.h"
#include "nvdsmeta.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <gst/gst.h>

namespace fs = std::filesystem;

namespace {

struct FrameDesc {
  guint frame_num;
  guint source_id;
};

bool run_precaps(
    const std::string& config_dir,
    bool one_pass_mode,
    bool expect_ok,
    size_t expected_width,
    size_t expected_height) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  if (one_pass_mode) {
    stitcher.SetProperty({"one-pass-mode", "1"});
  }

  hm::DSCustom_CreateParams params{};
  params.config_file = const_cast<char*>(config_dir.c_str());
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720");
  params.output_width_height[0] = 0;
  params.output_width_height[1] = 0;

  absl::Status status = stitcher.PreCapsInit(&params);
  if (params.m_inCaps) {
    gst_caps_unref(params.m_inCaps);
  }

  if (expect_ok != status.ok()) {
    std::cerr << "Unexpected PreCapsInit status for one_pass_mode=" << one_pass_mode << ": " << status << std::endl;
    return false;
  }

  if (!expect_ok) {
    return true;
  }

  if (params.output_width_height[0] != expected_width || params.output_width_height[1] != expected_height) {
    std::cerr << "Unexpected PreCapsInit size: " << params.output_width_height[0] << "x"
              << params.output_width_height[1] << ", expected: " << expected_width << "x" << expected_height
              << std::endl;
    return false;
  }

  return true;
}

bool expect_output_batch_size(guint input_batch_size, guint configured_batch_size, guint expected_batch_size) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  const guint actual_batch_size = stitcher.GetOutputBatchSize(input_batch_size, configured_batch_size);
  if (actual_batch_size != expected_batch_size) {
    std::cerr << "Unexpected stitcher output batch size for input_batch_size=" << input_batch_size
              << ", configured_batch_size=" << configured_batch_size << ": " << actual_batch_size
              << ", expected: " << expected_batch_size << std::endl;
    return false;
  }
  return true;
}

bool expect_generate_status(
    const std::vector<FrameDesc>& frames,
    const std::vector<guint>& eos_source_ids,
    absl::StatusCode expected_code,
    const std::string& test_name,
    bool pipeline_eos = false,
    const std::vector<guint>& stream_start_source_ids = {}) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  for (guint source_id : eos_source_ids) {
    GstEvent* event = gst_nvevent_new_stream_eos(source_id);
    stitcher.HandleEvent(event);
    gst_event_unref(event);
  }
  for (guint source_id : stream_start_source_ids) {
    GstEvent* event = gst_nvevent_new_stream_start(source_id, const_cast<char*>("test-stream"));
    stitcher.HandleEvent(event);
    gst_event_unref(event);
  }
  if (pipeline_eos) {
    stitcher.outputthread_stopped = true;
    GstEvent* event = gst_event_new_eos();
    stitcher.HandleEvent(event);
    gst_event_unref(event);
  }

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(std::max<size_t>(frames.size(), 1));
  if (!batch_meta) {
    std::cerr << "Failed to create batch meta for " << test_name << std::endl;
    return false;
  }

  for (size_t i = 0; i < frames.size(); ++i) {
    NvDsFrameMeta* frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
    if (!frame_meta) {
      std::cerr << "Failed to acquire frame meta for " << test_name << std::endl;
      nvds_destroy_batch_meta(batch_meta);
      return false;
    }
    frame_meta->batch_id = i;
    frame_meta->frame_num = frames[i].frame_num;
    frame_meta->source_id = frames[i].source_id;
    frame_meta->pad_index = frames[i].source_id;
    frame_meta->num_surfaces_per_frame = 1;
    nvds_add_frame_meta_to_batch(batch_meta, frame_meta);
  }

  std::vector<NvBufSurfaceParams> input_params(std::max<size_t>(frames.size(), 1));
  NvBufSurface in_surface{};
  in_surface.batchSize = std::max<guint>(2, static_cast<guint>(((frames.size() + 1) / 2) * 2));
  in_surface.numFilled = frames.size();
  in_surface.surfaceList = input_params.data();

  std::vector<NvBufSurfaceParams> output_params(std::max<size_t>(frames.size() / 2, 1));
  NvBufSurface out_surface{};
  out_surface.batchSize = std::max<guint>(1, in_surface.batchSize / 2);
  out_surface.surfaceList = output_params.data();

  const absl::Status status = stitcher.GenerateOutput(batch_meta, &in_surface, &out_surface);
  nvds_destroy_batch_meta(batch_meta);

  if (status.code() != expected_code) {
    std::cerr << "Unexpected GenerateOutput status for " << test_name << ": " << status
              << ", expected code: " << static_cast<int>(expected_code) << std::endl;
    return false;
  }
  return true;
}

} // namespace

int main() {
  gst_init(nullptr, nullptr);

  fs::path tmpdir = fs::temp_directory_path() / "hmstitcher_onepass_test";
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);

  const std::string config_dir = tmpdir.string();
  if (!run_precaps(config_dir, /*one_pass_mode=*/false, /*expect_ok=*/false, 0, 0)) {
    return 1;
  }
  if (!run_precaps(config_dir, /*one_pass_mode=*/true, /*expect_ok=*/true, 0, 0)) {
    return 2;
  }
  if (!expect_output_batch_size(/*input_batch_size=*/2, /*configured_batch_size=*/4, /*expected_batch_size=*/1)) {
    return 3;
  }
  if (!expect_output_batch_size(/*input_batch_size=*/4, /*configured_batch_size=*/4, /*expected_batch_size=*/2)) {
    return 4;
  }
  if (!expect_generate_status({{0, 0}}, {}, absl::StatusCode::kFailedPrecondition, "odd batch without eos")) {
    return 5;
  }
  if (!expect_generate_status({{0, 0}}, {1}, absl::StatusCode::kCancelled, "odd batch after source eos")) {
    return 6;
  }
  if (!expect_generate_status(
          {{0, 0}},
          {1},
          absl::StatusCode::kFailedPrecondition,
          "odd batch after cleared source eos",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{1})) {
    return 7;
  }
  if (!expect_generate_status(
          {{0, 0}},
          {},
          absl::StatusCode::kCancelled,
          "odd batch after pipeline eos",
          /*pipeline_eos=*/true)) {
    return 8;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 0}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "duplicate frame/source without eos")) {
    return 9;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 0}},
          {1},
          absl::StatusCode::kFailedPrecondition,
          "duplicate after source eos")) {
    return 10;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 1}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "even unpaired frames without eos")) {
    return 11;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 1}},
          {1},
          absl::StatusCode::kFailedPrecondition,
          "even unpaired after source eos")) {
    return 12;
  }

  return 0;
}
