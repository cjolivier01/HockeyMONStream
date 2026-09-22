#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>

#include "absl/status/status.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

class FailingRuntimeSize final : public hm::CustomAlgorithmBase {
 public:
  FailingRuntimeSize() : CustomAlgorithmBase(0, 2) {
    m_transformMode = true;
  }

  bool UsesRuntimeOutputSize() const override {
    return true;
  }

  absl::StatusOr<hm::videoprep::RuntimeOutputSize> PrepareRuntimeOutputSize(NvDsBatchMeta*, NvBufSurface*) override {
    ++sizing_calls;
    return absl::InternalError("Unable to remove recovered transaction: Directory not empty");
  }

  absl::Status GenerateOutput(NvDsBatchMeta*, NvBufSurface*, NvBufSurface*) override {
    ++generation_calls;
    return absl::InternalError("Output must not run after failed runtime sizing");
  }

  std::atomic<int> sizing_calls{0};
  std::atomic<int> generation_calls{0};
};

bool expect_nonfatal_pool_statuses() {
  hm::videoprep::RuntimeOutputPoolFlow flow;
  int eos_events = 0;
  const auto send_eos = [&] { ++eos_events; };
  bool ok = expect(
      flow.handle_status(
          hm::videoprep::runtime_output_pool_deferred_status("Waiting for selected pairs"), gst_buffer_new(), send_eos),
      "deferred capture must consume only its current input");
  ok &= expect(!flow.eos_terminal() && eos_events == 0, "deferred capture must remain resumable without EOS");
  ok &= expect(
      flow.handle_status(absl::CancelledError("Source reached EOS"), gst_buffer_new(), send_eos),
      "normal cancellation must consume input and end output");
  ok &= expect(flow.eos_terminal() && eos_events == 1, "normal cancellation must preserve one terminal EOS");
  ok &=
      expect(flow.consume_if_terminal(gst_buffer_new()) && eos_events == 1, "later terminal input must not repeat EOS");
  return ok;
}

bool expect_single_input_failure(GstElement* element, GstBus* bus, cudaStream_t stream) {
  FailingRuntimeSize algorithm;
  GstCaps* caps =
      gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA,width=16,height=16,framerate=30/1,batch-size=2");
  hm::DSCustom_CreateParams params;
  params.m_element = GST_BASE_TRANSFORM(element);
  params.m_inCaps = caps;
  params.m_outCaps = caps;
  params.m_cudaStream = stream;
  params.m_bufferPoolConfig.max_buffers = 2;
  const auto initialized = algorithm.PostCapsInit(&params);
  gst_caps_unref(caps);
  if (!expect(initialized.ok(), "the runtime-sized worker must initialize without allocating its output pool"))
    return false;

  // Only the NvBufSurface envelope is needed: the injected sizing error happens
  // before surface access, GPU allocation, matching, or output generation.
  NvBufSurface surface{};
  surface.batchSize = 2;
  surface.numFilled = 2;
  GstBuffer* input = gst_buffer_new_allocate(nullptr, sizeof(surface), nullptr);
  gst_buffer_fill(input, 0, &surface, sizeof(surface));
  std::atomic<int> released{0};
  gst_mini_object_weak_ref(
      GST_MINI_OBJECT_CAST(input),
      [](gpointer count, GstMiniObject*) { ++*static_cast<std::atomic<int>*>(count); },
      &released);
  const auto accepted = algorithm.ProcessBuffer(input);
  bool ok = expect(accepted == hm::BufferResult::Buffer_Async, "the single input must reach the private output worker");
  if (accepted != hm::BufferResult::Buffer_Async)
    gst_buffer_unref(input);

  // No second input, source EOS, timeout command, or externally requested
  // shutdown may be needed to make a runtime calibration failure observable.
  GstMessage* message =
      gst_bus_timed_pop_filtered(bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  ok &= expect(
      message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR,
      "one failed input must immediately post a bus error, never EOS");
  if (message) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const std::string diagnostic = error && error->message ? error->message : "";
      ok &= expect(
          diagnostic.find("INTERNAL: Unable to remove recovered transaction: Directory not empty") != std::string::npos,
          "the worker bus error must preserve the original calibration failure");
      g_clear_error(&error);
      g_free(debug);
    }
    gst_message_unref(message);
  }
  bool stopped = false;
  {
    std::unique_lock<std::mutex> lock(algorithm.m_processLock);
    stopped =
        algorithm.m_processCV.wait_for(lock, std::chrono::seconds(1), [&] { return algorithm.outputthread_stopped; });
  }
  ok &= expect(stopped, "a fatal sizing error must stop the worker without more input");
  algorithm.Shutdown();
  delete algorithm.m_outputThread;
  algorithm.m_outputThread = nullptr;
  ok &= expect(
      algorithm.shutdown_requested_ && algorithm.get_last_flow_ret() == GST_FLOW_ERROR && released == 1 &&
          algorithm.sizing_calls == 1 && algorithm.generation_calls == 0 && !algorithm.m_dsBufferPool,
      "failure must release input once and prevent output allocation or a calibration retry");
  GstMessage* extra =
      gst_bus_timed_pop_filtered(bus, 0, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  ok &= expect(extra == nullptr, "fatal sizing must produce exactly one terminal error and no EOS");
  if (extra)
    gst_message_unref(extra);
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = expect_nonfatal_pool_statuses();
  int devices = 0;
  if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
    std::cout << "SKIP: the actual output-worker regression requires CUDA device initialization\n";
    return ok ? 0 : 1;
  }
  cudaStream_t stream = nullptr;
  if (!expect(cudaStreamCreate(&stream) == cudaSuccess, "cannot initialize the worker's CUDA stream"))
    return 1;
  GstElement* pipeline = gst_pipeline_new("runtime-output-failure-test");
  GstElement* element = gst_element_factory_make("identity", "runtime-sized-algorithm");
  if (!expect(pipeline && element, "required GStreamer test elements are unavailable"))
    return 1;
  gst_bin_add(GST_BIN(pipeline), element);
  GstBus* bus = gst_element_get_bus(pipeline);
  ok &= expect_single_input_failure(element, bus, stream);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  cudaStreamDestroy(stream);
  return ok ? 0 : 1;
}
