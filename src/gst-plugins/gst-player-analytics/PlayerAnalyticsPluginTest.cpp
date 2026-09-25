// Native opt-in integration. With no bundle argument only the disabled path is
// exercised, including CUDA-call interception and descriptor-free passthrough.
#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <gst/check/gstharness.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hstream/src/gst-plugins/gst-player-analytics/BorrowedImage.h"
#include "hstream/src/gst-plugins/gst-player-analytics/PlayerAnalyticsProcessor.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

namespace {
namespace pa = hm::player_analytics;
std::atomic<bool> reject_cuda{false};
std::atomic<unsigned> cuda_calls{0};
std::atomic<bool> fail_copy{false};
std::atomic<bool> block_fence{false};
std::atomic<cudaStream_t> observed_stream{nullptr};
std::mutex fence_mutex;
std::condition_variable fence_condition;
bool fence_entered{false};
bool release_fence{false};

template <typename Function>
Function Actual(const char* name) {
  auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
  if (!function) {
    std::cerr << "Missing CUDA test interposition target " << name << '\n';
    std::abort();
  }
  return function;
}

bool Check(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

// These test-only interpositions fail the first possible CUDA operations on the
// disabled path and inject a queued-work failure/flush barrier on the enabled
// path. Production code has no test hooks or extra synchronization points.
extern "C" cudaError_t cudaSetDevice(int device) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  return Actual<decltype(&cudaSetDevice)>("cudaSetDevice")(device);
}
extern "C" cudaError_t cudaStreamCreateWithFlags(cudaStream_t* stream, unsigned flags) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  return Actual<decltype(&cudaStreamCreateWithFlags)>("cudaStreamCreateWithFlags")(stream, flags);
}
extern "C" cudaError_t cudaMalloc(void** pointer, size_t bytes) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  return Actual<decltype(&cudaMalloc)>("cudaMalloc")(pointer, bytes);
}
extern "C" cudaError_t cudaMallocHost(void** pointer, size_t bytes) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  return Actual<decltype(&cudaMallocHost)>("cudaMallocHost")(pointer, bytes);
}
extern "C" cudaError_t cudaMemcpyAsync(
    void* destination,
    const void* source,
    size_t bytes,
    cudaMemcpyKind kind,
    cudaStream_t stream) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  if (kind == cudaMemcpyDeviceToHost && fail_copy.exchange(false)) {
    observed_stream.store(stream);
    return cudaErrorInvalidValue;
  }
  return Actual<decltype(&cudaMemcpyAsync)>("cudaMemcpyAsync")(destination, source, bytes, kind, stream);
}
extern "C" cudaError_t cudaStreamSynchronize(cudaStream_t stream) {
  ++cuda_calls;
  if (reject_cuda)
    return cudaErrorInitializationError;
  if (block_fence.exchange(false)) {
    std::unique_lock<std::mutex> lock(fence_mutex);
    observed_stream.store(stream);
    fence_entered = true;
    fence_condition.notify_all();
    if (!fence_condition.wait_for(lock, std::chrono::seconds(5), [] { return release_fence; }))
      std::cerr << "Timed out awaiting test flush event\n";
  }
  return Actual<decltype(&cudaStreamSynchronize)>("cudaStreamSynchronize")(stream);
}

namespace {
struct Harness {
  GstElement* element{gst_element_factory_make("hmplayeranalytics", nullptr)};
  GstBus* bus{gst_bus_new()};
  GstHarness* harness{nullptr};
  explicit Harness(const std::string& configuration, int gpu = 0) {
    if (!element)
      return;
    g_object_set(element, "configuration", configuration.c_str(), "gpu-id", gpu, nullptr);
    gst_element_set_bus(element, bus);
    harness = gst_harness_new_with_element(element, "sink", "src");
    gst_harness_set_src_caps_str(harness, "video/x-raw(memory:NVMM),format=RGBA,width=320,height=240,framerate=30/1");
    gst_harness_play(harness);
  }
  ~Harness() {
    if (harness)
      gst_harness_teardown(harness);
    gst_object_unref(bus);
  }
  void Segment() {
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_harness_push_event(harness, gst_event_new_segment(&segment));
  }
};

bool Disabled() {
  reject_cuda = true;
  const auto before = cuda_calls.load();
  pa::Config disabled;
  disabled.pose.bundle = "/proc/1/not-readable/model.engine";
  disabled.batch_size = 0;
  const auto processor = pa::PlayerAnalyticsProcessor::Create(disabled, -1);
  bool okay = processor.ok() && !*processor;
  for (int invalid = 0; invalid < 4; ++invalid) {
    pa::Config config;
    config.batch_size = 1;
    config.pose.bundle = config.jersey.bundle = config.action.bundle = "/inaccessible";
    if (invalid == 0)
      config.action.enabled = true;
    if (invalid == 1) {
      config.pose.enabled = config.action.enabled = true;
      config.pose.rate_hz = 9;
    }
    if (invalid >= 2) {
      config.jersey.enabled = true;
      config.jersey_roi_mode = pa::JerseyRoiMode::kPose;
      if (invalid == 3) {
        config.pose.enabled = true;
        config.maximum_due_rois = 1;
      }
    }
    okay &= Check(
        !pa::PlayerAnalyticsProcessor::Create(config, 0).ok(),
        "Invalid semantic dependency/budget accepted before CUDA");
  }
  {
    Harness test(
        "pose: {enable: false, bundle: /proc/1/not-readable/model.engine}\n"
        "jersey: {enable: false, bundle: /proc/1/not-readable/jersey}\n"
        "action: {enable: false, bundle: /proc/1/not-readable/action}\n"
        "batch-size: invalid\nmax-tracks: invalid\ndraw-pose: true\n",
        G_MAXINT);
    if (!test.harness)
      return false;
    // No NvBufSurface descriptor or metadata: disabled inference must not map
    // the buffer or inspect model paths despite a drawing preference.
    GstBuffer* output = gst_harness_push_and_pull(test.harness, gst_buffer_new());
    okay &= Check(output && gst_buffer_get_size(output) == 0, "Disabled passthrough touched its input");
    if (output)
      gst_buffer_unref(output);
  }
  // Invalid enabled configuration must fail before CUDA and leave the element
  // restartable after NULL; startup errors must reach its bus.
  GstElement* invalid = gst_element_factory_make("hmplayeranalytics", nullptr);
  GstBus* bus = gst_bus_new();
  gst_element_set_bus(invalid, bus);
  g_object_set(invalid, "configuration", "pose: {enable: true, bundle: /missing}\nbatch-size: 0", nullptr);
  okay &= Check(
      gst_element_set_state(invalid, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE,
      "Invalid enabled configuration did not fail startup");
  GstMessage* message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
  okay &= Check(message != nullptr, "Startup failure did not post a bus error");
  if (message)
    gst_message_unref(message);
  gst_element_set_state(invalid, GST_STATE_NULL);
  g_object_set(invalid, "configuration", "", nullptr);
  okay &= Check(
      gst_element_set_state(invalid, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE,
      "Element could not restart disabled after failed startup");
  gst_element_set_state(invalid, GST_STATE_NULL);
  gst_object_unref(invalid);
  gst_object_unref(bus);
  okay &= Check(cuda_calls == before, "Disabled analytics called CUDA");
  reject_cuda = false;
  return okay;
}

GstBuffer* Input(uint64_t pts, int frame_number, bool discontinuity = false) {
  NvBufSurfaceCreateParams parameters{};
  parameters.gpuId = 0;
  parameters.width = 320;
  parameters.height = 240;
  parameters.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  parameters.layout = NVBUF_LAYOUT_PITCH;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  parameters.memType = NVBUF_MEM_SURFACE_ARRAY;
#else
  parameters.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
  NvBufSurface* surface = nullptr;
  if (NvBufSurfaceCreate(&surface, 1, &parameters) != 0)
    return nullptr;
  std::unique_ptr<NvBufSurface, decltype(&NvBufSurfaceDestroy)> owned(surface, &NvBufSurfaceDestroy);
  surface->numFilled = 1;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  // Fixture initialization only: the production EGL import is read-only.
  if (NvBufSurfaceMemSet(surface, 0, -1, 127) != 0)
    return nullptr;
#else
  {
    auto image = pa::BorrowedImage::Map(surface, 0, 0);
    if (!image.ok()) {
      std::cerr << image.status() << '\n';
      return nullptr;
    }
    cudaStream_t stream{};
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
      return nullptr;
    const auto& view = (*image)->view();
    const auto cleared = cudaMemset2DAsync(const_cast<void*>(view.data), view.pitch, 127, 320 * 4, 240, stream);
    const auto fenced = cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);
    if (cleared != cudaSuccess || fenced != cudaSuccess)
      return nullptr;
  }
#endif
  GstBuffer* buffer = gst_buffer_new_wrapped_full(
      GST_MEMORY_FLAG_READONLY, surface, sizeof(*surface), 0, sizeof(*surface), surface, +[](gpointer value) {
        NvBufSurfaceDestroy(static_cast<NvBufSurface*>(value));
      });
  owned.release();
  GST_BUFFER_PTS(buffer) = pts;
  if (discontinuity)
    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
  auto* batch = nvds_create_batch_meta(1);
  auto* frame = nvds_acquire_frame_meta_from_pool(batch);
  frame->source_id = 7;
  frame->source_frame_width = 640;
  frame->source_frame_height = 480;
  frame->frame_num = frame_number;
  frame->buf_pts = pts;
  nvds_add_frame_meta_to_batch(batch, frame);
  for (uint64_t id : {1, 2, 3}) {
    auto* object = nvds_acquire_obj_meta_from_pool(batch);
    object->object_id = (uint64_t{1} << 40) + id;
    object->class_id = 0;
    object->rect_params.left = 50.25F + id * 80;
    object->rect_params.top = 30.75F;
    object->rect_params.width = 100.5F;
    object->rect_params.height = 350.25F;
    nvds_add_obj_meta_to_frame(frame, object, nullptr);
  }
  auto* meta =
      gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  meta->meta_type = NVDS_BATCH_GST_META;
  return buffer;
}

bool Push(Harness& test, uint64_t pts, int frame_number, size_t count, uint64_t* epoch, bool discont = false) {
  GstBuffer* input = Input(pts, frame_number, discont);
  if (!Check(input != nullptr, "Cannot create GPU input"))
    return false;
  const auto flow = gst_harness_push(test.harness, input);
  if (!Check(flow == GST_FLOW_OK, "Actual model plugin processing failed"))
    return false;
  GstBuffer* output = gst_harness_try_pull(test.harness);
  if (!Check(output != nullptr, "Missing plugin output"))
    return false;
  auto* batch = gst_buffer_get_nvds_batch_meta(output);
  auto* frame = batch && batch->frame_meta_list ? static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data) : nullptr;
  const auto* result = pa::FindFrameResult(frame);
  bool okay = count ? result && pa::ValidateFrameResult(*result) && result->player_count == count : !result;
  if (result) {
    *epoch = result->epoch;
    for (size_t i = 0; i < result->player_count; ++i)
      okay &= result->players[i].has_pose && result->players[i].pose_observed_at == pts &&
          result->players[i].track_id > (uint64_t{1} << 40);
  }
  gst_buffer_unref(output);
  return Check(okay, "Incorrect same-frame pose metadata, cadence, or track identity");
}

bool Enabled(const char* bundle) {
  Harness test(
      "pose: {enable: true, bundle: '" + std::string(bundle) +
      "', rate-hz: 10, confidence-threshold: 0}\nmax-tracks: 4\nmax-due-rois: 2\nbatch-size: 2\n");
  if (!test.harness)
    return false;
  uint64_t epoch = 0;
  if (!Push(test, pa::kSecond, 1, 2, &epoch))
    return false;
  const uint64_t first_epoch = epoch;
  if (!Push(test, pa::kSecond, 2, 0, &epoch) || !Push(test, pa::kSecond + 10000000, 3, 1, &epoch))
    return false;
  test.Segment();
  if (!Push(test, pa::kSecond + 100000000, 4, 2, &epoch) ||
      !Check(epoch == first_epoch, "Continuous chapter segment reset analytics"))
    return false;
  if (!Push(test, pa::kSecond + 110000000, 5, 2, &epoch, true) ||
      !Check(epoch != first_epoch, "Discontinuity did not reset analytics"))
    return false;
  g_object_set(test.element, "gpu-id", G_MAXINT, nullptr);
  gint gpu = -1;
  g_object_get(test.element, "gpu-id", &gpu, nullptr);
  if (!Check(gpu == 0, "Running GPU configuration was mutable"))
    return false;

  // Stop inside the real stream completion boundary while a nonserialized
  // FLUSH_START arrives. Old metadata must not be published, and the source
  // surface must remain owned through the completion fence.
  GstBuffer* in_flight = Input(2 * pa::kSecond, 6);
  if (!in_flight)
    return false;
  block_fence = true;
  GstFlowReturn cancelled_flow = GST_FLOW_ERROR;
  std::thread pushing([&] { cancelled_flow = gst_harness_push(test.harness, in_flight); });
  bool reached = false;
  {
    std::unique_lock<std::mutex> lock(fence_mutex);
    reached = fence_condition.wait_for(lock, std::chrono::seconds(3), [] { return fence_entered; });
  }
  if (reached)
    gst_harness_push_event(test.harness, gst_event_new_flush_start());
  {
    std::lock_guard<std::mutex> lock(fence_mutex);
    release_fence = true;
  }
  fence_condition.notify_all();
  pushing.join();
  if (!Check(
          reached && cancelled_flow == GST_FLOW_FLUSHING && gst_harness_buffers_in_queue(test.harness) == 0,
          "Concurrent flush published an old result") ||
      !Check(cudaStreamQuery(observed_stream.load()) == cudaSuccess, "Flush returned before retiring GPU work"))
    return false;
  gst_harness_push_event(test.harness, gst_event_new_flush_stop(TRUE));
  test.Segment();
  if (!Push(test, pa::kSecond, 7, 2, &epoch))
    return false;

  GstBuffer* failing = Input(3 * pa::kSecond, 8);
  if (!failing)
    return false;
  fail_copy = true;
  const auto failed_flow = gst_harness_push(test.harness, failing);
  const auto fenced = cudaStreamQuery(observed_stream.load());
  GstMessage* message = gst_bus_timed_pop_filtered(test.bus, GST_SECOND, GST_MESSAGE_ERROR);
  const bool okay = Check(failed_flow == GST_FLOW_ERROR && message, "GPU failure did not post a fatal bus error") &&
      Check(fenced == cudaSuccess, "GPU failure released its borrowed surface before fencing") &&
      Check(gst_harness_push(test.harness, gst_buffer_new()) == GST_FLOW_ERROR, "Fatal error was not latched");
  if (message)
    gst_message_unref(message);
  return okay;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: PlayerAnalyticsPluginTest PLUGIN_SO [REAL_POSE_BUNDLE]\n";
    return 1;
  }
  GError* error = nullptr;
  GstPlugin* plugin = gst_plugin_load_file(argv[1], &error);
  if (!plugin) {
    std::cerr << (error ? error->message : "Cannot load analytics plugin") << '\n';
    g_clear_error(&error);
    return 1;
  }
  gst_object_unref(plugin);
  if (!Disabled() || (argc == 3 && !Enabled(argv[2])))
    return 1;
  std::cout << "Disabled CUDA/file bypass"
            << (argc == 3 ? "; real pose, cadence, discontinuity, flush and error fencing" : "") << " passed\n";
}
