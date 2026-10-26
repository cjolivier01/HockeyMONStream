#include "hstream/src/apps/apps-common/PipelineStartupError.h"

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::abort();
  }
}

struct Pipeline {
  GstElement* value{gst_pipeline_new(nullptr)};
  ~Pipeline() {
    gst_element_set_state(value, GST_STATE_NULL);
    gst_object_unref(value);
  }

  GstElement* Add(const char* factory, const char* name) {
    auto* element = gst_element_factory_make(factory, name);
    Check(element && gst_bin_add(GST_BIN(value), element), "Cannot create CPU test element");
    return element;
  }
};

void CheckFailure(const absl::Status& failure, const char* context, const char* source, const char* detail) {
  Check(failure.code() == absl::StatusCode::kInternal, "Startup failure lost its status code");
  const std::string expected = std::string(context) + ": " + source + ": " + detail;
  if (failure.message() != expected)
    std::cerr << "Expected: " << expected << "\nActual: " << failure.message() << '\n';
  Check(failure.message() == expected, "Startup failure lost its context, element name or original GError");
}

void SynchronousFailure() {
  Pipeline pipeline;
  auto* sink = pipeline.Add("fakesink", "failed_model_start");
  g_object_set(sink, "state-error", 2, nullptr); // Fail READY -> PAUSED, like a model's Start callback.
  Check(
      gst_element_set_state(pipeline.value, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE,
      "Test pipeline did not fail its synchronous PAUSED transition");
  CheckFailure(
      hm::gst::PipelineStartupFailure(pipeline.value, "Failed to set pipeline to PAUSED"),
      "Failed to set pipeline to PAUSED",
      "failed_model_start",
      "GStreamer error: state change failed and some element failed to post a proper error message with the reason for "
      "the failure.");
}

struct BufferGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool released{false};

  static void Block(GstElement*, GstBuffer*, GstPad*, gpointer data) {
    auto& gate = *static_cast<BufferGate*>(data);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.changed.wait(lock, [&] { return gate.released; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

void PrerollFailure() {
  BufferGate gate;
  Pipeline pipeline;
  auto* source = pipeline.Add("fakesrc", "source");
  auto* failure = pipeline.Add("identity", "failed_model_preroll");
  auto* sink = pipeline.Add("fakesink", "sink");
  g_object_set(source, "num-buffers", 1, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(source, "handoff", G_CALLBACK(BufferGate::Block), &gate);
  g_object_set(failure, "error-after", 1, nullptr);
  Check(gst_element_link_many(source, failure, sink, nullptr), "Cannot link CPU preroll pipeline");
  const auto initial = gst_element_set_state(pipeline.value, GST_STATE_PAUSED);
  gate.Release();
  Check(initial == GST_STATE_CHANGE_ASYNC, "Test pipeline did not start asynchronous preroll");
  Check(
      gst_element_get_state(pipeline.value, nullptr, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_FAILURE,
      "Test pipeline did not report its preroll failure through get_state");
  CheckFailure(
      hm::gst::PipelineStartupFailure(pipeline.value, "Pipeline failed while initializing models"),
      "Pipeline failed while initializing models",
      "failed_model_preroll",
      "Failed after iterations as requested.");
}

void OriginalDiagnosticAndFallback() {
  constexpr const char* fallback = "Failed to set pipeline to PAUSED";
  Check(hm::gst::PipelineStartupFailure(nullptr, fallback).message() == fallback, "Missing bus lost fallback");
  Pipeline pipeline;
  auto* source = pipeline.Add("identity", "player_analytics");
  constexpr const char* diagnostic =
      "Player analytics failed: FAILED_PRECONDITION: "
      "prepared engine SDK/GPU identity does not match the loaded runtime";
  GError* error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, diagnostic);
  Check(
      gst_element_post_message(source, gst_message_new_error(GST_OBJECT(source), error, "startup diagnostic")),
      "Could not queue the original model diagnostic");
  g_error_free(error);
  CheckFailure(hm::gst::PipelineStartupFailure(pipeline.value, fallback), fallback, "player_analytics", diagnostic);
  // With no queued error, the zero-timeout read must return without waiting for
  // a later message or dispatching the main context to discover one.
  Check(hm::gst::PipelineStartupFailure(pipeline.value, fallback).message() == fallback, "Empty bus lost fallback");
  Check(
      gst_element_set_state(pipeline.value, GST_STATE_PAUSED) == GST_STATE_CHANGE_SUCCESS,
      "Reading a startup error altered pipeline state ownership");
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool dispatched = false;
  GSource* pending = g_idle_source_new();
  g_source_set_callback(
      pending,
      [](gpointer data) -> gboolean {
        *static_cast<bool*>(data) = true;
        return G_SOURCE_REMOVE;
      },
      &dispatched,
      nullptr);
  g_source_attach(pending, nullptr);
  SynchronousFailure();
  PrerollFailure();
  OriginalDiagnosticAndFallback();
  Check(!dispatched, "Startup error handling dispatched GLib callbacks");
  g_source_destroy(pending);
  g_source_unref(pending);
  return 0;
}
