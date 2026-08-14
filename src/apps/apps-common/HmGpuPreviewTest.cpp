#include "hstream/src/apps/apps-common/HmGpuPreview.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

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
  gst_object_unref(sink);
  const auto [minimum, maximum] = rgba.empty()
      ? std::pair<std::uint8_t, std::uint8_t>{0, 0}
      : std::minmax({*std::min_element(rgba.begin(), rgba.end()), *std::max_element(rgba.begin(), rgba.end())});

  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  if (!ready || !captured || width != 640 || height != 360 || maximum == minimum) {
    std::cerr << "GPU preview did not expose its presented texture: ready=" << ready << " captured=" << captured
              << " range=" << static_cast<int>(maximum - minimum) << " error=" << capture_error << '\n';
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
  if (!run_two_stage_disabled_path_test())
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
  XStoreName(display, window, "hstream-gpu-preview-test");
  XMapRaised(display, window);
  XSync(display, False);

  const bool passed = run_renderer_test(display, window);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return passed ? 0 : 1;
}
