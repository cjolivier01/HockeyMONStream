#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"
#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"
#include "hstream/src/libs/common/PacedPlayback.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using Owner = hm::preview::RouteOwner;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Route {
  GstElement *ingress, *queue, *drain;
  GstPad *input, *selected;
  std::uint64_t operation{0};
};
struct Fixture {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* record = gst_element_factory_make("identity", "completed-video");
  GstElement* source_sink = gst_element_factory_make("fakesink", nullptr);
  GstElement* selected =
      hm::preview::selected_stream_create("selected", "retirement", GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY, "program");
  GstElement* preview_sink = gst_element_factory_make("fakesink", nullptr);
  GstElement* audio = gst_element_factory_make("identity", "source-pcm");
  GstElement* encoder = gst_element_factory_make("identity", "transport-encoder");
  GstElement* audio_sink = gst_element_factory_make("fakesink", nullptr);
  std::array<Route, 2> routes;
  std::shared_ptr<hm::gst::PacedPlayback> paced;
  Owner owner;
  std::mutex mutex;
  std::condition_variable changed;
  unsigned mapping_calls{0}, audio_frames{0};
  std::atomic<unsigned> shown{0};
  std::atomic<bool> exact_identity{false};

  Fixture() {
    gst_bin_add_many(
        GST_BIN(pipeline), record, source_sink, selected, preview_sink, audio, encoder, audio_sink, nullptr);
    require(
        gst_element_link(record, source_sink) && gst_element_link(selected, preview_sink) &&
            gst_element_link_many(audio, encoder, audio_sink, nullptr),
        "Could not link retirement fixture");
    for (auto* sink : {source_sink, preview_sink, audio_sink})
      g_object_set(sink, "async", FALSE, "sync", FALSE, "signal-handoffs", TRUE, nullptr);
    auto* pad = gst_element_get_static_pad(record, "src");
    paced =
        hm::gst::PacedPlayback::Create(pipeline, pad, {20000, 5000, 0, 128, 200 * GST_MSECOND, 1, 4096, 2, GST_SECOND});
    gst_object_unref(pad);
    require(paced && paced->AttachAudioTransport(encoder), "Could not attach shared PCM mapper");
    require(
        hm::preview::selected_stream_set_presentation_mapper(
            selected,
            [this](
                const GstSegment& segment, GstClockTime pts, const std::atomic<bool>& cancelled, GstClockTime* output) {
              {
                std::lock_guard<std::mutex> lock(mutex);
                ++mapping_calls;
                changed.notify_all();
              }
              return paced->PresentationTime(segment, pts, output, &cancelled);
            },
            [this] { paced->WakePresentationWaiters(); }),
        "Could not install route mapping cancellation");
    owner.initialize_selection("none", 0);
    for (unsigned i = 0; i < routes.size(); ++i) {
      auto& route = routes[i];
      const char* channel = i ? "source0" : "program";
      route.ingress = gst_element_factory_make("hmpreviewisolation", nullptr);
      route.queue = gst_element_factory_make("queue", nullptr);
      route.drain = gst_element_factory_make("hmpreviewisolation", nullptr);
      g_object_set(route.queue, "max-size-buffers", 1U, "max-size-bytes", 0U, "max-size-time", guint64{0}, nullptr);
      gst_bin_add_many(GST_BIN(pipeline), route.ingress, route.queue, route.drain, nullptr);
      route.input = gst_pad_new(nullptr, GST_PAD_SRC);
      route.selected = hm::preview::selected_stream_add_route(selected, channel);
      auto* input = gst_element_get_static_pad(route.ingress, "sink");
      auto* output = gst_element_get_static_pad(route.drain, "src");
      require(
          gst_element_link_many(route.ingress, route.queue, route.drain, nullptr) &&
              gst_pad_link(route.input, input) == GST_PAD_LINK_OK &&
              gst_pad_link(output, route.selected) == GST_PAD_LINK_OK,
          "Could not attach real preview gates");
      gst_object_unref(input);
      gst_object_unref(output);
      gst_pad_set_active(route.input, TRUE);
      require(
          owner.register_route(
              channel,
              {
                  [this, &route](bool active, std::uint64_t operation) {
                    if (!active)
                      hm::preview::selected_stream_cancel_presentation(selected, route.selected, operation);
                    hm::gpu_preview::set_isolation_active(route.ingress, active, operation);
                  },
                  [this, &route](bool active, std::uint64_t operation) {
                    if (!active)
                      hm::preview::selected_stream_cancel_presentation(selected, route.selected, operation);
                    hm::gpu_preview::set_isolation_active(route.drain, active, operation);
                  },
                  [this, &route] {
                    return hm::gpu_preview::isolation_active(route.ingress) &&
                        hm::gpu_preview::isolation_active(route.drain) &&
                        hm::preview::selected_stream_active(selected, route.selected, route.operation);
                  },
                  [this, &route](std::uint64_t operation) {
                    route.operation = operation;
                    hm::preview::selected_stream_select(selected, route.selected, operation);
                  },
                  [this, &route](std::uint64_t operation) {
                    route.operation = operation;
                    hm::gpu_preview::set_isolation_generation(route.ingress, operation);
                    hm::gpu_preview::set_isolation_generation(route.drain, operation);
                    return hm::preview::selected_stream_select(selected, route.selected, operation);
                  },
                  [this](std::uint64_t operation) {
                    return hm::preview::selected_stream_select(selected, nullptr, operation);
                  },
              }),
          "Could not register real gated route");
    }
    g_signal_connect(
        preview_sink,
        "handoff",
        G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
          auto& self = *static_cast<Fixture*>(data);
          const auto* identity = hm::preview::frame_identity(buffer);
          self.exact_identity =
              identity && identity->sequence == 1 && identity->operation == 6 && identity->source_pts == 0;
          ++self.shown;
        }),
        this);
    g_signal_connect(
        audio_sink,
        "handoff",
        G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
          auto& self = *static_cast<Fixture*>(data);
          std::lock_guard<std::mutex> lock(self.mutex);
          self.audio_frames += gst_buffer_get_size(buffer) / (2 * sizeof(float));
          self.changed.notify_all();
        }),
        this);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Could not play fixture");
    headers(record, "video/x-raw,format=RGB,width=2,height=2,framerate=30/1");
    headers(audio, "audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
    for (auto& route : routes) {
      gst_pad_push_event(route.input, gst_event_new_stream_start("route"));
      auto* caps = gst_caps_from_string("video/x-raw,format=RGB,width=2,height=2,framerate=30/1");
      gst_pad_push_event(route.input, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      gst_pad_push_event(route.input, gst_event_new_segment(&segment));
    }
  }
  ~Fixture() {
    paced->Cancel(); // Failure-only escape also prevents a failed regression hanging teardown.
    for (auto& route : routes) {
      hm::preview::selected_stream_cancel_presentation(selected, route.selected, G_MAXUINT64);
      hm::gpu_preview::set_isolation_active(route.ingress, false, G_MAXUINT64);
      hm::gpu_preview::set_isolation_active(route.drain, false, G_MAXUINT64);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    for (auto& route : routes) {
      gst_pad_set_active(route.input, FALSE);
      gst_object_unref(route.input);
      gst_object_unref(route.selected);
    }
    gst_object_unref(pipeline);
  }
  static void headers(GstElement* element, const char* description) {
    auto* pad = gst_element_get_static_pad(element, "sink");
    gst_pad_send_event(pad, gst_event_new_stream_start(description));
    auto* caps = gst_caps_from_string(description);
    gst_pad_send_event(pad, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_pad_send_event(pad, gst_event_new_segment(&segment));
    gst_object_unref(pad);
  }
  void wait_mapping(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    require(changed.wait_for(lock, 1s, [&] { return mapping_calls >= count; }), "Gated route did not enter mapper");
  }
  void push_route(unsigned route, GstClockTime pts) {
    auto* buffer = gst_buffer_new_allocate(nullptr, 12, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = 30 * GST_MSECOND;
    require(gst_pad_push(routes[route].input, buffer) == GST_FLOW_OK, "Real gate rejected route input");
  }
  static void push(GstElement* element, GstClockTime pts, GstClockTime duration, size_t bytes) {
    auto* pad = gst_element_get_static_pad(element, "sink");
    auto* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = duration;
    const auto result = gst_pad_chain(pad, buffer);
    gst_object_unref(pad);
    require(result == GST_FLOW_OK, "Fixture source input failed");
  }
  template <typename Work>
  void bounded(Work work, const char* message) {
    auto pending = std::async(std::launch::async, work);
    const bool timely = pending.wait_for(100ms) == std::future_status::ready;
    if (!timely)
      paced->Cancel(); // Release an unfixed real gate before reporting failure.
    pending.get();
    require(timely, message);
  }
};

void run() {
  Fixture f;
  require(f.owner.select_current("program", 1, [] {}).status == Owner::Status::kActivated, "Could not select Program");
  Fixture::push(f.audio, 0, 30 * GST_MSECOND, 1440 * 2 * sizeof(float)); // queued PCM awaits completed video.
  f.push_route(0, GST_SECOND);
  f.wait_mapping(1);
  f.push_route(0, GST_SECOND + 30 * GST_MSECOND); // fill the queue behind the blocked drain.
  auto blocked_ingress = std::async(std::launch::async, [&] { f.push_route(0, GST_SECOND + 60 * GST_MSECOND); });
  const bool queue_full = blocked_ingress.wait_for(30ms) == std::future_status::timeout;
  if (!queue_full)
    f.paced->Cancel();
  require(queue_full, "Real ingress did not block on the full route queue");
  require(gst_element_set_state(f.pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE, "Could not pause fixture");
  require(gst_element_get_state(f.pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_FAILURE, "Pause failed");
  f.bounded(
      [&] {
        require(
            f.owner.select_current("source0", 2, [] {}).status == Owner::Status::kActivated,
            "Paused route switch failed");
      },
      "Paused route switch blocked behind real isolation gate");
  require(
      blocked_ingress.wait_for(100ms) == std::future_status::ready,
      "Route cancellation failed to release the full-queue ingress push");
  blocked_ingress.get();
  require(f.shown == 0, "Retired mapping consumed a video identity");
  // The successor has a fresh cancellation token, including while PAUSED.
  f.push_route(1, 2 * GST_SECOND);
  f.wait_mapping(2);
  f.bounded(
      [&] {
        require(
            f.owner.select_current("source0", 3, [] {}).status == Owner::Status::kActivated,
            "Paused same-route rearm failed");
      },
      "Paused same-route rearm blocked behind mapping");
  require(
      !hm::preview::selected_stream_cancel_presentation(f.selected, f.routes[1].selected, 2),
      "A stale cancellation retired the successor operation");
  f.push_route(1, 2500 * GST_MSECOND);
  f.wait_mapping(3);
  f.bounded(
      [&] {
        require(
            f.owner.select_current("none", 4, [] {}).status == Owner::Status::kDeactivated, "Paused Render off failed");
      },
      "Paused Render off blocked behind real isolation gate");
  require(f.owner.select_current("program", 5, [] {}).status == Owner::Status::kActivated, "Render on failed");
  f.push_route(0, 3 * GST_SECOND);
  f.wait_mapping(4);
  // Match WebPreviewSession::Stop's order rather than cancelling the shared owner.
  f.bounded(
      [&] {
        for (auto& route : f.routes) {
          hm::preview::selected_stream_cancel_presentation(f.selected, route.selected, 5);
          hm::gpu_preview::set_isolation_active(route.ingress, false, 5);
          hm::gpu_preview::set_isolation_active(route.drain, false, 5);
        }
        require(hm::preview::selected_stream_select(f.selected, nullptr, 5), "Stop could not retire selected route");
      },
      "Paused Stop blocked behind real isolation gate");
  require(f.shown == 0, "A cancelled frame reached the preview sink");
  require(
      gst_element_set_state(f.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Could not resume shared PCM");
  Fixture::push(f.record, 0, 30 * GST_MSECOND, 12);
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  GstClockTime presentation;
  require(
      f.paced->PresentationTime(segment, 0, &presentation) == GST_FLOW_OK,
      "Route retirement cancelled the shared playback epoch");
  {
    std::unique_lock<std::mutex> lock(f.mutex);
    require(
        f.changed.wait_for(lock, 1s, [&] { return f.audio_frames == 1440; }),
        "Route retirement discarded or blocked pending shared PCM");
  }
  require(
      f.owner.select_current("source0", 6, [] {}).status == Owner::Status::kActivated,
      "A later selection could not rearm after paused Stop");
  f.push_route(1, 0);
  f.wait_mapping(5);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!f.shown && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  require(
      f.shown == 1 && f.exact_identity,
      "A fresh route token failed to retain exact source/operation identity without consuming cancelled identities");
  std::cout << "Real gates: paused switch, Render off/on and Stop retire within 100 ms; "
               "fresh mapping and all 1440 pending PCM frames survive in the shared epoch\n";
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!hm::gpu_preview::register_elements())
    return 1;
  try {
    run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
