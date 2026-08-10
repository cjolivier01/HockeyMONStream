#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

#include "absl/status/status.h"
#include "gst-nvevent.h"
#include "nvdsmeta.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

namespace fs = std::filesystem;

namespace {

struct FrameDesc {
  guint frame_num;
  guint source_id;
};

class OutputThreadProbeStitcher final : public hm::stitcher::StitcherPriv {
 public:
  OutputThreadProbeStitcher() : StitcherPriv(/*gpu_id=*/0, /*batch_size=*/2) {}

  absl::StatusOr<hm::videoprep::RuntimeOutputSize> PrepareRuntimeOutputSize(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface) override {
    ++runtime_size_calls;
    return StitcherPriv::PrepareRuntimeOutputSize(batch_meta, in_surface);
  }

  std::atomic<int> runtime_size_calls{0};
};

bool wait_until(const std::function<bool()>& condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return condition();
}

GstBuffer* make_partial_input_buffer(
    guint frame_num,
    guint source_id,
    NvBufSurfaceParams* surface_params,
    std::atomic<int>* released_inputs) {
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, sizeof(NvBufSurface), nullptr);
  if (!buffer) {
    return nullptr;
  }

  GstMapInfo map = GST_MAP_INFO_INIT;
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    return nullptr;
  }
  auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
  *surface = NvBufSurface{};
  surface->batchSize = 2;
  surface->numFilled = 1;
  surface->surfaceList = surface_params;
  gst_buffer_unmap(buffer, &map);

  NvDsBatchMeta* batch_meta = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame_meta = batch_meta ? nvds_acquire_frame_meta_from_pool(batch_meta) : nullptr;
  if (!batch_meta || !frame_meta) {
    if (batch_meta) {
      nvds_destroy_batch_meta(batch_meta);
    }
    gst_buffer_unref(buffer);
    return nullptr;
  }
  frame_meta->batch_id = 0;
  frame_meta->frame_num = frame_num;
  frame_meta->source_id = source_id;
  frame_meta->pad_index = source_id;
  frame_meta->num_surfaces_per_frame = 1;
  nvds_add_frame_meta_to_batch(batch_meta, frame_meta);

  NvDsMeta* meta =
      gst_buffer_add_nvds_meta(buffer, batch_meta, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  if (!meta) {
    nvds_destroy_batch_meta(batch_meta);
    gst_buffer_unref(buffer);
    return nullptr;
  }
  meta->meta_type = NVDS_BATCH_GST_META;
  batch_meta->base_meta.batch_meta = batch_meta;
  batch_meta->base_meta.copy_func = nvds_batch_meta_copy_func;
  batch_meta->base_meta.release_func = nvds_batch_meta_release_func;
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<std::atomic<int>*>(user_data); },
      released_inputs);
  return buffer;
}

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

bool expect_runtime_pair_recovery() {
  const auto partial = hm::stitcher::select_runtime_stitch_pair({{4, 0}});
  if (partial.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected the first partial runtime-sizing batch to be retryable, got " << partial.status()
              << std::endl;
    return false;
  }

  const auto observed_source_eos =
      hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {0}, /*pipeline_eos_seen=*/false);
  const auto missing_source_eos =
      hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {1}, /*pipeline_eos_seen=*/false);
  if (observed_source_eos.status().code() != absl::StatusCode::kUnavailable ||
      missing_source_eos.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected either source's local EOS to discard the partial without ending the output pipeline; "
              << "observed-source status=" << observed_source_eos.status()
              << ", missing-source status=" << missing_source_eos.status() << std::endl;
    return false;
  }

  const auto combined_eos = hm::stitcher::select_runtime_stitch_pair({{4, 0}}, {0}, /*pipeline_eos_seen=*/true);
  if (combined_eos.status().code() != absl::StatusCode::kCancelled) {
    std::cerr << "Expected pipeline EOS to cancel a valid partial even after its observed source reached EOS, got "
              << combined_eos.status() << std::endl;
    return false;
  }

  const auto offset_complete = hm::stitcher::select_runtime_stitch_pair({{4, 0}, {3, 1}});
  if (!offset_complete.ok() || offset_complete->first != 0 || offset_complete->second != 1) {
    std::cerr << "Expected the next offset-but-synchronized batch to produce a runtime sizing pair" << std::endl;
    return false;
  }
  return true;
}

bool expect_prepare_runtime_partial_is_retryable() {
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
  if (result.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected PrepareRuntimeOutputSize to retry a partial first batch, got " << result.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_enqueued_partial_preserves_event_order(bool pipeline_eos) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  stitcher.SetProperty({"one-pass-mode", "1"});
  stitcher.outputthread_stopped = true;

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
  const auto partial_result = stitcher.PrepareRuntimeOutputSize(batch_meta, &in_surface);
  if (partial_result.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected runtime sizing to wait before EOS, got " << partial_result.status() << std::endl;
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  hm::videoprep::RuntimeOutputPoolFlow output_pool_flow;
  GstBuffer* retry_input_buffer = gst_buffer_new();
  int retry_released_inputs = 0;
  int retry_eos_events = 0;
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(retry_input_buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<int*>(user_data); },
      &retry_released_inputs);
  const bool retry_handled = output_pool_flow.handle_status(
      partial_result.status(), retry_input_buffer, [&retry_eos_events]() { ++retry_eos_events; });
  if (!retry_handled || retry_released_inputs != 1 || retry_eos_events != 0) {
    if (!retry_handled) {
      gst_buffer_unref(retry_input_buffer);
    }
    std::cerr << "Expected unavailable runtime sizing to release its input without sending EOS" << std::endl;
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }

  // Reproduce production ordering: ProcessBuffer snapshots state when the surface is enqueued, then the serialized
  // event arrives before OutputThread sizes it. The later event must not be applied retroactively to this buffer.
  GstBuffer* queued_input_buffer = gst_buffer_new_allocate(nullptr, sizeof(NvBufSurface), nullptr);
  GstMapInfo queued_write_map = GST_MAP_INFO_INIT;
  if (!queued_input_buffer || !gst_buffer_map(queued_input_buffer, &queued_write_map, GST_MAP_WRITE)) {
    std::cerr << "Could not allocate the queued runtime-sizing test surface" << std::endl;
    if (queued_input_buffer) {
      gst_buffer_unref(queued_input_buffer);
    }
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  *reinterpret_cast<NvBufSurface*>(queued_write_map.data) = in_surface;
  gst_buffer_unmap(queued_input_buffer, &queued_write_map);
  if (stitcher.ProcessBuffer(queued_input_buffer) != hm::BufferResult::Buffer_Async) {
    std::cerr << "Could not enqueue the runtime-sizing test surface" << std::endl;
    gst_buffer_unref(queued_input_buffer);
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }

  if (pipeline_eos) {
    GstEvent* event = gst_event_new_eos();
    stitcher.HandleEvent(event);
    gst_event_unref(event);
  } else {
    GstEvent* event = gst_nvevent_new_stream_eos(/*source_id=*/1);
    stitcher.HandleEvent(event);
    gst_event_unref(event);
  }

  GstMapInfo queued_read_map = GST_MAP_INFO_INIT;
  if (!gst_buffer_map(queued_input_buffer, &queued_read_map, GST_MAP_READ)) {
    std::cerr << "Could not remap the queued runtime-sizing test surface" << std::endl;
    gst_buffer_unref(queued_input_buffer);
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  const auto eos_result =
      stitcher.PrepareRuntimeOutputSize(batch_meta, reinterpret_cast<NvBufSurface*>(queued_read_map.data));
  gst_buffer_unmap(queued_input_buffer, &queued_read_map);
  gst_buffer_unref(queued_input_buffer);
  nvds_destroy_batch_meta(batch_meta);
  if (eos_result.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected a partial queued before " << (pipeline_eos ? "pipeline" : "source-local")
              << " EOS to retain its pre-event nonterminal state, got " << eos_result.status() << std::endl;
    return false;
  }
  GstBuffer* input_buffer = gst_buffer_new();
  int released_inputs = 0;
  int downstream_eos_events = 0;
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(input_buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<int*>(user_data); },
      &released_inputs);
  const bool handled = output_pool_flow.handle_status(
      eos_result.status(), input_buffer, [&downstream_eos_events]() { ++downstream_eos_events; });
  if (!handled || released_inputs != 1 || downstream_eos_events != 0 || output_pool_flow.eos_terminal()) {
    if (!handled) {
      gst_buffer_unref(input_buffer);
    }
    std::cerr << "Expected runtime output-pool sizing to release its input and reserve downstream EOS for global "
                 "pipeline termination; handled="
              << handled << ", released_inputs=" << released_inputs
              << ", downstream_eos_events=" << downstream_eos_events << std::endl;
    return false;
  }
  GstBuffer* later_input_buffer = gst_buffer_new();
  int later_released_inputs = 0;
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(later_input_buffer),
      [](gpointer user_data, GstMiniObject*) { ++*static_cast<int*>(user_data); },
      &later_released_inputs);
  const bool terminal_consumed = output_pool_flow.consume_if_terminal(later_input_buffer);
  const bool terminal_behavior_ok = !terminal_consumed && later_released_inputs == 0 && downstream_eos_events == 0;
  if (!terminal_consumed) {
    gst_buffer_unref(later_input_buffer);
  }
  if (!terminal_behavior_ok) {
    std::cerr << "A later event must not terminalize output while pre-event queued buffers are draining" << std::endl;
    return false;
  }
  return true;
}

bool expect_enqueued_partial_restart_is_retryable() {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  stitcher.SetProperty({"one-pass-mode", "1"});
  stitcher.outputthread_stopped = true;

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
  GstBuffer* queued_input_buffer = gst_buffer_new_allocate(nullptr, sizeof(NvBufSurface), nullptr);
  GstMapInfo write_map = GST_MAP_INFO_INIT;
  if (!queued_input_buffer || !gst_buffer_map(queued_input_buffer, &write_map, GST_MAP_WRITE)) {
    if (queued_input_buffer) {
      gst_buffer_unref(queued_input_buffer);
    }
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  *reinterpret_cast<NvBufSurface*>(write_map.data) = in_surface;
  gst_buffer_unmap(queued_input_buffer, &write_map);

  GstEvent* eos_event = gst_nvevent_new_stream_eos(/*source_id=*/1);
  stitcher.HandleEvent(eos_event);
  gst_event_unref(eos_event);
  if (stitcher.ProcessBuffer(queued_input_buffer) != hm::BufferResult::Buffer_Async) {
    gst_buffer_unref(queued_input_buffer);
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  GstEvent* restart_event = gst_nvevent_new_stream_start(/*source_id=*/1, const_cast<char*>("restarted-stream"));
  stitcher.HandleEvent(restart_event);
  gst_event_unref(restart_event);

  GstMapInfo read_map = GST_MAP_INFO_INIT;
  if (!gst_buffer_map(queued_input_buffer, &read_map, GST_MAP_READ)) {
    gst_buffer_unref(queued_input_buffer);
    nvds_destroy_batch_meta(batch_meta);
    return false;
  }
  const auto result = stitcher.PrepareRuntimeOutputSize(batch_meta, reinterpret_cast<NvBufSurface*>(read_map.data));
  gst_buffer_unmap(queued_input_buffer, &read_map);
  gst_buffer_unref(queued_input_buffer);
  nvds_destroy_batch_meta(batch_meta);
  if (result.status().code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected a source restart to clear the enqueued surface's stale EOS snapshot, got " << result.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_output_thread_survives_source_restart() {
  cudaStream_t stream = nullptr;
  const cudaError_t cuda_error = cudaStreamCreate(&stream);
  if (cuda_error != cudaSuccess || !stream) {
    std::cerr << "Could not create CUDA stream for OutputThread restart test: " << cudaGetErrorString(cuda_error)
              << std::endl;
    return false;
  }

  GstElement* identity = gst_element_factory_make("identity", "stitcher-output-thread-test");
  GstCaps* caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=RGBA,width=1280,height=720,framerate=30/1,batch-size=2");
  bool ok = identity && caps;
  if (!ok) {
    std::cerr << "Could not create GStreamer fixtures for OutputThread restart test" << std::endl;
  } else {
    OutputThreadProbeStitcher stitcher;
    NvBufSurfaceParams first_surface_params{};
    NvBufSurfaceParams second_surface_params{};
    std::atomic<int> first_released{0};
    std::atomic<int> second_released{0};
    stitcher.SetProperty({"one-pass-mode", "1"});
    stitcher.m_transformMode = true;

    hm::DSCustom_CreateParams params{};
    params.m_element = GST_BASE_TRANSFORM(identity);
    params.m_gpuId = 0;
    params.m_cudaStream = stream;
    params.m_inCaps = caps;
    params.m_outCaps = caps;
    params.m_bufferPoolConfig.max_buffers = 4;
    params.m_bufferPoolConfig.batch_size = 2;
    params.m_bufferPoolConfig.gpu_id = 0;
    const absl::Status init_status = stitcher.PostCapsInit(&params);
    if (!init_status.ok()) {
      std::cerr << "Could not start real stitcher OutputThread: " << init_status << std::endl;
      ok = false;
    } else {
      GstEvent* eos_event = gst_nvevent_new_stream_eos(/*source_id=*/1);
      stitcher.HandleEvent(eos_event);
      gst_event_unref(eos_event);

      GstBuffer* first =
          make_partial_input_buffer(/*frame_num=*/4, /*source_id=*/0, &first_surface_params, &first_released);
      if (!first || stitcher.ProcessBuffer(first) != hm::BufferResult::Buffer_Async) {
        if (first) {
          gst_buffer_unref(first);
        }
        std::cerr << "Could not enqueue source-EOS partial batch on real OutputThread" << std::endl;
        ok = false;
      } else if (!wait_until([&]() { return stitcher.runtime_size_calls.load() >= 1 && first_released.load() == 1; })) {
        std::cerr << "Real OutputThread did not discard the source-EOS partial batch" << std::endl;
        ok = false;
      }

      if (ok) {
        GstEvent* restart_event =
            gst_nvevent_new_stream_start(/*source_id=*/1, const_cast<char*>("restarted-stream"));
        stitcher.HandleEvent(restart_event);
        gst_event_unref(restart_event);

        GstBuffer* second =
            make_partial_input_buffer(/*frame_num=*/5, /*source_id=*/0, &second_surface_params, &second_released);
        if (!second || stitcher.ProcessBuffer(second) != hm::BufferResult::Buffer_Async) {
          if (second) {
            gst_buffer_unref(second);
          }
          std::cerr << "Could not enqueue post-restart batch on real OutputThread" << std::endl;
          ok = false;
        } else if (!wait_until(
                       [&]() { return stitcher.runtime_size_calls.load() >= 2 && second_released.load() == 1; })) {
          std::cerr << "Source EOS terminalized the real OutputThread before the restarted source could resume"
                    << std::endl;
          ok = false;
        }
      }
    }
    stitcher.Shutdown();
  }

  if (caps) {
    gst_caps_unref(caps);
  }
  if (identity) {
    gst_object_unref(identity);
  }
  cudaStreamDestroy(stream);
  return ok;
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
  if (!expect_runtime_pair_recovery()) {
    return 13;
  }
  if (!expect_prepare_runtime_partial_is_retryable()) {
    return 14;
  }
  if (!expect_enqueued_partial_preserves_event_order(/*pipeline_eos=*/false)) {
    return 15;
  }
  if (!expect_enqueued_partial_preserves_event_order(/*pipeline_eos=*/true)) {
    return 16;
  }
  if (!expect_enqueued_partial_restart_is_retryable()) {
    return 18;
  }
  if (!expect_output_thread_survives_source_restart()) {
    return 21;
  }
  if (!expect_generated_output_eos_is_terminal()) {
    return 19;
  }
  if (!expect_generate_status({{0, 0}}, {}, absl::StatusCode::kUnavailable, "odd batch without eos")) {
    return 5;
  }
  if (!expect_generate_status(
          {{0, 0}, {0, 1}, {1, 0}}, {}, absl::StatusCode::kUnavailable, "three-frame partial batch without eos")) {
    return 17;
  }
  if (!expect_generate_status({{0, 0}}, {1}, absl::StatusCode::kUnavailable, "odd batch after missing source eos")) {
    return 6;
  }
  if (!expect_generate_status(
          {{0, 0}}, {0}, absl::StatusCode::kUnavailable, "odd batch after observed source eos")) {
    return 22;
  }
  if (!expect_generate_status(
          {{0, 0}},
          {1},
          absl::StatusCode::kUnavailable,
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
