#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/libs/common/ApplicationPayload.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gstnvdsmeta.h>
#include <nvdsmeta.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

namespace fs = std::filesystem;

class TempDirectory {
 public:
  explicit TempDirectory(const char* stem) {
    path_ = fs::temp_directory_path() /
        (std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const {
    return path_;
  }

 private:
  fs::path path_;
};

bool run_flow_isolation_test() {
  GstElement* isolation = gst_element_factory_make("hmpreviewisolation", "test_isolation");
  GstElement* ingress = gst_element_factory_make("hmpreviewisolation", "test_ingress_isolation");
  GstPad* upstream = gst_pad_new("upstream", GST_PAD_SRC);
  GstPad* downstream = gst_pad_new("downstream", GST_PAD_SINK);
  GstPad* isolation_sink = isolation ? gst_element_get_static_pad(isolation, "sink") : nullptr;
  GstPad* isolation_src = isolation ? gst_element_get_static_pad(isolation, "src") : nullptr;
  if (!isolation || !ingress || !upstream || !downstream || !isolation_sink || !isolation_src) {
    std::cerr << "Could not construct preview flow-isolation test\n";
    return false;
  }

  int calls = 0;
  g_object_set_data(G_OBJECT(downstream), "test-calls", &calls);
  gst_pad_set_chain_function(
      downstream, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
        auto* count = static_cast<int*>(g_object_get_data(G_OBJECT(pad), "test-calls"));
        const GstFlowReturn result = (*count)++ == 0 ? GST_FLOW_FLUSHING : GST_FLOW_ERROR;
        gst_buffer_unref(buffer);
        return result;
      });
  gst_pad_set_active(upstream, TRUE);
  gst_pad_set_active(downstream, TRUE);
  gst_pad_set_active(isolation_sink, TRUE);
  gst_pad_set_active(isolation_src, TRUE);
  const bool linked = gst_pad_link(upstream, isolation_sink) == GST_PAD_LINK_OK &&
      gst_pad_link(isolation_src, downstream) == GST_PAD_LINK_OK;
  hm::gpu_preview::set_isolation_active(isolation, true, 1);
  hm::gpu_preview::set_isolation_active(ingress, true, 1);
  hm::gpu_preview::set_isolation_failure_peer(isolation, ingress);
  GstCaps* caps = gst_caps_from_string("video/x-raw,format=RGBA,width=1,height=1,framerate=1/1");
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_push_event(upstream, gst_event_new_stream_start("preview-isolation-test"));
  gst_pad_push_event(upstream, gst_event_new_caps(caps));
  gst_pad_push_event(upstream, gst_event_new_segment(&segment));
  gst_caps_unref(caps);
  const GstFlowReturn transient = linked ? gst_pad_push(upstream, gst_buffer_new()) : GST_FLOW_ERROR;
  const bool survived_flush = transient == GST_FLOW_OK && hm::gpu_preview::isolation_active(isolation);
  const GstFlowReturn failed = linked ? gst_pad_push(upstream, gst_buffer_new()) : GST_FLOW_ERROR;
  const bool contained_error = failed == GST_FLOW_OK && !hm::gpu_preview::isolation_active(isolation) &&
      !hm::gpu_preview::isolation_active(ingress);

  gst_pad_unlink(upstream, isolation_sink);
  gst_pad_unlink(isolation_src, downstream);
  gst_object_unref(isolation_sink);
  gst_object_unref(isolation_src);
  gst_object_unref(upstream);
  gst_object_unref(downstream);
  gst_object_unref(isolation);
  gst_object_unref(ingress);
  if (!survived_flush || !contained_error) {
    std::cerr << "Preview isolation did not distinguish transient flushing from a real flow error\n";
    return false;
  }
  return true;
}

bool run_deactivation_barrier_test() {
  struct Barrier {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool release{false};
  } barrier;

  GstElement* isolation = gst_element_factory_make("hmpreviewisolation", "barrier_isolation");
  GstPad* upstream = gst_pad_new("barrier_upstream", GST_PAD_SRC);
  GstPad* downstream = gst_pad_new("barrier_downstream", GST_PAD_SINK);
  GstPad* isolation_sink = isolation ? gst_element_get_static_pad(isolation, "sink") : nullptr;
  GstPad* isolation_src = isolation ? gst_element_get_static_pad(isolation, "src") : nullptr;
  if (!isolation || !upstream || !downstream || !isolation_sink || !isolation_src) {
    std::cerr << "Could not construct preview deactivation-barrier test\n";
    return false;
  }

  g_object_set_data(G_OBJECT(downstream), "barrier", &barrier);
  gst_pad_set_chain_function(
      downstream, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
        auto* state = static_cast<Barrier*>(g_object_get_data(G_OBJECT(pad), "barrier"));
        std::unique_lock<std::mutex> lock(state->mutex);
        state->entered = true;
        state->condition.notify_all();
        state->condition.wait(lock, [state] { return state->release; });
        lock.unlock();
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
      });
  gst_pad_set_active(upstream, TRUE);
  gst_pad_set_active(downstream, TRUE);
  gst_pad_set_active(isolation_sink, TRUE);
  gst_pad_set_active(isolation_src, TRUE);
  const bool linked = gst_pad_link(upstream, isolation_sink) == GST_PAD_LINK_OK &&
      gst_pad_link(isolation_src, downstream) == GST_PAD_LINK_OK;
  hm::gpu_preview::set_isolation_active(isolation, true, 1);

  std::atomic<int> push_result{GST_FLOW_ERROR};
  std::thread producer([&] { push_result = linked ? gst_pad_push(upstream, gst_buffer_new()) : GST_FLOW_ERROR; });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(barrier.mutex);
    entered = barrier.condition.wait_for(lock, std::chrono::seconds(2), [&barrier] { return barrier.entered; });
  }
  std::atomic<bool> deactivated{false};
  std::atomic<bool> setter_entered{false};
  g_object_set_data(G_OBJECT(isolation), "hstream-preview-test-active-setter-entered", &setter_entered);
  std::thread deactivate([&] {
    hm::gpu_preview::set_isolation_active(isolation, false, 2);
    deactivated = true;
  });
  const auto setter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!setter_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < setter_deadline)
    std::this_thread::yield();
  const bool setter_attempted = setter_entered.load(std::memory_order_acquire);
  const bool waited_for_in_flight_buffer = setter_attempted && !deactivated.load();
  std::atomic<bool> generation_updated{false};
  std::thread update_generation([&] {
    hm::gpu_preview::set_isolation_generation(isolation, 3);
    generation_updated = true;
  });
  const auto generation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!generation_updated.load() && std::chrono::steady_clock::now() < generation_deadline)
    std::this_thread::yield();
  const bool generation_update_did_not_wait = generation_updated.load();
  {
    std::lock_guard<std::mutex> lock(barrier.mutex);
    barrier.release = true;
  }
  barrier.condition.notify_all();
  producer.join();
  deactivate.join();
  update_generation.join();
  const bool passed = entered && setter_attempted && waited_for_in_flight_buffer && generation_update_did_not_wait &&
      deactivated.load() && push_result.load() == GST_FLOW_OK && !hm::gpu_preview::isolation_active(isolation);

  gst_pad_unlink(upstream, isolation_sink);
  gst_pad_unlink(isolation_src, downstream);
  gst_object_unref(isolation_sink);
  gst_object_unref(isolation_src);
  gst_object_unref(upstream);
  gst_object_unref(downstream);
  gst_object_unref(isolation);
  if (!passed)
    std::cerr << "Preview generation update blocked, or deactivation returned before an in-flight buffer drained\n";
  return passed;
}

bool run_deactivation_exception_barrier_test() {
  struct Barrier {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool release{false};
  } barrier;

  GstElement* isolation = gst_element_factory_make("hmpreviewisolation", "exception_barrier_isolation");
  GstPad* upstream = gst_pad_new("exception_barrier_upstream", GST_PAD_SRC);
  GstPad* downstream = gst_pad_new("exception_barrier_downstream", GST_PAD_SINK);
  GstPad* isolation_sink = isolation ? gst_element_get_static_pad(isolation, "sink") : nullptr;
  GstPad* isolation_src = isolation ? gst_element_get_static_pad(isolation, "src") : nullptr;
  if (!isolation || !upstream || !downstream || !isolation_sink || !isolation_src) {
    std::cerr << "Could not construct preview exception-deactivation fixture\n";
    return false;
  }

  g_object_set_data(G_OBJECT(downstream), "exception-barrier", &barrier);
  gst_pad_set_chain_function(
      downstream, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
        auto* state = static_cast<Barrier*>(g_object_get_data(G_OBJECT(pad), "exception-barrier"));
        std::unique_lock<std::mutex> lock(state->mutex);
        state->entered = true;
        state->condition.notify_all();
        state->condition.wait(lock, [state] { return state->release; });
        lock.unlock();
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
      });
  gst_pad_set_active(upstream, TRUE);
  gst_pad_set_active(downstream, TRUE);
  gst_pad_set_active(isolation_sink, TRUE);
  gst_pad_set_active(isolation_src, TRUE);
  const bool linked = gst_pad_link(upstream, isolation_sink) == GST_PAD_LINK_OK &&
      gst_pad_link(isolation_src, downstream) == GST_PAD_LINK_OK;
  hm::gpu_preview::set_isolation_active(isolation, true, 1);

  std::atomic<int> push_result{GST_FLOW_ERROR};
  std::thread producer([&] { push_result = linked ? gst_pad_push(upstream, gst_buffer_new()) : GST_FLOW_ERROR; });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(barrier.mutex);
    entered = barrier.condition.wait_for(lock, std::chrono::seconds(2), [&barrier] { return barrier.entered; });
  }

  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationSetProperty,
      hm::gpu_preview::CallbackExceptionInjection::kLengthError);
  std::atomic<bool> deactivated{false};
  std::thread deactivate([&] {
    // Exercise the exact active=false callback used before renderer teardown,
    // without a preceding generation property consuming the injection.
    g_object_set(G_OBJECT(isolation), "active", FALSE, nullptr);
    deactivated = true;
  });
  const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (hm::gpu_preview::isolation_active(isolation) && std::chrono::steady_clock::now() < wait_deadline)
    std::this_thread::yield();
  const bool failed_closed_before_barrier = !hm::gpu_preview::isolation_active(isolation);
  const bool waited_for_in_flight_buffer = failed_closed_before_barrier && !deactivated.load();

  {
    std::lock_guard<std::mutex> lock(barrier.mutex);
    barrier.release = true;
  }
  barrier.condition.notify_all();
  producer.join();
  deactivate.join();
  const bool passed = linked && entered && failed_closed_before_barrier && waited_for_in_flight_buffer &&
      deactivated.load() && push_result.load() == GST_FLOW_OK && !hm::gpu_preview::isolation_active(isolation);

  gst_pad_unlink(upstream, isolation_sink);
  gst_pad_unlink(isolation_src, downstream);
  gst_object_unref(isolation_sink);
  gst_object_unref(isolation_src);
  gst_object_unref(upstream);
  gst_object_unref(downstream);
  gst_object_unref(isolation);
  if (!passed)
    std::cerr << "Injected active=false exception did not fail closed behind the in-flight flow barrier\n";
  return passed;
}

GstBuffer* make_overlay_inspection_buffer(bool snapshot, bool transform) {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
  NvDsObjectMeta* player = batch ? nvds_acquire_obj_meta_from_pool(batch) : nullptr;
  NvDsDisplayMeta* display = batch ? nvds_acquire_display_meta_from_pool(batch) : nullptr;
  if (!batch || !frame || !player || !display) {
    if (batch)
      nvds_destroy_batch_meta(batch);
    return nullptr;
  }
  batch->max_frames_in_batch = 1;
  frame->source_frame_width = 4000;
  frame->source_frame_height = 2000;
  nvds_add_frame_meta_to_batch(batch, frame);
  player->class_id = 0;
  player->object_id = 7;
  player->rect_params.left = 100.0F;
  player->rect_params.top = 200.0F;
  player->rect_params.width = 300.0F;
  player->rect_params.height = 400.0F;
  nvds_add_obj_meta_to_frame(frame, player, nullptr);
  display->num_rects = 1;
  display->rect_params[0].left = 500.0F;
  display->rect_params[0].top = 600.0F;
  display->rect_params[0].width = 700.0F;
  display->rect_params[0].height = 800.0F;
  display->rect_params[0].border_width = 3;
  display->rect_params[0].border_color = NvOSD_ColorParams{1.0, 0.5, 0.0, 1.0};
  nvds_add_display_meta_to_frame(frame, display);
  if (snapshot && !hm::preview_overlay::add_overlay_snapshot_meta(frame)) {
    nvds_destroy_batch_meta(batch);
    return nullptr;
  }
  if (transform) {
    const hm::preview_overlay::PlayCropperTransform program_transform{
        4000.0F,
        2000.0F,
        4000.0F,
        2000.0F,
        500.0F,
        100.0F,
        1500.0F,
        900.0F,
        0.0F,
        0.0F,
        2000.0F,
        1000.0F,
        640.0F,
        360.0F,
        0.0F,
        false,
    };
    if (!hm::preview_overlay::add_playcropper_transform_meta(frame, program_transform)) {
      nvds_destroy_batch_meta(batch);
      return nullptr;
    }
  }
  GstBuffer* buffer = gst_buffer_new();
  NvDsMeta* meta = buffer
      ? gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func)
      : nullptr;
  if (!meta) {
    if (buffer)
      gst_buffer_unref(buffer);
    nvds_destroy_batch_meta(batch);
    return nullptr;
  }
  meta->meta_type = NVDS_BATCH_GST_META;
  return buffer;
}

bool run_program_transform_fail_closed_test() {
  GstElement* sink = gst_element_factory_make("hmgpupreviewsink", "overlay_inspection_sink");
  GstBuffer* missing_transform = make_overlay_inspection_buffer(/*snapshot=*/true, /*transform=*/false);
  GstBuffer* transformed = make_overlay_inspection_buffer(/*snapshot=*/true, /*transform=*/true);
  GstBuffer* unproven_object_fallback = make_overlay_inspection_buffer(/*snapshot=*/false, /*transform=*/false);
  if (!sink || !missing_transform || !transformed || !unproven_object_fallback) {
    std::cerr << "Could not construct Program overlay coordinate-space fixture\n";
    if (missing_transform)
      gst_buffer_unref(missing_transform);
    if (transformed)
      gst_buffer_unref(transformed);
    if (unproven_object_fallback)
      gst_buffer_unref(unproven_object_fallback);
    if (sink)
      gst_object_unref(sink);
    return false;
  }
  g_object_set(
      G_OBJECT(sink),
      "channel",
      "program",
      "show-player-tracking",
      TRUE,
      "show-play-tracking",
      TRUE,
      "show-rink-mask",
      TRUE,
      nullptr);
  const auto missing = hm::gpu_preview::inspect_preview_overlays_for_test(sink, missing_transform);
  const auto valid = hm::gpu_preview::inspect_preview_overlays_for_test(sink, transformed);
  const auto fallback = hm::gpu_preview::inspect_preview_overlays_for_test(sink, unproven_object_fallback);
  g_object_set(G_OBJECT(sink), "channel", "stitched", nullptr);
  const auto stitched = hm::gpu_preview::inspect_preview_overlays_for_test(sink, missing_transform);
  gst_buffer_unref(missing_transform);
  gst_buffer_unref(transformed);
  gst_buffer_unref(unproven_object_fallback);
  gst_object_unref(sink);
  const bool passed = !missing.diagnostic_coordinates_valid && missing.path_count == 0 &&
      valid.diagnostic_coordinates_valid && valid.path_count >= 2 && !fallback.diagnostic_coordinates_valid &&
      fallback.path_count == 0 && stitched.diagnostic_coordinates_valid && stitched.path_count >= 2;
  if (!passed)
    std::cerr << "Program diagnostics did not fail closed when crop-transform metadata was unavailable\n";
  return passed;
}

bool run_two_stage_disabled_path_test() {
  struct DownstreamState {
    std::mutex mutex;
    std::condition_variable condition;
    int handoffs{0};
    bool release_first{false};
  } state;

  GstElement* pipeline = gst_pipeline_new("two-stage-preview-test");
  GstElement* ingress = gst_element_factory_make("hmpreviewisolation", "two_stage_ingress");
  GstElement* queue = gst_element_factory_make("queue", "two_stage_queue");
  GstElement* drain = gst_element_factory_make("hmpreviewisolation", "two_stage_drain");
  GstElement* sink = gst_element_factory_make("fakesink", "two_stage_sink");
  GstPad* upstream = gst_pad_new("two_stage_upstream", GST_PAD_SRC);
  GstPad* ingress_sink = ingress ? gst_element_get_static_pad(ingress, "sink") : nullptr;
  if (!pipeline || !ingress || !queue || !drain || !sink || !upstream || !ingress_sink) {
    std::cerr << "Could not construct two-stage disabled preview path test\n";
    return false;
  }

  g_object_set(G_OBJECT(queue), "max-size-buffers", 8, "max-size-bytes", 0, "max-size-time", guint64{0}, nullptr);
  g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(
      sink,
      "handoff",
      G_CALLBACK(+[](GstElement*, GstBuffer*, GstPad*, gpointer user_data) {
        auto* downstream = static_cast<DownstreamState*>(user_data);
        std::unique_lock<std::mutex> lock(downstream->mutex);
        ++downstream->handoffs;
        downstream->condition.notify_all();
        if (downstream->handoffs == 1)
          downstream->condition.wait(lock, [downstream] { return downstream->release_first; });
      }),
      &state);
  gst_bin_add_many(GST_BIN(pipeline), ingress, queue, drain, sink, nullptr);
  const bool linked = gst_element_link_many(ingress, queue, drain, sink, nullptr) &&
      gst_pad_link(upstream, ingress_sink) == GST_PAD_LINK_OK;
  gst_pad_set_active(upstream, TRUE);
  hm::gpu_preview::set_isolation_active(drain, true, 1);
  hm::gpu_preview::set_isolation_active(ingress, true, 1);
  const bool playing = linked && gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);

  GstCaps* caps = gst_caps_from_string("video/x-raw,format=RGBA,width=1,height=1,framerate=30/1");
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_push_event(upstream, gst_event_new_stream_start("two-stage-preview-test"));
  gst_pad_push_event(upstream, gst_event_new_caps(caps));
  gst_pad_push_event(upstream, gst_event_new_segment(&segment));
  gst_caps_unref(caps);
  const bool first_pushed = playing && gst_pad_push(upstream, gst_buffer_new()) == GST_FLOW_OK;
  bool first_entered = false;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    first_entered = state.condition.wait_for(lock, std::chrono::seconds(2), [&state] { return state.handoffs == 1; });
  }
  const bool queued_second = first_entered && gst_pad_push(upstream, gst_buffer_new()) == GST_FLOW_OK;

  hm::gpu_preview::set_isolation_active(ingress, false, 2);
  std::atomic<bool> drain_closed{false};
  std::atomic<bool> drain_setter_entered{false};
  g_object_set_data(G_OBJECT(drain), "hstream-preview-test-active-setter-entered", &drain_setter_entered);
  std::thread close_drain([&] {
    hm::gpu_preview::set_isolation_active(drain, false, 2);
    drain_closed = true;
  });
  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!drain_setter_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < close_deadline)
    std::this_thread::yield();
  const bool drain_waited = drain_setter_entered.load(std::memory_order_acquire) && !drain_closed.load();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.release_first = true;
  }
  state.condition.notify_all();
  close_drain.join();

  int handoffs_at_close = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    handoffs_at_close = state.handoffs;
  }

  for (int i = 0; i < 100; ++i)
    gst_pad_push(upstream, gst_buffer_new());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int handoffs_while_off = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    handoffs_while_off = state.handoffs;
  }

  hm::gpu_preview::set_isolation_active(drain, true, 3);
  hm::gpu_preview::set_isolation_active(ingress, true, 3);
  const bool reenabled_push = gst_pad_push(upstream, gst_buffer_new()) == GST_FLOW_OK;
  bool reenabled_handoff = false;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    reenabled_handoff = state.condition.wait_for(
        lock, std::chrono::seconds(2), [&state, handoffs_at_close] { return state.handoffs == handoffs_at_close + 1; });
  }
  const bool passed = first_pushed && queued_second && drain_waited && drain_closed.load() &&
      (handoffs_at_close == 1 || handoffs_at_close == 2) && handoffs_while_off == handoffs_at_close && reenabled_push &&
      reenabled_handoff;

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_pad_unlink(upstream, ingress_sink);
  gst_pad_set_active(upstream, FALSE);
  gst_object_unref(ingress_sink);
  gst_object_unref(upstream);
  gst_object_unref(pipeline);
  if (!passed) {
    std::cerr << "Two-stage preview gate failed to drain once, stay idle while off, or reactivate (handoffs="
              << handoffs_while_off << ")\n";
  }
  return passed;
}

bool run_capture_geometry_budget_test() {
  const auto unchanged = hm::gpu_preview::bounded_capture_dimensions(1920, 1080);
  const auto bounded = hm::gpu_preview::bounded_capture_dimensions(16384, 16384);
  const std::uint64_t bounded_bytes = static_cast<std::uint64_t>(bounded.first) * bounded.second * 4U;
  const double bounded_aspect = static_cast<double>(bounded.first) / bounded.second;
  if (unchanged != std::pair<unsigned, unsigned>{1920, 1080} || bounded.first >= 16384 || bounded.second >= 16384 ||
      bounded_bytes > hm::gpu_preview::kMaximumPresentedFrameCaptureBytes || std::abs(bounded_aspect - 1.0) > 0.001) {
    std::cerr << "Presented-frame capture geometry exceeded its byte budget or distorted the source\n";
    return false;
  }
  return true;
}

bool run_render_exception_boundary_test() {
  for (const auto injection : {
           hm::gpu_preview::RenderExceptionInjection::kBadAlloc,
           hm::gpu_preview::RenderExceptionInjection::kLengthError,
           hm::gpu_preview::RenderExceptionInjection::kUnknown,
       }) {
    GstElement* sink = gst_element_factory_make("hmgpupreviewsink", nullptr);
    GstBuffer* buffer = gst_buffer_new();
    if (!sink || !buffer) {
      std::cerr << "Could not construct GPU preview exception-boundary fixture\n";
      if (buffer)
        gst_buffer_unref(buffer);
      if (sink)
        gst_object_unref(sink);
      return false;
    }
    hm::gpu_preview::set_render_exception_injection_for_test(sink, injection);
    auto* sink_class = GST_BASE_SINK_GET_CLASS(sink);
    const GstFlowReturn result = sink_class->render(GST_BASE_SINK(sink), buffer);
    gst_buffer_unref(buffer);
    gst_object_unref(sink);
    if (result != GST_FLOW_ERROR) {
      std::cerr << "GPU preview render exception escaped or returned a non-error flow result\n";
      return false;
    }
  }
  return true;
}

bool run_callback_exception_boundary_test() {
  GstElement* isolation = gst_element_factory_make("hmpreviewisolation", "callback_isolation");
  GstPad* upstream = gst_pad_new("callback_upstream", GST_PAD_SRC);
  GstPad* isolation_sink = isolation ? gst_element_get_static_pad(isolation, "sink") : nullptr;
  if (!isolation || !upstream || !isolation_sink) {
    std::cerr << "Could not construct isolation callback-boundary fixture\n";
    return false;
  }
  gst_pad_set_active(upstream, TRUE);
  gst_pad_set_active(isolation_sink, TRUE);
  const bool linked = gst_pad_link(upstream, isolation_sink) == GST_PAD_LINK_OK;
  hm::gpu_preview::set_isolation_active(isolation, true, 1);

  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationChain,
      hm::gpu_preview::CallbackExceptionInjection::kBadAlloc);
  const GstFlowReturn chain_result = linked ? gst_pad_push(upstream, gst_buffer_new()) : GST_FLOW_ERROR;

  GstBufferList* list = gst_buffer_list_new();
  gst_buffer_list_add(list, gst_buffer_new());
  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationChainList,
      hm::gpu_preview::CallbackExceptionInjection::kLengthError);
  const GstFlowReturn list_result = linked ? gst_pad_push_list(upstream, list) : GST_FLOW_ERROR;
  if (!linked)
    gst_buffer_list_unref(list);

  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationEvent,
      hm::gpu_preview::CallbackExceptionInjection::kUnknown);
  const bool event_result = linked && gst_pad_push_event(upstream, gst_event_new_stream_start("callback-test"));

  GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationQuery,
      hm::gpu_preview::CallbackExceptionInjection::kBadAlloc);
  const bool query_result = gst_pad_query(isolation_sink, query);
  gst_query_unref(query);

  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationSetProperty,
      hm::gpu_preview::CallbackExceptionInjection::kLengthError);
  g_object_set(G_OBJECT(isolation), "generation", static_cast<guint64>(2), nullptr);
  hm::gpu_preview::set_callback_exception_injection_for_test(
      isolation,
      hm::gpu_preview::CallbackPoint::kIsolationGetProperty,
      hm::gpu_preview::CallbackExceptionInjection::kUnknown);
  guint64 generation = 0;
  g_object_get(G_OBJECT(isolation), "generation", &generation, nullptr);

  gst_pad_unlink(upstream, isolation_sink);
  gst_object_unref(isolation_sink);
  gst_object_unref(upstream);
  gst_object_unref(isolation);
  if (chain_result != GST_FLOW_OK || list_result != GST_FLOW_OK || !event_result || query_result) {
    std::cerr << "Isolation callback exception escaped or returned an unsafe ownership result\n";
    return false;
  }

  const std::array<std::pair<hm::gpu_preview::CallbackPoint, hm::gpu_preview::CallbackExceptionInjection>, 8>
      sink_callbacks = {{
          {hm::gpu_preview::CallbackPoint::kSinkStart, hm::gpu_preview::CallbackExceptionInjection::kBadAlloc},
          {hm::gpu_preview::CallbackPoint::kSinkSetCaps, hm::gpu_preview::CallbackExceptionInjection::kLengthError},
          {hm::gpu_preview::CallbackPoint::kSinkStop, hm::gpu_preview::CallbackExceptionInjection::kUnknown},
          {hm::gpu_preview::CallbackPoint::kSinkSetProperty, hm::gpu_preview::CallbackExceptionInjection::kBadAlloc},
          {hm::gpu_preview::CallbackPoint::kSinkGetProperty, hm::gpu_preview::CallbackExceptionInjection::kLengthError},
          {hm::gpu_preview::CallbackPoint::kSinkUnlock, hm::gpu_preview::CallbackExceptionInjection::kUnknown},
          {hm::gpu_preview::CallbackPoint::kSinkUnlockStop, hm::gpu_preview::CallbackExceptionInjection::kBadAlloc},
          {hm::gpu_preview::CallbackPoint::kSinkSetCaps, hm::gpu_preview::CallbackExceptionInjection::kUnknown},
      }};
  for (const auto& [point, injection] : sink_callbacks) {
    GstElement* sink = gst_element_factory_make("hmgpupreviewsink", nullptr);
    if (!sink)
      return false;
    hm::gpu_preview::set_callback_exception_injection_for_test(sink, point, injection);
    auto* sink_class = GST_BASE_SINK_GET_CLASS(sink);
    gboolean callback_result = TRUE;
    switch (point) {
      case hm::gpu_preview::CallbackPoint::kSinkStart:
        callback_result = sink_class->start(GST_BASE_SINK(sink));
        break;
      case hm::gpu_preview::CallbackPoint::kSinkSetCaps: {
        GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA,width=1,height=1,framerate=1/1");
        callback_result = sink_class->set_caps(GST_BASE_SINK(sink), caps);
        gst_caps_unref(caps);
        break;
      }
      case hm::gpu_preview::CallbackPoint::kSinkStop:
        callback_result = sink_class->stop(GST_BASE_SINK(sink));
        break;
      case hm::gpu_preview::CallbackPoint::kSinkSetProperty:
        g_object_set(G_OBJECT(sink), "channel", "injected", nullptr);
        break;
      case hm::gpu_preview::CallbackPoint::kSinkGetProperty: {
        guint gpu_id = 0;
        g_object_get(G_OBJECT(sink), "gpu-id", &gpu_id, nullptr);
        break;
      }
      case hm::gpu_preview::CallbackPoint::kSinkUnlock:
        callback_result = sink_class->unlock(GST_BASE_SINK(sink));
        break;
      case hm::gpu_preview::CallbackPoint::kSinkUnlockStop:
        callback_result = sink_class->unlock_stop(GST_BASE_SINK(sink));
        break;
      default:
        callback_result = FALSE;
        break;
    }
    gst_object_unref(sink);
    if (!callback_result) {
      std::cerr << "Preview-sink callback exception escaped or propagated into the main pipeline\n";
      return false;
    }
  }
  return true;
}

bool make_rink_mask_fixture(const fs::path& root, std::string* output_generation) {
  if (!output_generation)
    return false;
  for (const char* name : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
           "seam_file.png",
       }) {
    std::ofstream(root / name, std::ios::binary) << "preview-rink-mask-reactivation-fixture\n";
  }
  auto lock = hm::stitching::HuginProject::RecoverAndLock(root);
  if (!lock.ok()) {
    std::cerr << "Could not lock rink-mask reactivation fixture: " << lock.status() << '\n';
    return false;
  }
  auto hugin_generation = hm::stitching::HuginProject::GenerationId(root, **lock);
  lock->reset();
  if (!hugin_generation.ok()) {
    std::cerr << "Could not identify rink-mask reactivation fixture: " << hugin_generation.status() << '\n';
    return false;
  }
  auto generation = hm::stitching::stitched_output_generation_id(*hugin_generation, 0.0);
  if (!generation.ok()) {
    std::cerr << "Could not create stitched-output fixture generation: " << generation.status() << '\n';
    return false;
  }
  if (!cv::imwrite((root / "rink_mask_0.png").string(), cv::Mat(360, 640, CV_8UC1, cv::Scalar(255)))) {
    std::cerr << "Could not write rink-mask reactivation PNG fixture\n";
    return false;
  }
  YAML::Node config;
  config["rink"]["stitched_output_generation"] = *generation;
  std::ofstream config_output(root / "config.yaml");
  config_output << config << '\n';
  if (!config_output) {
    std::cerr << "Could not write rink-mask reactivation config fixture\n";
    return false;
  }
  *output_generation = *generation;
  return true;
}

struct GenerationProbeState {
  std::string generation;
};

GstPadProbeReturn attach_stitched_generation(GstPad*, GstPadProbeInfo* info, gpointer user_data) noexcept {
#ifdef HAS_NVDS_CUSTOMUSERMETA
  try {
    auto* state = static_cast<GenerationProbeState*>(user_data);
    GstBuffer* input = info && (info->type & GST_PAD_PROBE_TYPE_BUFFER) ? GST_PAD_PROBE_INFO_BUFFER(info) : nullptr;
    if (!state || !input)
      return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(input);
    if (!buffer)
      return GST_PAD_PROBE_OK;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    NvDsBatchMeta* batch = nvds_create_batch_meta(1);
    NvDsFrameMeta* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
    if (!batch || !frame) {
      if (batch)
        nvds_destroy_batch_meta(batch);
      return GST_PAD_PROBE_OK;
    }
    batch->max_frames_in_batch = 1;
    // DeepStream metadata can use a scaled coordinate space even though the
    // live NVMM surface and persisted rink mask remain full resolution.
    frame->source_frame_width = 320;
    frame->source_frame_height = 180;
    nvds_add_frame_meta_to_batch(batch, frame);
    hm::stitching::StitchedOutputGenerationPayload::create_and_add<hm::stitching::StitchedOutputGenerationPayload>(
        frame, state->generation);
    NvDsMeta* meta =
        gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
    if (!meta) {
      nvds_destroy_batch_meta(batch);
      return GST_PAD_PROBE_OK;
    }
    meta->meta_type = NVDS_BATCH_GST_META;
  } catch (const std::exception& error) {
    std::cerr << "Generation probe exception: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "Generation probe unknown exception\n";
  }
#else
  (void)info;
  (void)user_data;
#endif
  return GST_PAD_PROBE_OK;
}

template <typename Predicate>
bool wait_for_condition(Predicate predicate, std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

bool run_rink_mask_reactivation_test(Window window) {
#ifndef HAS_NVDS_CUSTOMUSERMETA
  (void)window;
  return true;
#else
  TempDirectory fixture("hstream-preview-rink-reactivation");
  std::string output_generation;
  if (!make_rink_mask_fixture(fixture.path(), &output_generation))
    return false;

  const char* existing_invalidation = g_getenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  const bool restore_invalidation = existing_invalidation && *existing_invalidation;
  const std::string saved_invalidation = restore_invalidation ? existing_invalidation : "";
  g_unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");

  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc is-live=true pattern=black ! "
      "video/x-raw,width=640,height=360,framerate=30/1 ! "
      "nvvideoconvert gpu-id=0 nvbuf-memory-type=2 output-buffers=1 ! "
      "video/x-raw(memory:NVMM),format=RGBA,width=640,height=360 ! "
      "hmpreviewisolation name=gate ! "
      "hmgpupreviewsink name=preview gpu-id=0 channel=stitched sync=false async=false",
      &error);
  GstElement* gate = pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), "gate") : nullptr;
  GstElement* sink = pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), "preview") : nullptr;
  GstPad* gate_sink = gate ? gst_element_get_static_pad(gate, "sink") : nullptr;
  GenerationProbeState probe_state{output_generation};
  const gulong probe = gate_sink
      ? gst_pad_add_probe(gate_sink, GST_PAD_PROBE_TYPE_BUFFER, attach_stitched_generation, &probe_state, nullptr)
      : 0;
  if (!pipeline || !gate || !sink || !gate_sink || probe == 0) {
    std::cerr << "Could not construct rink-mask reactivation pipeline: " << (error ? error->message : "unknown")
              << '\n';
    g_clear_error(&error);
    if (gate_sink)
      gst_object_unref(gate_sink);
    if (gate)
      gst_object_unref(gate);
    if (sink)
      gst_object_unref(sink);
    if (pipeline)
      gst_object_unref(pipeline);
    if (restore_invalidation)
      g_setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", saved_invalidation.c_str(), TRUE);
    return false;
  }
  g_clear_error(&error);
  g_object_set(
      G_OBJECT(sink),
      "window-id",
      static_cast<guint64>(window),
      "show-rink-mask",
      TRUE,
      "rink-mask-file",
      (fixture.path() / "rink_mask_0.png").c_str(),
      nullptr);
  hm::gpu_preview::set_source_geometry(sink, 640, 360);
  hm::gpu_preview::set_isolation_active(gate, true, 17);
  hm::gpu_preview::set_renderer_generation(sink, 17);
  const bool playing = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  const bool initially_loaded =
      playing &&
      wait_for_condition(
          [&] { return hm::gpu_preview::renderer_rink_mask_loaded_for_test(sink, output_generation, 640, 360); },
          std::chrono::seconds(10));

  hm::gpu_preview::set_isolation_active(gate, false, 17);
  const bool quiesced = hm::gpu_preview::quiesce(sink, 17);
  const bool cache_cleared = hm::gpu_preview::renderer_rink_mask_cache_cleared_for_test(sink);
  hm::gpu_preview::set_isolation_active(gate, true, 17);
  const bool reloaded = wait_for_condition(
      [&] { return hm::gpu_preview::renderer_rink_mask_loaded_for_test(sink, output_generation, 640, 360); },
      std::chrono::seconds(10));

  hm::gpu_preview::set_isolation_active(gate, false, 17);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_pad_remove_probe(gate_sink, probe);
  gst_object_unref(gate_sink);
  gst_object_unref(gate);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  if (restore_invalidation)
    g_setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", saved_invalidation.c_str(), TRUE);
  else
    g_unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  const bool passed = initially_loaded && quiesced && cache_cleared && reloaded;
  if (!passed) {
    std::cerr << "Rink mask did not reload after same-generation renderer quiesce/reactivation: initial="
              << initially_loaded << " quiesced=" << quiesced << " cleared=" << cache_cleared
              << " reloaded=" << reloaded << '\n';
  }
  return passed;
#endif
}

bool run_renderer_test(Display* display, Window window) {
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc pattern=smpte num-buffers=5 ! "
      "video/x-raw,width=640,height=360,framerate=30/1 ! "
      "nvvideoconvert gpu-id=0 nvbuf-memory-type=2 output-buffers=1 ! "
      "video/x-raw(memory:NVMM),format=RGBA,width=640,height=360 ! "
      "hmgpupreviewsink name=preview gpu-id=0 channel=test sync=false async=false",
      &error);
  if (!pipeline) {
    std::cerr << "Could not create GPU preview test pipeline: " << (error ? error->message : "unknown") << '\n';
    g_clear_error(&error);
    return false;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "preview");
  g_object_set(G_OBJECT(sink), "window-id", static_cast<guint64>(window), nullptr);
  gst_object_unref(sink);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Could not start GPU preview test pipeline\n";
    gst_object_unref(pipeline);
    return false;
  }

  bool ready = false;
  GstBus* bus = gst_element_get_bus(pipeline);
  const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (!ready && g_get_monotonic_time() < deadline) {
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_APPLICATION | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!message)
      continue;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_APPLICATION) {
      const GstStructure* structure = gst_message_get_structure(message);
      const gchar* status = structure ? gst_structure_get_string(structure, "status") : nullptr;
      ready = ready || g_strcmp0(status, "ready") == 0;
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* bus_error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &bus_error, &debug);
      std::cerr << "GPU preview pipeline error: " << (bus_error ? bus_error->message : "unknown") << '\n';
      g_clear_error(&bus_error);
      g_free(debug);
      gst_message_unref(message);
      gst_object_unref(bus);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return false;
    }
    gst_message_unref(message);
  }

  std::vector<std::uint8_t> rgba;
  unsigned width = 0;
  unsigned height = 0;
  std::string capture_error;
  sink = gst_bin_get_by_name(GST_BIN(pipeline), "preview");
  const bool captured = hm::gpu_preview::capture_presented_frame(sink, &rgba, &width, &height, &capture_error);
  hm::gpu_preview::set_capture_exception_injection_for_test(
      sink, hm::gpu_preview::CallbackExceptionInjection::kBadAlloc);
  std::vector<std::uint8_t> failed_capture_rgba;
  unsigned failed_capture_width = 0;
  unsigned failed_capture_height = 0;
  std::string failed_capture_error;
  const bool injected_capture = hm::gpu_preview::capture_presented_frame(
      sink, &failed_capture_rgba, &failed_capture_width, &failed_capture_height, &failed_capture_error);
  std::vector<std::uint8_t> recovered_capture_rgba;
  unsigned recovered_capture_width = 0;
  unsigned recovered_capture_height = 0;
  std::string recovered_capture_error;
  const bool recovered_capture = hm::gpu_preview::capture_presented_frame(
      sink, &recovered_capture_rgba, &recovered_capture_width, &recovered_capture_height, &recovered_capture_error);
  XResizeWindow(display, window, 4096, 2160);
  XSync(display, False);
  std::vector<std::uint8_t> bounded_rgba;
  unsigned bounded_width = 0;
  unsigned bounded_height = 0;
  std::string bounded_capture_error;
  const bool bounded_capture = hm::gpu_preview::capture_presented_frame(
      sink, &bounded_rgba, &bounded_width, &bounded_height, &bounded_capture_error);
  const auto [minimum, maximum] = rgba.empty()
      ? std::pair<std::uint8_t, std::uint8_t>{0, 0}
      : std::minmax({*std::min_element(rgba.begin(), rgba.end()), *std::max_element(rgba.begin(), rgba.end())});

  bool eos = false;
  GstMessage* terminal =
      gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  if (terminal) {
    eos = GST_MESSAGE_TYPE(terminal) == GST_MESSAGE_EOS;
    gst_message_unref(terminal);
  }
  // The UI owns the foreign XID and may destroy it before the sink transitions
  // to NULL. Cleanup must use the sink's private GLX drawable in that case.
  XDestroyWindow(display, window);
  XSync(display, False);
  gst_object_unref(bus);
  hm::gpu_preview::set_callback_exception_injection_for_test(
      sink, hm::gpu_preview::CallbackPoint::kSinkStop, hm::gpu_preview::CallbackExceptionInjection::kUnknown);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  const bool stale_xid_cleanup = hm::gpu_preview::renderer_resources_released_for_test(sink);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  const bool bounded_capture_ok = bounded_capture && bounded_width < 4096 && bounded_height < 2160 &&
      bounded_rgba.size() <= hm::gpu_preview::kMaximumPresentedFrameCaptureBytes;
  const bool capture_recovery_ok = !injected_capture && !failed_capture_error.empty() && recovered_capture &&
      recovered_capture_width == 640 && recovered_capture_height == 360 && !recovered_capture_rgba.empty();
  if (!ready || !eos || !captured || width != 640 || height != 360 || maximum == minimum || !capture_recovery_ok ||
      !bounded_capture_ok || !stale_xid_cleanup) {
    std::cerr << "GPU preview did not expose its presented texture: ready=" << ready << " captured=" << captured
              << " range=" << static_cast<int>(maximum - minimum) << " error=" << capture_error
              << " bounded-capture=" << bounded_capture << " bounded-size=" << bounded_width << 'x' << bounded_height
              << " bounded-error=" << bounded_capture_error << " capture-recovery=" << capture_recovery_ok << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  if (!hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements()) {
    std::cout << "GPU preview renderer unavailable; skipping\n";
    return 0;
  }
  if (!run_flow_isolation_test())
    return 1;
  if (!run_deactivation_barrier_test())
    return 1;
  if (!run_deactivation_exception_barrier_test())
    return 1;
  if (!run_program_transform_fail_closed_test())
    return 1;
  if (!run_two_stage_disabled_path_test())
    return 1;
  if (!run_capture_geometry_budget_test())
    return 1;
  if (!run_render_exception_boundary_test())
    return 1;
  if (!run_callback_exception_boundary_test())
    return 1;
  if (!gst_element_factory_find("nvvideoconvert") || !std::getenv("DISPLAY")) {
    std::cout << "NVMM conversion or X11 display unavailable; skipping\n";
    return 0;
  }

  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    std::cout << "X11 display unavailable; skipping\n";
    return 0;
  }
  const int screen = DefaultScreen(display);
  auto create_window = [&](const char* name) {
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        360,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    XStoreName(display, window, name);
    XMapRaised(display, window);
    return window;
  };
  Window rink_window = create_window("hstream-rink-mask-reactivation-test");
  XSync(display, False);
  const bool rink_reactivation_passed = run_rink_mask_reactivation_test(rink_window);
  XDestroyWindow(display, rink_window);
  XSync(display, False);

  Window renderer_window = create_window("hstream-gpu-preview-test");
  XSync(display, False);
  const bool renderer_passed = run_renderer_test(display, renderer_window);
  XCloseDisplay(display);
  return rink_reactivation_passed && renderer_passed ? 0 : 1;
}
