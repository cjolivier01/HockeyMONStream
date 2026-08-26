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

bool expect_high_bit_property_contract(const std::string& config_dir) {
  hm::stitcher::StitcherPriv high_bit_then_show(/*gpu_id=*/0, /*batch_size=*/2);
  hm::stitcher::StitcherPriv show_then_high_bit(/*gpu_id=*/0, /*batch_size=*/2);
  if (!high_bit_then_show.SetProperty({"high-bit-depth", "1"}) || high_bit_then_show.SetProperty({"show", "1"}) ||
      !high_bit_then_show.SetProperty({"show", "0"}) || !show_then_high_bit.SetProperty({"show", "1"}) ||
      show_then_high_bit.SetProperty({"high-bit-depth", "1"})) {
    std::cerr << "High-bit mode and stitcher debug rendering must be rejected in either property order\n";
    return false;
  }

  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  if (stitcher.SetProperty({"high-bit-depth", "yes"}) || stitcher.SetProperty({"high-bit-depth", "2"}) ||
      !stitcher.SetProperty({"high-bit-depth", "TRUE"}) || stitcher.SetProperty({"stitch-compute-precision", "fp32"}) ||
      !stitcher.SetProperty({"stitch-compute-precision", "fp16"}) || !stitcher.SetProperty({"shadow-lift", "0"}) ||
      !stitcher.SetProperty({"shadow-lift", "100"}) || stitcher.SetProperty({"shadow-lift", "-1"}) ||
      stitcher.SetProperty({"shadow-lift", "101"}) || stitcher.SetProperty({"shadow-lift", "nan"}) ||
      !stitcher.SetProperty({"shadow-lift-black-point", "1"}) ||
      !stitcher.SetProperty({"shadow-lift-black-point", "false"}) ||
      stitcher.SetProperty({"shadow-lift-black-point", "yes"}) || !stitcher.SetProperty({"exposure", "0"}) ||
      !stitcher.SetProperty({"exposure", "1.3"}) || stitcher.SetProperty({"exposure", "-0.01"}) ||
      stitcher.SetProperty({"exposure", "1.31"}) || stitcher.SetProperty({"exposure", "inf"}) ||
      !stitcher.SetProperty({"stitched-output-epoch", "16:authorization-b221"}) ||
      stitcher.SetProperty({"stitched-output-epoch", "authorization-b2:21"}) ||
      !stitcher.SetProperty({"one-pass-mode", "1"})) {
    std::cerr << "High-bit stitcher property validation failed\n";
    return false;
  }

  hm::DSCustom_CreateParams params{};
  params.config_file = const_cast<char*>(config_dir.c_str());
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGB10A2_LE,width=1280,height=720");
  const absl::Status status = stitcher.PreCapsInit(&params);
  gst_caps_unref(params.m_inCaps);
  if (!status.ok() || stitcher.SetProperty({"high-bit-depth", "false"}) ||
      !stitcher.SetProperty({"high-bit-depth", "true"})) {
    std::cerr << "High-bit mode must be immutable after caps initialization: " << status << '\n';
    return false;
  }
  return true;
}

bool expect_runtime_pair_contract() {
  const auto initial_pair = hm::stitcher::select_runtime_stitch_pair({{0, 0}, {0, 1}});
  if (!initial_pair.ok()) {
    std::cerr << "Expected the exact initial camera pair to be accepted for runtime sizing: " << initial_pair.status()
              << std::endl;
    return false;
  }

  const auto partial = hm::stitcher::select_runtime_stitch_pair({{4, 0}});
  if (partial.status().code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Expected a partial runtime-sizing batch to fail instead of being discarded, got " << partial.status()
              << std::endl;
    return false;
  }

  const auto observed_source_eos = hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {0}, /*pipeline_eos_seen=*/false);
  const auto missing_source_eos = hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {1}, /*pipeline_eos_seen=*/false);
  if (observed_source_eos.status().code() != absl::StatusCode::kCancelled ||
      missing_source_eos.status().code() != absl::StatusCode::kCancelled) {
    std::cerr << "Expected either source's permanent EOS to end stitched output without continuing past a partial; "
              << "observed-source status=" << observed_source_eos.status()
              << ", missing-source status=" << missing_source_eos.status() << std::endl;
    return false;
  }

  const auto even_tail_after_source_eos =
      hm::stitcher::select_runtime_stitch_pair({{4, 0}, {5, 0}}, {1}, /*pipeline_eos_seen=*/false);
  const auto even_tail_after_pipeline_eos =
      hm::stitcher::select_runtime_stitch_pair({{4, 0}, {5, 0}}, {}, /*pipeline_eos_seen=*/true);
  const auto duplicate_tail =
      hm::stitcher::select_runtime_stitch_pair({{4, 0}, {4, 0}}, {1}, /*pipeline_eos_seen=*/false);
  if (even_tail_after_source_eos.status().code() != absl::StatusCode::kCancelled ||
      even_tail_after_pipeline_eos.status().code() != absl::StatusCode::kCancelled ||
      duplicate_tail.status().code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Expected metadata-valid even tails to be terminal after source or pipeline EOS while duplicate "
                 "frames remain errors; source status="
              << even_tail_after_source_eos.status() << ", pipeline status=" << even_tail_after_pipeline_eos.status()
              << ", duplicate status=" << duplicate_tail.status() << std::endl;
    return false;
  }

  const auto combined_eos = hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {0}, /*pipeline_eos_seen=*/true);
  if (combined_eos.status().code() != absl::StatusCode::kCancelled) {
    std::cerr << "Expected pipeline EOS to cancel a valid partial even after its observed source reached EOS, got "
              << combined_eos.status() << std::endl;
    return false;
  }

  const auto offset_complete = hm::stitcher::select_runtime_stitch_pair({{4, 0}, {3, 1}});
  const auto skipped_initial = hm::stitcher::select_runtime_stitch_pair({{4, 0}, {4, 1}});
  const auto skipped_initial_for_calibration = hm::stitcher::select_runtime_stitch_pair(
      {{4, 0}, {4, 1}},
      {},
      /*pipeline_eos_seen=*/false,
      /*require_initial_continuity=*/false);
  if (offset_complete.status().code() != absl::StatusCode::kFailedPrecondition ||
      skipped_initial.status().code() != absl::StatusCode::kFailedPrecondition ||
      !skipped_initial_for_calibration.ok()) {
    std::cerr << "Expected mismatched or skipped camera counters to fail before runtime sizing; mismatch="
              << offset_complete.status() << ", skipped=" << skipped_initial.status()
              << ", calibration skipped=" << skipped_initial_for_calibration.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_lossless_frame_continuity_contract() {
  const auto first_batch = hm::stitcher::validate_stitch_frame_continuity({0, 1});
  if (!first_batch.ok() || *first_batch != 1) {
    std::cerr << "Expected the initial consecutive stitched frames to be accepted: " << first_batch.status()
              << std::endl;
    return false;
  }
  const auto next_batch = hm::stitcher::validate_stitch_frame_continuity({2, 3}, *first_batch);
  if (!next_batch.ok() || *next_batch != 3) {
    std::cerr << "Expected consecutive stitched frames across batches to be accepted: " << next_batch.status()
              << std::endl;
    return false;
  }
  const auto missing_initial = hm::stitcher::validate_stitch_frame_continuity({1});
  const auto skipped = hm::stitcher::validate_stitch_frame_continuity({2, 4}, 1);
  const auto reset = hm::stitcher::validate_stitch_frame_continuity({0}, 14);
  if (missing_initial.status().code() != absl::StatusCode::kFailedPrecondition ||
      skipped.status().code() != absl::StatusCode::kFailedPrecondition ||
      reset.status().code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Expected initial gaps, skipped frames, and chapter counter resets to fail; initial="
              << missing_initial.status() << ", skipped=" << skipped.status() << ", reset=" << reset.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_output_capacity_is_stable_across_batches() {
  std::vector<NvBufSurfaceParams> output_params(2);
  NvBufSurface output_surface{};
  output_surface.batchSize = 2;
  output_surface.numFilled = 2;
  output_surface.surfaceList = output_params.data();

  absl::Status first_status = hm::stitcher::prepare_stitch_output_surface(&output_surface, /*planned_frames=*/1);
  if (!first_status.ok() || output_surface.batchSize != 2 || output_surface.numFilled != 0) {
    std::cerr << "Expected a one-frame batch to preserve output capacity 2: " << first_status << std::endl;
    return false;
  }

  output_surface.numFilled = 1;
  absl::Status second_status = hm::stitcher::prepare_stitch_output_surface(&output_surface, /*planned_frames=*/2);
  if (!second_status.ok() || output_surface.batchSize != 2 || output_surface.numFilled != 0) {
    std::cerr << "Expected a later two-frame batch to reuse output capacity 2: " << second_status << std::endl;
    return false;
  }

  const absl::Status overflow_status =
      hm::stitcher::prepare_stitch_output_surface(&output_surface, /*planned_frames=*/3);
  if (overflow_status.code() != absl::StatusCode::kFailedPrecondition || output_surface.batchSize != 2) {
    std::cerr << "Expected output overflow to fail without changing capacity: " << overflow_status << std::endl;
    return false;
  }
  return true;
}

bool expect_resumed_calibration_progress_contract() {
  if (!hm::stitcher::should_attempt_one_pass_field_mask(false, {}, "generation-a") ||
      hm::stitcher::should_attempt_one_pass_field_mask(true, "generation-a", "generation-a") ||
      !hm::stitcher::should_attempt_one_pass_field_mask(true, "generation-a", "generation-b")) {
    std::cerr << "Superseded one-pass field-mask inference must retry only after output generation changes"
              << std::endl;
    return false;
  }
  g_unsetenv("HSTREAM_CALIBRATION_PENDING");
  const auto normal_existing = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false, /*mask_configured=*/true);
  if (normal_existing.report || normal_existing.create_mask || normal_existing.complete) {
    std::cerr << "Ordinary playback should not emit calibration completion for cached mappings" << std::endl;
    return false;
  }
  const auto normal_missing = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false, /*mask_configured=*/false);
  if (!normal_missing.report || !normal_missing.create_mask || normal_missing.complete) {
    std::cerr << "Ordinary playback should report a missing rink mask as active calibration" << std::endl;
    return false;
  }
  const auto normal_after_creation = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false,
      /*mask_configured=*/true,
      /*report_latched=*/normal_missing.report);
  if (!normal_after_creation.report || normal_after_creation.create_mask || !normal_after_creation.complete) {
    std::cerr << "Ordinary playback should complete the progress sequence after creating its missing rink mask"
              << std::endl;
    return false;
  }

  g_setenv("HSTREAM_CALIBRATION_PENDING", "1", TRUE);
  const auto resumed_existing = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false, /*mask_configured=*/true);
  const auto resumed_missing = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false, /*mask_configured=*/false);
  const auto resumed_after_creation = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false, /*mask_configured=*/true);
  const auto recreated_after_completion = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/false,
      /*mask_configured=*/true,
      /*report_latched=*/false,
      /*process_completion_latched=*/true);
  g_unsetenv("HSTREAM_CALIBRATION_PENDING");
  if (!resumed_existing.report || resumed_existing.create_mask || !resumed_existing.complete ||
      !resumed_missing.report || !resumed_missing.create_mask || resumed_missing.complete ||
      !resumed_after_creation.complete || recreated_after_completion.report || recreated_after_completion.create_mask ||
      recreated_after_completion.complete) {
    std::cerr << "Resumed calibration should complete with an existing mask or create and then complete a missing mask"
              << std::endl;
    return false;
  }

  const auto configured_missing = hm::stitcher::one_pass_calibration_progress_plan(
      /*configured_during_run=*/true, /*mask_configured=*/false);
  if (!configured_missing.report || !configured_missing.create_mask || configured_missing.complete) {
    std::cerr << "New one-pass configuration should retain its calibration progress contract" << std::endl;
    return false;
  }
  hm::stitcher::OnePassCalibrationCompletionLatch completion_latch;
  const std::string first_scope = "output-a/invalidation-a/run-1";
  const std::string second_scope = "output-b/invalidation-a/run-2";
  if (!completion_latch.try_begin_delivery(first_scope) || completion_latch.delivered(first_scope)) {
    std::cerr << "Completion latch should expose an in-flight delivery without marking it delivered" << std::endl;
    return false;
  }
  completion_latch.finish_delivery(first_scope, /*delivered=*/false);
  if (completion_latch.delivered(first_scope) || !completion_latch.try_begin_delivery(first_scope)) {
    std::cerr << "A failed bus post should release completion ownership for a recreated stitcher" << std::endl;
    return false;
  }
  completion_latch.finish_delivery(first_scope, /*delivered=*/true);
  if (!completion_latch.delivered(first_scope) || completion_latch.try_begin_delivery(first_scope)) {
    std::cerr << "A delivered completion should suppress duplicate recreated-stitcher messages" << std::endl;
    return false;
  }
  if (completion_latch.delivered(second_scope) || !completion_latch.try_begin_delivery(second_scope)) {
    std::cerr << "A later output/invalidation/run generation should have independent completion ownership" << std::endl;
    return false;
  }
  completion_latch.finish_delivery(second_scope, /*delivered=*/true);
  return true;
}

bool expect_validated_artifact_load_failure_retries_once() {
  if (!hm::stitcher::should_defer_validated_artifact_load_failure(
          /*one_pass_mode=*/true, /*retry_already_failed=*/false) ||
      hm::stitcher::should_defer_validated_artifact_load_failure(
          /*one_pass_mode=*/true, /*retry_already_failed=*/true) ||
      hm::stitcher::should_defer_validated_artifact_load_failure(
          /*one_pass_mode=*/false, /*retry_already_failed=*/false)) {
    std::cerr << "Validated artifact loading should defer exactly once for an immediate one-pass retry" << std::endl;
    return false;
  }
  return true;
}

bool expect_prepare_runtime_partial_fails() {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  stitcher.SetProperty({"one-pass-mode", "1"});

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
  frame_meta->frame_num = 4;
  frame_meta->source_id = 0;
  frame_meta->num_surfaces_per_frame = 1;
  nvds_add_frame_meta_to_batch(batch_meta, frame_meta);

  NvBufSurfaceParams input_param{};
  NvBufSurface in_surface{};
  in_surface.batchSize = 2;
  in_surface.numFilled = 1;
  in_surface.surfaceList = &input_param;
  const auto result = stitcher.PrepareRuntimeOutputSize(batch_meta, &in_surface);
  nvds_destroy_batch_meta(batch_meta);
  if (result.status().code() != absl::StatusCode::kFailedPrecondition) {
    std::cerr << "Expected PrepareRuntimeOutputSize to reject a partial first batch, got " << result.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_prepare_runtime_invalid_envelopes_fail() {
  auto expect_failed = [](guint batch_size, guint num_filled, const std::string& label) {
    hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
    stitcher.SetProperty({"one-pass-mode", "1"});

    NvDsBatchMeta* batch_meta = nvds_create_batch_meta(std::max<guint>(num_filled, 1));
    for (guint index = 0; index < num_filled; ++index) {
      NvDsFrameMeta* frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta);
      frame_meta->frame_num = index / 2;
      frame_meta->source_id = index % 2;
      frame_meta->num_surfaces_per_frame = 1;
      nvds_add_frame_meta_to_batch(batch_meta, frame_meta);
    }

    std::vector<NvBufSurfaceParams> input_params(std::max<guint>(num_filled, 1));
    NvBufSurface in_surface{};
    in_surface.batchSize = batch_size;
    in_surface.numFilled = num_filled;
    in_surface.surfaceList = input_params.data();
    const auto result = stitcher.PrepareRuntimeOutputSize(batch_meta, &in_surface);
    nvds_destroy_batch_meta(batch_meta);
    if (result.status().code() != absl::StatusCode::kFailedPrecondition) {
      std::cerr << "Expected runtime sizing to reject " << label << ", got " << result.status() << std::endl;
      return false;
    }
    return true;
  };

  return expect_failed(/*batch_size=*/3, /*num_filled=*/1, "odd configured batch size") &&
      expect_failed(/*batch_size=*/2, /*num_filled=*/3, "numFilled greater than batchSize");
}

bool expect_generated_output_eos_is_terminal() {
  hm::videoprep::RuntimeOutputPoolFlow output_flow;
  int output_releases = 0;
  int later_input_releases = 0;
  int eos_events = 0;
  GstBuffer* output_buffer = gst_buffer_new();
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(output_buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<int*>(user_data); },
      &output_releases);
  output_flow.finish_with_eos(output_buffer, [&eos_events]() { ++eos_events; });

  GstBuffer* later_input_buffer = gst_buffer_new();
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(later_input_buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<int*>(user_data); },
      &later_input_releases);
  const bool later_consumed = output_flow.consume_if_terminal(later_input_buffer);
  if (!output_flow.eos_terminal() || output_releases != 1 || !later_consumed || later_input_releases != 1 ||
      eos_events != 1) {
    if (!later_consumed) {
      gst_buffer_unref(later_input_buffer);
    }
    std::cerr << "Expected GenerateOutput cancellation to release its output, send EOS once, and consume later input"
              << std::endl;
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
    const std::vector<guint>& stream_start_source_ids = {},
    gint input_batch_size = -1,
    bool require_decoded_sequence_meta = false) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  if (require_decoded_sequence_meta) {
    stitcher.SetProperty({"require-decoded-frame-sequence-meta", "1"});
  }
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
  in_surface.batchSize = input_batch_size >= 0 ? static_cast<guint>(input_batch_size)
                                               : std::max<guint>(2, static_cast<guint>(((frames.size() + 1) / 2) * 2));
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
  if (!expect_high_bit_property_contract(config_dir)) {
    return 34;
  }
  if (!expect_runtime_pair_contract()) {
    return 13;
  }
  if (!expect_lossless_frame_continuity_contract()) {
    return 28;
  }
  if (!expect_output_capacity_is_stable_across_batches()) {
    return 29;
  }
  if (!expect_resumed_calibration_progress_contract()) {
    return 33;
  }
  if (!expect_validated_artifact_load_failure_retries_once()) {
    return 35;
  }
  if (!expect_prepare_runtime_partial_fails()) {
    return 14;
  }
  if (!expect_prepare_runtime_invalid_envelopes_fail()) {
    return 27;
  }
  if (!expect_generated_output_eos_is_terminal()) {
    return 19;
  }
  if (!expect_generate_status({{0, 0}}, {}, absl::StatusCode::kFailedPrecondition, "odd batch without eos")) {
    return 5;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {1, 0}}, {}, absl::StatusCode::kFailedPrecondition, "three-frame partial without eos")) {
    return 17;
  }
  if (!expect_generate_status({{0, 0}}, {1}, absl::StatusCode::kCancelled, "odd batch after missing source eos")) {
    return 6;
  }
  if (!expect_generate_status({{0, 0}}, {0}, absl::StatusCode::kCancelled, "odd batch after observed source eos")) {
    return 22;
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
          {{0, 0}},
          {0},
          absl::StatusCode::kCancelled,
          "odd batch after observed source and pipeline eos",
          /*pipeline_eos=*/true)) {
    return 20;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 0}},
          {1},
          absl::StatusCode::kCancelled,
          "batch-size-4 even tail after missing source eos",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/4)) {
    return 23;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 0}},
          {0},
          absl::StatusCode::kCancelled,
          "batch-size-4 even tail after observed source eos",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/4)) {
    return 24;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 0}},
          {},
          absl::StatusCode::kCancelled,
          "batch-size-4 even tail after pipeline eos",
          /*pipeline_eos=*/true,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/4)) {
    return 25;
  }
  if (!expect_generate_status(
          {{0, 0}, {1, 0}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "batch-size-4 even single-source batch without eos",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/4)) {
    return 26;
  }
  if (!expect_generate_status(
          {{0, 0}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "odd configured batch envelope",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/3)) {
    return 28;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {1, 0}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "numFilled greater than batchSize",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/2)) {
    return 29;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "zero batchSize with a balanced pair",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/0)) {
    return 30;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "balanced pairs with numFilled greater than batchSize",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/2)) {
    return 31;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "mandatory decoded sequence metadata completely missing",
          /*pipeline_eos=*/false,
          /*stream_start_source_ids=*/{},
          /*input_batch_size=*/2,
          /*require_decoded_sequence_meta=*/true)) {
    return 32;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 0}}, {}, absl::StatusCode::kFailedPrecondition, "duplicate frame/source without eos")) {
    return 9;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 0}}, {1}, absl::StatusCode::kFailedPrecondition, "duplicate after source eos")) {
    return 10;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {0, 2}, {1, 3}},
          {},
          absl::StatusCode::kFailedPrecondition,
          "unbalanced source frames without eos")) {
    return 11;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {0, 2}, {1, 3}},
          {1},
          absl::StatusCode::kFailedPrecondition,
          "unbalanced source frames after source eos")) {
    return 12;
  }

  return 0;
}
