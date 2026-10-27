#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"
#include "hstream/src/libs/common/PacedPlayback.h"

#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr unsigned kRate = 48000, kVideoFrames = 90, kSamples = kVideoFrames * 800;
struct Receipt {
  GstClockTime source, presentation;
};
struct Capture {
  std::mutex mutex;
  std::vector<Receipt> video;
  std::vector<float> audio;
  std::vector<GstClockTime> audio_times;
  GstClockTime first_audio{GST_CLOCK_TIME_NONE};
  unsigned video_count{0};
  bool stall{false}, contiguous{true};
  GstClockTime audio_end{GST_CLOCK_TIME_NONE};
};
bool require(bool value, const char* detail) {
  if (!value)
    std::cerr << detail << '\n';
  return value;
}

bool run_allocation_query() {
  // The first PCM packet spans a video interval which has not arrived yet.
  // A converter's serialized allocation query must not wait behind that PCM:
  // the shared source cannot produce the next video until its query returns.
  constexpr unsigned kFrames = 960;
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<float> samples;
    bool exact_stereo{true};
  } state;
  auto* pipeline = gst_pipeline_new("paced-allocation-query");
  auto* video = gst_element_factory_make("appsrc", "video-source");
  auto* video_sink = gst_element_factory_make("fakesink", "video-output");
  auto* audio = gst_element_factory_make("appsrc", "audio-source");
  auto* upstream = gst_element_factory_make("identity", "allocation-query-source");
  auto* encoder = gst_element_factory_make("identity", "raw-encoder-boundary");
  auto* audio_sink = gst_element_factory_make("fakesink", "audio-output");
  gst_bin_add_many(GST_BIN(pipeline), video, video_sink, audio, upstream, encoder, audio_sink, nullptr);
  const bool linked =
      gst_element_link(video, video_sink) && gst_element_link_many(audio, upstream, encoder, audio_sink, nullptr);
  auto* video_pad = gst_element_get_static_pad(video, "src");
  auto owner = hm::gst::PacedPlayback::Create(
      pipeline, video_pad, {20000, 5000, 0, 128, 2 * GST_SECOND, 1, 4096, 2, GST_SECOND});
  gst_object_unref(video_pad);
  if (!require(linked && owner && owner->AttachAudioTransport(encoder), "Could not construct allocation fixture")) {
    gst_object_unref(pipeline);
    return false;
  }
  for (auto* source : {video, audio})
    g_object_set(source, "format", GST_FORMAT_TIME, "block", TRUE, "max-bytes", guint64{4096}, nullptr);
  auto* caps = gst_caps_from_string("video/x-raw,format=RGB,width=2,height=2,framerate=100/1");
  gst_app_src_set_caps(GST_APP_SRC(video), caps);
  gst_caps_unref(caps);
  caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
  gst_app_src_set_caps(GST_APP_SRC(audio), caps);
  for (auto* sink : {video_sink, audio_sink})
    g_object_set(sink, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(
      audio_sink,
      "handoff",
      G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
        auto& state = *static_cast<State*>(data);
        GstMapInfo bytes{};
        if (!gst_buffer_map(buffer, &bytes, GST_MAP_READ))
          return;
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto* samples = reinterpret_cast<const float*>(bytes.data);
        for (size_t i = 0; i < bytes.size / sizeof(float); i += 2) {
          state.exact_stereo &= samples[i] == samples[i + 1];
          if (samples[i] != 0) // A delayed video interval may add transport silence.
            state.samples.push_back(samples[i]);
        }
        gst_buffer_unmap(buffer, &bytes);
        state.changed.notify_all();
      }),
      &state);
  const auto push_video = [&](GstClockTime pts) {
    auto* buffer = gst_buffer_new_allocate(nullptr, 12, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
    return gst_app_src_push_buffer(GST_APP_SRC(video), buffer) == GST_FLOW_OK;
  };
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE && push_video(0);
  auto* buffer = gst_buffer_new_allocate(nullptr, kFrames * 2 * sizeof(float), nullptr);
  GstMapInfo bytes{};
  gst_buffer_map(buffer, &bytes, GST_MAP_WRITE);
  auto* samples = reinterpret_cast<float*>(bytes.data);
  for (unsigned i = 0; i < kFrames; ++i)
    samples[2 * i] = samples[2 * i + 1] = float(i + 1);
  gst_buffer_unmap(buffer, &bytes);
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
  ok &= gst_app_src_push_buffer(GST_APP_SRC(audio), buffer) == GST_FLOW_OK;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    ok &= require(
        state.changed.wait_for(lock, std::chrono::seconds(1), [&] { return state.samples.size() >= 480; }) &&
            state.samples.size() == 480,
        "First PCM interval did not stop at the missing video interval");
  }
  std::atomic<bool> query_finished{false};
  bool query_result = true;
  auto* query_pad = gst_element_get_static_pad(upstream, "src");
  std::thread query([&] {
    auto* allocation = gst_query_new_allocation(caps, TRUE);
    query_result = gst_pad_peer_query(query_pad, allocation);
    gst_query_unref(allocation);
    query_finished.store(true, std::memory_order_release);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!query_finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool prompt = query_finished.load(std::memory_order_acquire);
  ok &= require(prompt, "Serialized allocation query waited behind PCM which needs the next source video");
  if (!prompt)
    owner->Cancel(); // Failure-only teardown must release the pre-fix query.
  query.join();
  ok &= require(!query_result, "Private PCM allocation must return the bounded upstream fallback");
  gst_object_unref(query_pad);
  gst_caps_unref(caps);
  if (ok) {
    // No cancellation or epoch reset is permitted on the successful path.
    ok &= push_video(10 * GST_MSECOND);
    gst_app_src_end_of_stream(GST_APP_SRC(video));
    gst_app_src_end_of_stream(GST_APP_SRC(audio));
    auto* bus = gst_element_get_bus(pipeline);
    auto* message =
        gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    ok &= require(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "Allocation fixture did not reach EOS");
    if (message)
      gst_message_unref(message);
    gst_object_unref(bus);
  }
  owner->Cancel();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  ok &= require(state.exact_stereo && state.samples.size() == kFrames, "Allocation query lost retained PCM");
  for (unsigned i = 0; i < state.samples.size(); ++i)
    ok &= require(state.samples[i] == float(i + 1), "Allocation query reordered or changed source PCM");
  std::cout << "allocation query prompt=" << prompt << " fallback=" << !query_result
            << " source-samples=" << state.samples.size() << '\n';
  gst_object_unref(pipeline);
  return ok;
}

bool run(bool stall, bool pause = false, GstClockTime origin = 0) {
  Capture capture;
  capture.stall = stall;
  GstElement* pipeline = gst_pipeline_new(nullptr);
  auto* video = gst_element_factory_make("appsrc", "video-source");
  auto* processing = gst_element_factory_make("identity", "processing");
  auto* selected =
      hm::preview::selected_stream_create("selected", "paced-cpu-preview", "memory:SystemMemory", "program");
  auto* video_sink = gst_element_factory_make("fakesink", "video-output");
  auto* audio = gst_element_factory_make("appsrc", "audio-source");
  auto* encoder = gst_element_factory_make("identity", "raw-encoder-boundary");
  auto* audio_sink = gst_element_factory_make("fakesink", "audio-output");
  gst_bin_add_many(GST_BIN(pipeline), video, processing, selected, video_sink, audio, encoder, audio_sink, nullptr);
  auto* route = hm::preview::selected_stream_add_route(selected, "program");
  auto* source = gst_element_get_static_pad(processing, "src");
  bool linked = gst_element_link(video, processing) && gst_pad_link(source, route) == GST_PAD_LINK_OK &&
      gst_element_link(selected, video_sink) && gst_element_link_many(audio, encoder, audio_sink, nullptr);
  const hm::gst::PacedPlaybackSettings bounds{20000, 5000, 0, 128, 2 * GST_SECOND, 1, 4096, 2, 2 * GST_SECOND};
  auto owner = hm::gst::PacedPlayback::Create(pipeline, source, bounds);
  gst_object_unref(source);
  if (!require(linked && owner && owner->AttachAudioTransport(encoder), "Could not attach CPU transport fixture"))
    return false;
  hm::preview::selected_stream_set_presentation_mapper(
      selected,
      [owner](const GstSegment& segment, GstClockTime pts, const std::atomic<bool>& cancelled, GstClockTime* output) {
        return owner->PresentationTime(segment, pts, output, &cancelled);
      },
      [owner] { owner->WakePresentationWaiters(); });
  hm::preview::selected_stream_select(selected, route, 1);
  for (auto* appsrc : {video, audio})
    g_object_set(
        appsrc,
        "format",
        GST_FORMAT_TIME,
        "block",
        TRUE,
        "max-bytes",
        guint64{4096},
        "handle-segment-change",
        TRUE,
        nullptr);
  auto* caps = gst_caps_from_string("video/x-raw,format=RGB,width=2,height=2,framerate=60/1");
  gst_app_src_set_caps(GST_APP_SRC(video), caps);
  gst_caps_unref(caps);
  caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
  gst_app_src_set_caps(GST_APP_SRC(audio), caps);
  gst_caps_unref(caps);
  for (auto* sink : {video_sink, audio_sink})
    g_object_set(
        sink,
        "sync",
        TRUE,
        "async",
        FALSE,
        "qos",
        FALSE,
        "max-lateness",
        gint64{-1},
        "processing-deadline",
        guint64{0},
        "signal-handoffs",
        TRUE,
        nullptr);
  auto* process_pad = gst_element_get_static_pad(processing, "sink");
  gst_pad_add_probe(
      process_pad,
      GST_PAD_PROBE_TYPE_BUFFER,
      +[](GstPad*, GstPadProbeInfo*, gpointer data) {
        auto& c = *static_cast<Capture*>(data);
        // Sustained slightly slow processing, then one separate stall. Both
        // must delay PCM with the exact video interval instead of losing it.
        if (c.stall) {
          std::this_thread::sleep_for(std::chrono::milliseconds(17));
          if (c.video_count == 30)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        ++c.video_count;
        return GST_PAD_PROBE_OK;
      },
      &capture,
      nullptr);
  gst_object_unref(process_pad);
  g_signal_connect(
      video_sink,
      "handoff",
      G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
        auto& c = *static_cast<Capture*>(data);
        const auto* frame = hm::preview::frame_identity(buffer);
        std::lock_guard<std::mutex> lock(c.mutex);
        if (frame)
          c.video.push_back({frame->source_pts, frame->presentation_pts});
      }),
      &capture);
  g_signal_connect(
      audio_sink,
      "handoff",
      G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
        auto& c = *static_cast<Capture*>(data);
        GstMapInfo bytes{};
        if (!gst_buffer_map(buffer, &bytes, GST_MAP_READ))
          return;
        std::lock_guard<std::mutex> lock(c.mutex);
        const auto* samples = reinterpret_cast<const float*>(bytes.data);
        if (!GST_CLOCK_TIME_IS_VALID(c.first_audio))
          c.first_audio = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(c.audio_end))
          c.contiguous &= c.audio_end == GST_BUFFER_PTS(buffer);
        for (size_t i = 0; i < bytes.size / sizeof(float); i += 2) {
          c.audio.push_back(samples[i]);
          c.audio_times.push_back(GST_BUFFER_PTS(buffer) + gst_util_uint64_scale(i / 2, GST_SECOND, kRate));
          c.contiguous &= samples[i] == samples[i + 1];
        }
        c.audio_end = GST_BUFFER_PTS(buffer) + GST_BUFFER_DURATION(buffer);
        gst_buffer_unmap(buffer, &bytes);
      }),
      &capture);
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  std::vector<GstBuffer*> originals;
  const auto push = [origin](GstElement* source, GstBuffer* buffer, bool first) {
    if (!first || !origin)
      return gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.start = segment.position = segment.time = origin;
    auto* sample = gst_sample_new(buffer, nullptr, &segment, nullptr);
    const auto result = gst_app_src_push_sample(GST_APP_SRC(source), sample);
    gst_sample_unref(sample);
    gst_buffer_unref(buffer);
    return result;
  };
  std::thread audio_sender([&] {
    for (unsigned start = 0; start < kSamples;) {
      const auto count = std::min(333U, kSamples - start); // crosses every video/sample boundary
      auto* buffer = gst_buffer_new_allocate(nullptr, count * 2 * sizeof(float), nullptr);
      GstMapInfo bytes{};
      gst_buffer_map(buffer, &bytes, GST_MAP_WRITE);
      auto* samples = reinterpret_cast<float*>(bytes.data);
      for (unsigned i = 0; i < count; ++i)
        samples[2 * i] = samples[2 * i + 1] = float(start + i + 1);
      gst_buffer_unmap(buffer, &bytes);
      GST_BUFFER_PTS(buffer) = origin + gst_util_uint64_scale(start, GST_SECOND, kRate);
      GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(count, GST_SECOND, kRate);
      originals.push_back(gst_buffer_ref(buffer));
      if (push(audio, buffer, start == 0) != GST_FLOW_OK)
        break;
      start += count;
    }
    gst_app_src_end_of_stream(GST_APP_SRC(audio));
  });
  std::thread video_sender([&] {
    for (unsigned i = 0; i < kVideoFrames; ++i) {
      auto* buffer = gst_buffer_new_allocate(nullptr, 12, nullptr);
      GST_BUFFER_PTS(buffer) = origin + gst_util_uint64_scale(i, GST_SECOND, 60);
      GST_BUFFER_DURATION(buffer) = origin + gst_util_uint64_scale(i + 1, GST_SECOND, 60) - GST_BUFFER_PTS(buffer);
      if (push(video, buffer, i == 0) != GST_FLOW_OK)
        break;
    }
    gst_app_src_end_of_stream(GST_APP_SRC(video));
  });
  if (pause) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      bool started;
      {
        std::lock_guard<std::mutex> lock(capture.mutex);
        started = capture.video.size() >= 15;
      }
      if (started || std::chrono::steady_clock::now() >= deadline)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    GstStateChangeReturn paused;
    do {
      paused = hm::gst::RequestPacedPipelinePause(pipeline);
      if (paused == GST_STATE_CHANGE_ASYNC)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (paused == GST_STATE_CHANGE_ASYNC && std::chrono::steady_clock::now() < deadline);
    ok &= require(
        paused != GST_STATE_CHANGE_FAILURE &&
            gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_FAILURE,
        "Transport pause did not quiesce");
    const auto base = gst_element_get_base_time(pipeline);
    size_t audio_count;
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      audio_count = capture.audio.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      ok &= require(capture.audio.size() == audio_count, "A paused transport emitted PCM");
    }
    ok &= require(
        hm::gst::RequestPacedPipelineResume(pipeline) != GST_STATE_CHANGE_FAILURE,
        "Transport resume required a nonexistent physical audio clock");
    ok &= require(
        gst_element_get_base_time(pipeline) >= base + 180 * GST_MSECOND,
        "System-clock transport must retain ordinary pipeline pause base adjustment");
  }
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  ok &= require(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "Transport fixture did not reach EOS");
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError* error{};
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
    g_error_free(error);
    g_free(debug);
  }
  if (message)
    gst_message_unref(message);
  owner->Cancel();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  audio_sender.join();
  video_sender.join();
  ok &= require(capture.contiguous && capture.video.size() == kVideoFrames, "PCM timeline or video identity was lost");
  unsigned expected = 1, silence = 0;
  GstClockTime maximum_error = 0;
  for (size_t i = 0; i < capture.audio.size(); ++i) {
    const auto value = capture.audio[i];
    if (value == 0) {
      ++silence;
      continue;
    }
    ok &= value == float(expected);
    const auto frame = std::min<unsigned>((expected - 1) / 800, kVideoFrames - 1);
    if (frame < capture.video.size()) {
      const auto desired =
          capture.video[frame].presentation + gst_util_uint64_scale((expected - 1) % 800, GST_SECOND, kRate);
      const auto actual = capture.audio_times[i];
      maximum_error = std::max(maximum_error, actual > desired ? actual - desired : desired - actual);
    }
    ++expected;
  }
  ok &= require(expected == kSamples + 1, "Every unique source sample must survive exactly once and in order");
  ok &= require(
      maximum_error <= GST_SECOND / kRate + 2, "PCM and exact video presentation differ by more than one sample");
  if (stall)
    ok &= require(silence >= kRate / 4, "Processing stall must insert real transport silence");
  for (unsigned i = 0; i < originals.size(); ++i) {
    ok &= GST_BUFFER_PTS(originals[i]) == origin + gst_util_uint64_scale(i * 333, GST_SECOND, kRate);
    gst_buffer_unref(originals[i]);
  }
  for (size_t i = 0; i < capture.video.size(); ++i)
    ok &= capture.video[i].source == origin + gst_util_uint64_scale(i, GST_SECOND, 60);
  std::cout << "transport stall=" << stall << " pause=" << pause << " origin=" << origin
            << " source-samples=" << expected - 1 << " silence=" << silence << " video=" << capture.video.size()
            << " max-error-ns=" << maximum_error << '\n';
  gst_object_unref(bus);
  gst_object_unref(route);
  gst_object_unref(pipeline);
  return ok;
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc == 2 && std::string(argv[1]) == "--allocation-query-only")
    return run_allocation_query() ? 0 : 1;
  return run_allocation_query() && run(false) && run(true) && run(true, true) && run(false, false, 5 * GST_SECOND) ? 0
                                                                                                                   : 1;
}
