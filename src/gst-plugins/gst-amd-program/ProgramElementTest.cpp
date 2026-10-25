#include "hstream/src/gst-plugins/gst-amd-program/ProgramElement.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Counts {
  unsigned reset{}, frames{};
  bool fail{}, throwing{}, block{};
  std::atomic<bool> blocked{false};
  std::mutex mutex;
  std::condition_variable wake;
  bool interrupted{};
};
class Counter : public hm::program::gst::Component {
 public:
  explicit Counter(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* buffer) override {
    if (counts_->block) {
      std::unique_lock<std::mutex> lock(counts_->mutex);
      counts_->blocked = true;
      counts_->wake.wait(lock, [&] { return counts_->interrupted; });
      counts_->blocked = false;
      return absl::CancelledError("Injected resource wait interrupted");
    }
    if (counts_->throwing)
      throw std::runtime_error("injected component exception");
    if (counts_->fail)
      return absl::DataLossError("injected component failure");
    Check(gst_buffer_is_writable(buffer), "Component received shared writable metadata");
    GST_BUFFER_OFFSET(buffer) += 1;
    ++counts_->frames;
    return buffer;
  }
  void Interrupt() noexcept override {
    std::lock_guard<std::mutex> lock(counts_->mutex);
    counts_->interrupted = true;
    counts_->wake.notify_all();
  }
  absl::Status Reset() override {
    std::lock_guard<std::mutex> lock(counts_->mutex);
    counts_->interrupted = false;
    ++counts_->reset;
    return absl::OkStatus();
  }

 private:
  std::shared_ptr<Counts> counts_;
};
struct Pipeline {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* source = gst_element_factory_make("appsrc", nullptr);
  GstElement* sink = gst_element_factory_make("appsink", nullptr);
  std::vector<std::shared_ptr<Counts>> counts;
  std::vector<GstElement*> stages;
  Pipeline() {
    Check(pipeline && source && sink, "Missing app plugin");
    gst_bin_add_many(GST_BIN(pipeline), source, sink, nullptr);
    g_object_set(source, "format", GST_FORMAT_TIME, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, nullptr);
    auto* caps = gst_caps_new_empty_simple("application/x-test-metadata");
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* previous = source;
    for (const auto* factory :
         {"hmamdstitch", "hmamdinfer", "hmrinkfilter", "hmobjecttrack", "hmcameratrack", "hmamdcrop", "hmrinkmap"}) {
      counts.push_back(std::make_shared<Counts>());
      auto result = hm::program::gst::CreateElement(factory, factory, std::make_unique<Counter>(counts.back()));
      Check(result.ok(), "Cannot construct Program component");
      auto* stage = *result;
      stages.push_back(stage);
      gst_bin_add(GST_BIN(pipeline), stage);
      Check(gst_element_link(previous, stage), "Cannot link components");
      previous = stage;
    }
    Check(gst_element_link(previous, sink), "Cannot link completed sink");
    Play();
  }
  ~Pipeline() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  void Play() {
    Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot play");
  }
  void Push(GstBuffer* input) {
    Check(gst_app_src_push_buffer(GST_APP_SRC(source), input) == GST_FLOW_OK, "Push failed");
  }
  GstSample* Pull() {
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
    Check(sample != nullptr, "No completed sample");
    return sample;
  }
};
GstBuffer* Input() {
  auto* input = gst_buffer_new();
  GST_BUFFER_OFFSET(input) = 0;
  GST_BUFFER_PTS(input) = 0;
  GST_BUFFER_DURATION(input) = GST_SECOND / 25;
  return input;
}
void Run() {
  {
    bool freed = false;
    auto* original = gst_buffer_new_allocate(nullptr, 16, nullptr);
    auto* memory = gst_buffer_peek_memory(original, 0);
    gst_mini_object_weak_ref(
        GST_MINI_OBJECT(original), [](gpointer data, GstMiniObject*) { *static_cast<bool*>(data) = true; }, &freed);
    auto header = hm::program::gst::WritableHeader(gst_buffer_ref(original));
    Check(header.ok() && gst_buffer_is_writable(*header), "Output header remains shared");
    Check(gst_buffer_peek_memory(*header, 0) == memory, "Output handoff copied frame memory");
    gst_buffer_unref(original);
    Check(!freed, "Output handoff released its source pool buffer early");
    gst_buffer_unref(*header);
    Check(freed, "Output handoff leaked its source pool buffer");
  }
  Check(hm::program::gst::RegisterProgramPlugin(), "Cannot register plugin");
  auto* unconfigured = gst_element_factory_make("hmamdinfer", nullptr);
  Check(unconfigured != nullptr, "Plugin factory is absent");
  Check(
      gst_element_set_state(unconfigured, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE,
      "Unconfigured component accepted streaming");
  gst_element_set_state(unconfigured, GST_STATE_NULL);
  gst_object_unref(unconfigured);
  {
    Pipeline graph;
    auto* retained = Input();
    graph.Push(gst_buffer_ref(retained));
    auto* sample = graph.Pull();
    Check(
        GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)) == graph.stages.size(),
        "Components did not execute in graph order");
    Check(GST_BUFFER_OFFSET(retained) == 0, "Shared upstream header was modified");
    Check(
        GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == 0 &&
            GST_BUFFER_DURATION(gst_sample_get_buffer(sample)) == GST_SECOND / 25,
        "Timeline changed through components");
    gst_sample_unref(sample);
    gst_buffer_unref(retained);
    for (auto* stage : graph.stages) {
      guint64 frames = 0;
      g_object_get(stage, "processed-frames", &frames, nullptr);
      Check(frames == 1, "Per-component diagnostics lost frame count");
    }
    Check(gst_element_set_state(graph.pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "Cannot reset graph");
    for (const auto& count : graph.counts)
      Check(count->reset > 0, "Timeline reset bypassed component state");
    graph.Play();
    graph.Push(Input());
    sample = graph.Pull();
    gst_sample_unref(sample);
    Check(gst_app_src_end_of_stream(GST_APP_SRC(graph.source)) == GST_FLOW_OK, "Cannot send EOS");
    auto* bus = gst_element_get_bus(graph.pipeline);
    auto* eos = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    Check(eos && GST_MESSAGE_TYPE(eos) == GST_MESSAGE_EOS, "EOS did not traverse component graph");
    gst_message_unref(eos);
    gst_object_unref(bus);
  }
  {
    Pipeline graph;
    graph.counts[0]->block = true;
    graph.Push(Input());
    for (unsigned i = 0; i < 200 && !graph.counts[0]->blocked; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Check(graph.counts[0]->blocked, "Resource-wait fixture never blocked");
    auto* pad = gst_element_get_static_pad(graph.source, "src");
    Check(gst_pad_push_event(pad, gst_event_new_flush_start()), "Flush did not interrupt resource wait");
    gst_object_unref(pad);
    Check(
        gst_element_set_state(graph.pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE,
        "Blocked graph could not stop");
    Check(!graph.counts[0]->blocked, "Streaming remained blocked after stop");
    graph.counts[0]->block = false;
    graph.Play();
    graph.Push(Input());
    auto* sample = graph.Pull();
    gst_sample_unref(sample);
  }
  for (const bool throwing : {false, true}) {
    Pipeline graph;
    graph.counts[2]->fail = !throwing;
    graph.counts[2]->throwing = throwing;
    graph.Push(Input());
    auto* bus = gst_element_get_bus(graph.pipeline);
    auto* error = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GST_MESSAGE_ERROR);
    Check(error != nullptr, "Component failure did not reach the bus");
    gst_message_unref(error);
    gst_object_unref(bus);
    Check(graph.counts[3]->frames == 0, "Failure escaped to a downstream component");
  }
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    Run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
