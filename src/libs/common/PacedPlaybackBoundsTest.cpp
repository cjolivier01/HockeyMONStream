#include "hstream/src/libs/common/PacedPlayback.h"

#include <gst/base/gstbasesink.h>

#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool value, const char* detail) {
  if (!value)
    throw std::runtime_error(detail);
}

struct Fixture {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* ingress = gst_element_factory_make("identity", nullptr);
  GstElement* source_sink = gst_element_factory_make("fakesink", nullptr);
  GstElement* route = gst_element_factory_make("identity", nullptr);
  GstElement* sink = gst_element_factory_make("fakesink", nullptr);
  GstElement* audio = nullptr;
  GstElement* audio_sink = nullptr;
  std::atomic<unsigned> presented{0}, audio_entered{0};
  std::array<std::atomic<unsigned>, 3> audio_presented{};
  std::shared_ptr<hm::gst::PacedPlayback> owner;

  explicit Fixture(bool with_audio = false) {
    Check(pipeline && ingress && source_sink && route && sink, "Missing test elements");
    g_object_set(source_sink, "sync", TRUE, "async", FALSE, nullptr);
    g_object_set(sink, "sync", TRUE, "async", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), ingress, source_sink, route, sink, nullptr);
    Check(gst_element_link(ingress, source_sink) && gst_element_link(route, sink), "Could not link test branches");
    auto* pad = gst_element_get_static_pad(ingress, "src");
    owner = hm::gst::PacedPlayback::Create(pipeline, pad, {16683, 4170, 0, 2, GST_SECOND, 1, 16384, 2, 3 * GST_SECOND});
    gst_object_unref(pad);
    Check(owner && owner->AttachVideoSink(sink, 2), "Could not attach bounded presentation owner");
    g_object_set(sink, "signal-handoffs", TRUE, nullptr);
    g_signal_connect(
        sink,
        "handoff",
        G_CALLBACK(+[](GstElement*, GstBuffer*, GstPad*, gpointer data) { ++static_cast<Fixture*>(data)->presented; }),
        this);
    if (with_audio) {
      audio = gst_element_factory_make("identity", nullptr);
      audio_sink = gst_element_factory_make("fakesink", nullptr);
      Check(audio && audio_sink, "Missing CPU audio elements");
      g_object_set(audio_sink, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, nullptr);
      gst_bin_add_many(GST_BIN(pipeline), audio, audio_sink, nullptr);
      Check(
          gst_element_link(audio, audio_sink) && owner->AttachAudioTransport(audio_sink),
          "Could not attach PCM mapper");
      g_signal_connect(
          audio_sink,
          "handoff",
          G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
            guint16 marker = 0;
            gst_buffer_extract(buffer, 0, &marker, sizeof(marker));
            if (marker >= 1 && marker <= 3)
              ++static_cast<Fixture*>(data)->audio_presented[marker - 1];
          }),
          this);
      auto* output = gst_element_get_static_pad(audio_sink, "sink");
      auto* peer = gst_pad_get_peer(output);
      auto* mapper = gst_pad_get_parent_element(peer);
      auto* input = gst_element_get_static_pad(mapper, "sink");
      gst_pad_add_probe(
          input,
          GST_PAD_PROBE_TYPE_BUFFER,
          +[](GstPad*, GstPadProbeInfo*, gpointer data) {
            ++static_cast<Fixture*>(data)->audio_entered;
            return GST_PAD_PROBE_OK;
          },
          this,
          nullptr);
      gst_object_unref(input);
      gst_object_unref(mapper);
      gst_object_unref(peer);
      gst_object_unref(output);
    }
    Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Could not start test graph");
    for (auto* input : {ingress, route}) {
      auto* pad = gst_element_get_static_pad(input, "sink");
      gst_pad_send_event(pad, gst_event_new_stream_start(input == ingress ? "source" : "route"));
      auto* caps = gst_caps_from_string("video/x-raw,format=RGBA,width=1,height=1,framerate=0/1");
      gst_pad_send_event(pad, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      gst_pad_send_event(pad, gst_event_new_segment(&segment));
      gst_object_unref(pad);
    }
    if (with_audio) {
      Event(audio, gst_event_new_stream_start("audio"));
      auto* caps = gst_caps_from_string("audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1");
      Event(audio, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      Segment(audio);
    }
  }
  ~Fixture() {
    if (owner)
      owner->Cancel();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  void Event(GstElement* input, GstEvent* event) {
    auto* pad = gst_element_get_static_pad(input, "sink");
    const auto accepted = gst_pad_send_event(pad, event);
    gst_object_unref(pad);
    Check(accepted, "Test event was rejected");
  }
  void Segment(GstElement* input) {
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    Event(input, gst_event_new_segment(&segment));
  }
  void Audio(guint16 marker) {
    std::array<guint16, 480> samples;
    samples.fill(marker);
    auto* buffer = gst_buffer_new_allocate(nullptr, sizeof(samples), nullptr);
    gst_buffer_fill(buffer, 0, samples.data(), sizeof(samples));
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
    auto* pad = gst_element_get_static_pad(audio, "sink");
    const auto result = gst_pad_chain(pad, buffer);
    gst_object_unref(pad);
    Check(result == GST_FLOW_OK, "PCM input was rejected");
  }
  void Wait(const std::atomic<unsigned>& observed, unsigned expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (observed.load() < expected && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(observed.load() == expected, "CPU branch did not reach its expected buffer");
  }
  void Push(GstElement* input, guint frame, GstClockTime duration = 10 * GST_MSECOND) {
    auto* pad = gst_element_get_static_pad(input, "sink");
    auto* buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) = frame * 10 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = duration;
    const auto result = gst_pad_chain(pad, buffer);
    gst_object_unref(pad);
    Check(result == GST_FLOW_OK, "Test input did not reach its pad probe");
  }
  void Error(const char* expected) {
    auto* bus = gst_element_get_bus(pipeline);
    auto* message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    Check(message, "Invalid live presentation did not report a stream error");
    GError* error{};
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    const bool matched = debug && std::string(debug).find(expected) != std::string::npos;
    g_clear_error(&error);
    g_free(debug);
    gst_message_unref(message);
    Check(matched, "Presentation failed for an unexpected reason");
  }
};
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    bool seek_regressions_passed = true;
    for (const unsigned preroll_segments : {0U, 1U, 2U}) {
      try {
        Fixture test(true);
        g_object_set(test.source_sink, "sync", FALSE, nullptr);
        g_object_set(test.sink, "sync", FALSE, nullptr);
        test.Audio(1);
        test.Push(test.ingress, 0);
        test.Push(test.route, 0);
        test.Wait(test.audio_presented[0], 1);
        Check(test.presented == 1, "Initial retained preview did not present");
        for (unsigned seek = 0; seek < 6; ++seek) {
          test.owner->Cancel();
          // The retained sink still has its previous SEGMENT. Even the same PTS
          // must remain cancelled until that sink receives the new segment.
          test.Push(test.route, 0);
          Check(test.presented == seek + 1, "An old-epoch video frame escaped cancellation");
          for (auto* input : {test.ingress, test.route, test.audio})
            test.Event(input, gst_event_new_flush_start());
          for (auto* input : {test.ingress, test.route, test.audio})
            test.Event(input, gst_event_new_flush_stop(TRUE));
          test.Segment(test.route);
          // Stitched may see its SEGMENT before Program. Program can also
          // receive one or more segments while replacement sources preroll,
          // before any video interval is available to map audio or previews.
          for (unsigned segment = 0; segment < preroll_segments; ++segment)
            test.Segment(test.ingress);
          test.Segment(test.audio);
          test.Audio(2);
          test.Wait(test.audio_entered, 2 + 2 * seek);
          Check(test.audio_presented[1] == 0, "Unmapped preroll PCM was presented");
          // A source's local preroll seek can flush audio after Stitched already
          // received its segment, but before Program establishes the mapping.
          // This must retire the pending PCM without changing the reserved video
          // epoch. FLUSH_STOP also joins the mapper's pending chain invocation.
          test.Event(test.audio, gst_event_new_flush_start());
          test.Event(test.audio, gst_event_new_flush_stop(TRUE));
          test.Segment(test.audio);
          test.Audio(3);
          test.Segment(test.ingress);
          test.Push(test.ingress, 0);
          test.Push(test.route, 0);
          test.Wait(test.audio_presented[2], seek + 1);
          Check(test.presented == seek + 2, "Audio preroll flush stranded the retained preview epoch");
          Check(test.audio_presented[1] == 0, "Cancelled preroll PCM resumed in the new mapping");
        }
        std::cout << "Six retained-preview seeks survived " << preroll_segments
                  << " preroll video segments and late audio flushes without reviving old video or PCM\n";
      } catch (const std::exception& error) {
        std::cerr << "Preroll video segments=" << preroll_segments << ": " << error.what() << '\n';
        seek_regressions_passed = false;
      }
    }
    try {
      Fixture test;
      g_object_set(test.sink, "sync", FALSE, nullptr);
      // Initial preroll also has no committed video interval. Duplicate
      // segments must not strand a preview which already received its segment.
      // Rejected video cannot commit that interval either.
      test.Push(test.ingress, 0, GST_CLOCK_TIME_NONE);
      test.Error("finite source intervals");
      test.Segment(test.ingress);
      test.Segment(test.ingress);
      test.Push(test.ingress, 0);
      test.Push(test.route, 0);
      Check(test.presented == 1, "Initial duplicate segments stranded the preview epoch");
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      seek_regressions_passed = false;
    }
    Check(seek_regressions_passed, "Retained preview preroll regressions failed");
    {
      Fixture test;
      g_object_set(test.sink, "sync", FALSE, nullptr);
      test.Push(test.ingress, 0);
      test.Push(test.route, 0);
      Check(test.presented == 1, "Initial committed preview did not present");
      test.owner->Cancel();
      test.Event(test.ingress, gst_event_new_flush_start());
      test.Event(test.ingress, gst_event_new_flush_stop(TRUE));
      test.Segment(test.ingress);
      test.Push(test.ingress, 0);
      // A real flush after recorded video must advance the epoch. Even after
      // the new mapping opens, a retained old-segment frame must stay fenced.
      test.Push(test.route, 0);
      Check(test.presented == 1, "Old video resumed after the replacement mapping opened");
      test.Segment(test.route);
      test.Push(test.route, 0);
      Check(test.presented == 2, "Replacement-segment video did not present");
      // An unflushed new segment after recorded video also begins a new epoch.
      test.Segment(test.ingress);
      test.Push(test.ingress, 0);
      test.Push(test.route, 0);
      Check(test.presented == 2, "Old video resumed after an unflushed committed segment");
      test.Segment(test.route);
      test.Push(test.route, 0);
      Check(test.presented == 3, "New unflushed-segment video did not present");
    }
    {
      Fixture test;
      for (guint frame = 0; frame < 4; ++frame)
        test.Push(test.ingress, frame);
      test.Push(test.route, 0);
      test.Error("bounded history");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0);
      Check(gst_element_send_event(test.sink, gst_event_new_latency(100 * GST_MSECOND)), "Device lead event rejected");
      Check(gst_base_sink_get_latency(GST_BASE_SINK(test.sink)) == 100 * GST_MSECOND, "Device lead was not negotiated");
      test.Push(test.route, 0);
      test.Error("bounded GPU retention");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0, GST_CLOCK_TIME_NONE);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      auto* pad = gst_element_get_static_pad(test.ingress, "sink");
      for (const char* description :
           {"video/x-raw,format=RGBA,width=1,height=1,framerate=30/1",
            "video/x-raw,format=RGBA,width=1,height=1,framerate=0/1"}) {
        auto* caps = gst_caps_from_string(description);
        Check(gst_pad_send_event(pad, gst_event_new_caps(caps)), "Test CAPS rejected");
        gst_caps_unref(caps);
      }
      gst_object_unref(pad);
      test.Push(test.ingress, 0, GST_CLOCK_TIME_NONE);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0, guint64(INT64_MAX) + 1);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      test.Push(test.ingress, 0);
      std::atomic<bool> retired{false};
      auto waiting = std::async(std::launch::async, [&] {
        GstClockTime presentation;
        return test.owner->PresentationTime(segment, GST_SECOND, &presentation, &retired);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      Check(
          waiting.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout,
          "An unadmitted route must wait for its shared source interval");
      Check(
          gst_element_set_state(test.pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE,
          "Could not pause shared mapping fixture");
      retired.store(true, std::memory_order_release);
      test.owner->WakePresentationWaiters();
      Check(
          waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready &&
              waiting.get() == GST_FLOW_FLUSHING,
          "Retiring one paused route must promptly release only its mapping wait");
      GstClockTime presentation;
      Check(
          test.owner->PresentationTime(segment, 0, &presentation) == GST_FLOW_OK,
          "Retiring a preview route must retain the shared mapping history and epoch");
    }
    {
      Fixture test;
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      auto waiting = std::async(std::launch::async, [&] {
        GstClockTime presentation;
        return test.owner->PresentationTime(segment, GST_SECOND, &presentation);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      Check(
          waiting.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout,
          "Unadmitted transport video must wait for its shared source interval");
      test.owner->Cancel();
      Check(
          waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready &&
              waiting.get() == GST_FLOW_FLUSHING,
          "Cancellation must retire transport mapping waits promptly");
    }
    std::cout << "History eviction, device lead and unknown/overflowing source duration fail explicitly\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
