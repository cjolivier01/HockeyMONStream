#include "hstream/src/libs/common/PacedPlayback.h"

#include <gst/app/gstappsrc.h>
#include <gst/audio/gstaudioclock.h>
#include <gst/audio/gstaudiosink.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrames = 480;
constexpr int kBuffers = 400;
constexpr auto kPeriod = std::chrono::milliseconds(10);
bool Failure(int line) {
  std::cerr << "Paced playback regression failed at line " << line << '\n';
  return false;
}

struct Device {
  std::mutex writes, mutex;
  std::condition_variable changed;
  int periods = 20;
  bool stopped = false;
  bool hold_completion = false, completion_waiting = false, release_completion = false;
  bool paused = false;
  Clock::time_point paused_at{}, until{};
  std::vector<float> pcm;
  std::vector<Clock::time_point> audible;
  std::vector<std::pair<Clock::time_point, Clock::time_point>> pauses;
};

struct CompletionRelease {
  Device* device;
  ~CompletionRelease() {
    if (device) {
      std::lock_guard<std::mutex> lock(device->mutex);
      device->release_completion = true;
      device->changed.notify_all();
    }
  }
};

// A real GstAudioSink ringbuffer feeding a CPU device. Like ALSA, the device
// accepts its negotiated physical buffer immediately, then blocks each write
// until capacity is available. Counting handoffs before the ringbuffer would
// miss the failure: its commit path reports success when it discards late PCM.
struct BufferedSink {
  GstAudioSink parent;
  Device* device;
};
struct BufferedSinkClass {
  GstAudioSinkClass parent;
};
G_DEFINE_TYPE(BufferedSink, buffered_sink, GST_TYPE_AUDIO_SINK)

gboolean Open(GstAudioSink*) {
  return TRUE;
}

gboolean Prepare(GstAudioSink* sink, GstAudioRingBufferSpec* spec) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  spec->segsize = kFrames * GST_AUDIO_INFO_BPF(&spec->info);
  spec->segtotal = spec->seglatency = device.periods;
  std::lock_guard<std::mutex> lock(device.mutex);
  device.stopped = false;
  device.hold_completion = device.completion_waiting = device.release_completion = false;
  device.paused = false;
  device.until = Clock::now();
  device.pcm.clear();
  device.audible.clear();
  device.pauses.clear();
  return TRUE;
}

void Stop(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  device.stopped = true;
  device.until = Clock::now();
  device.changed.notify_all();
}

void Pause(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> write_lock(device.writes);
  std::lock_guard<std::mutex> lock(device.mutex);
  if (!device.paused) {
    device.paused = true;
    device.paused_at = Clock::now();
  }
}

void Resume(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  if (device.paused) {
    const auto resumed_at = Clock::now();
    const auto pause = resumed_at - device.paused_at;
    device.pauses.emplace_back(device.paused_at, resumed_at);
    if (device.until > device.paused_at)
      device.until += pause;
    for (auto& audible : device.audible)
      if (audible >= device.paused_at)
        audible += pause;
    device.paused = false;
  }
  device.stopped = false;
}

gboolean Unprepare(GstAudioSink* sink) {
  Stop(sink);
  return TRUE;
}

gint Write(GstAudioSink* sink, gpointer data, guint length) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::unique_lock<std::mutex> write_lock(device.writes);
  std::unique_lock<std::mutex> lock(device.mutex);
  const auto frames = length / (kChannels * sizeof(float));
  const auto duration = std::chrono::nanoseconds(gst_util_uint64_scale(frames, GST_SECOND, kRate));
  const auto ready = device.until + duration - device.periods * kPeriod;
  // ALSA serializes its pending period write and hardware pause with the
  // same device lock. Complete the current bounded write before pausing;
  // returning success without admitting its samples would hide real loss.
  lock.unlock();
  std::this_thread::sleep_until(ready);
  lock.lock();
  if (device.stopped)
    return length;
  if (device.paused)
    return 0;
  const auto start = std::max(device.until, Clock::now());
  device.until = start + duration;
  const auto* pcm = static_cast<float*>(data);
  for (size_t frame = 0; frame < frames; ++frame) {
    device.pcm.push_back(pcm[frame * kChannels]);
    device.audible.push_back(start + std::chrono::nanoseconds(gst_util_uint64_scale(frame, GST_SECOND, kRate)));
  }
  if (device.hold_completion) {
    // Device admission and hardware pause share their own lock. GstAudioSink
    // publishes the completed period only after this callback returns, so a
    // valid pause can fall between those two operations.
    device.completion_waiting = true;
    write_lock.unlock();
    device.changed.notify_all();
    device.changed.wait(lock, [&] { return device.release_completion || device.stopped; });
    device.hold_completion = false;
  }
  return length;
}

guint Delay(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               device.until - (device.paused ? device.paused_at : Clock::now()))
                               .count();
  return gst_util_uint64_scale(std::max<int64_t>(0, nanoseconds), kRate, GST_SECOND);
}

void Finalize(GObject* object) {
  delete reinterpret_cast<BufferedSink*>(object)->device;
  G_OBJECT_CLASS(buffered_sink_parent_class)->finalize(object);
}

void buffered_sink_class_init(BufferedSinkClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = Finalize;
  auto* audio = GST_AUDIO_SINK_CLASS(klass);
  audio->open = Open;
  audio->close = Open;
  audio->prepare = Prepare;
  audio->unprepare = Unprepare;
  audio->write = Write;
  audio->delay = Delay;
  audio->pause = Pause;
  audio->stop = Stop;
  audio->resume = Resume;
  auto* element = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(element, "Buffered CPU audio device", "Sink/Audio", "PCM regression", "Test");
  GstCaps* caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
  gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
}

void buffered_sink_init(BufferedSink* sink) {
  sink->device = new Device;
}

struct Observations {
  std::mutex mutex;
  std::vector<GstClockTime> archived_pts;
  std::vector<float> archived_pcm;
  GstElement* physical_sink{};
  struct VideoTarget {
    GstClockTime physical_time, rate_num, rate_denom;
  };
  std::vector<VideoTarget> video_targets;
  std::vector<GstClockTime> video_pts;
  std::vector<Clock::time_point> video_time;
};

void Archived(GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
  auto& observations = *static_cast<Observations*>(data);
  std::lock_guard<std::mutex> lock(observations.mutex);
  observations.archived_pts.push_back(GST_BUFFER_PTS(buffer));
  GstMapInfo map{};
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const auto* pcm = reinterpret_cast<const float*>(map.data);
    observations.archived_pcm.insert(observations.archived_pcm.end(), pcm, pcm + map.size / sizeof(float));
    gst_buffer_unmap(buffer, &map);
  }
}

void Presented(GstElement* video, GstBuffer* buffer, GstPad*, gpointer data) {
  auto& observations = *static_cast<Observations*>(data);
  std::lock_guard<std::mutex> lock(observations.mutex);
  observations.video_pts.push_back(GST_BUFFER_PTS(buffer));
  observations.video_time.push_back(Clock::now());
  // Capture the video's scheduled sample position without reading or
  // modifying the running clock. Compare it later to actual device writes.
  auto* physical = GST_AUDIO_BASE_SINK(observations.physical_sink);
  const auto target = GST_BUFFER_PTS(buffer) + gst_element_get_base_time(video) +
      gst_base_sink_get_ts_offset(GST_BASE_SINK(video)) + gst_base_sink_get_latency(GST_BASE_SINK(video));
  GstClockTime internal, external, num, denom;
  gst_clock_get_calibration(physical->provided_clock, &internal, &external, &num, &denom);
  const auto physical_target = target >= external ? internal + gst_util_uint64_scale(target - external, denom, num)
                                                  : internal - gst_util_uint64_scale(external - target, denom, num);
  observations.video_targets.push_back(
      {physical_target - GST_AUDIO_CLOCK(physical->provided_clock)->time_offset, num, denom});
}

bool DrainLatency(GstElement* pipeline) {
  GstBus* bus = gst_element_get_bus(pipeline);
  bool ok = true;
  while (GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_LATENCY)) {
    ok = gst_bin_recalculate_latency(GST_BIN(pipeline)) && ok;
    gst_message_unref(message);
  }
  gst_object_unref(bus);
  return ok;
}

bool Push(GstElement* audio, GstElement* video, int index) {
  auto* buffer = gst_buffer_new_allocate(nullptr, kFrames * kChannels * sizeof(float), nullptr);
  GstMapInfo map{};
  gst_buffer_map(buffer, &map, GST_MAP_WRITE);
  auto* pcm = reinterpret_cast<float*>(map.data);
  std::fill(pcm, pcm + kFrames * kChannels, float(index + 1));
  gst_buffer_unmap(buffer, &map);
  GST_BUFFER_PTS(buffer) = index * 10 * GST_MSECOND;
  GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
  if (gst_app_src_push_buffer(GST_APP_SRC(audio), buffer) != GST_FLOW_OK)
    return Failure(__LINE__);
  buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
  GST_BUFFER_PTS(buffer) = index * 10 * GST_MSECOND;
  GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
  return gst_app_src_push_buffer(GST_APP_SRC(video), buffer) == GST_FLOW_OK;
}

bool Run(
    GstElement* pipeline,
    GstElement* audio,
    GstElement* video,
    GstElement* sink,
    Observations& observations,
    int periods,
    const std::shared_ptr<hm::gst::PacedPlayback>& owner,
    bool pause_midway = false) {
  observations.archived_pts.clear();
  observations.archived_pcm.clear();
  observations.video_targets.clear();
  observations.video_pts.clear();
  observations.video_time.clear();
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  device.periods = periods;
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE || !Push(audio, video, 0) ||
      gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
      !DrainLatency(pipeline))
    return Failure(__LINE__);
  // The video sink owns asynchronous preroll; the independent audio queue may
  // still be negotiating after that completes.
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!GST_AUDIO_BASE_SINK(sink)->ringbuffer ||
         !gst_audio_ring_buffer_is_acquired(GST_AUDIO_BASE_SINK(sink)->ringbuffer)) {
    if (Clock::now() >= deadline)
      return Failure(__LINE__);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!gst_bin_recalculate_latency(GST_BIN(pipeline)))
    return Failure(__LINE__);
  const auto& spec = GST_AUDIO_BASE_SINK(sink)->ringbuffer->spec;
  const auto physical = gst_util_uint64_scale(
      guint64(spec.seglatency) * spec.segsize, GST_SECOND, guint64(kRate) * kChannels * sizeof(float));
  auto* output = gst_bin_get_by_name(GST_BIN(pipeline), "video-output");
  const auto common = gst_base_sink_get_latency(GST_BASE_SINK(output));
  gst_object_unref(output);
  // The device, not a guessed offset, determines this after every negotiation.
  const bool shared_latency = physical && common >= physical &&
      gst_base_sink_get_latency(GST_BASE_SINK(sink)) == common &&
      gst_pipeline_get_latency(GST_PIPELINE(pipeline)) == GST_CLOCK_TIME_NONE;
  if (!shared_latency) {
    std::cerr << "Audio and video did not share negotiated device latency\n";
  }
  gboolean live = TRUE;
  g_object_get(audio, "is-live", &live, nullptr);
  if (live || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return Failure(__LINE__);
  auto start = Clock::now();
  for (int index = 1; index < kBuffers; ++index) {
    bool submitted = false;
    if (pause_midway && index == kBuffers / 2) {
      const auto before_pause = Clock::now();
      if (!owner->PreparePause() || gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
          gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_SUCCESS)
        return Failure(__LINE__);
      if (!Push(audio, video, index))
        return Failure(__LINE__);
      submitted = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
          gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
          !owner->ResumeAudioClock())
        return Failure(__LINE__);
      start += Clock::now() - before_pause;
    }
    // Exact-pair/processing work supplies audio near its original presentation
    // time, with a delayed interval inside the negotiated processing deadline.
    // No sample timestamp is adjusted.
    const auto delay = index >= 40 && index < 46 ? std::chrono::milliseconds(15) : kPeriod;
    std::this_thread::sleep_until(start + index * std::chrono::microseconds(11500) + delay);
    if (!DrainLatency(pipeline) || (!submitted && !Push(audio, video, index)))
      return Failure(__LINE__);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(audio));
  gst_app_src_end_of_stream(GST_APP_SRC(video));
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message =
      gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const bool ended = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message)
    gst_message_unref(message);
  gst_object_unref(bus);
  if (!ended || gst_element_set_state(pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
    return Failure(__LINE__);

  // Archive remains bit-exact, with every original packet and timestamp.
  if (observations.archived_pts.size() != kBuffers || observations.video_pts != observations.archived_pts ||
      observations.archived_pcm.size() != kBuffers * kFrames * kChannels)
    return Failure(__LINE__);
  for (int index = 0; index < kBuffers; ++index) {
    if (observations.archived_pts[index] != guint64(index) * 10 * GST_MSECOND)
      return Failure(__LINE__);
    for (int sample = 0; sample < kFrames * kChannels; ++sample)
      if (observations.archived_pcm[index * kFrames * kChannels + sample] != float(index + 1))
        return Failure(__LINE__);
  }
  std::vector<int> delivered(kBuffers);
  std::vector<Clock::time_point> audible(kBuffers);
  {
    std::lock_guard<std::mutex> lock(device.mutex);
    for (size_t sample = 0; sample < device.pcm.size(); ++sample) {
      const float value = device.pcm[sample];
      if (value >= 1 && value <= kBuffers && value == std::floor(value)) {
        const int index = int(value) - 1;
        if (!delivered[index])
          audible[index] = device.audible[sample];
        ++delivered[index];
      }
    }
  }
  int minimum = kFrames;
  double maximum_skew = 0, minimum_signed_skew = 0, maximum_signed_skew = 0;
  int worst_packet = 0;
  for (int index = 0; index < kBuffers; ++index) {
    minimum = std::min(minimum, delivered[index]);
    const auto skew =
        std::chrono::duration<double, std::milli>(audible[index] - observations.video_time[index]).count();
    if (std::abs(skew) > maximum_skew)
      worst_packet = index;
    maximum_skew = std::max(maximum_skew, std::abs(skew));
    minimum_signed_skew = std::min(minimum_signed_skew, skew);
    maximum_signed_skew = std::max(maximum_signed_skew, skew);
    if (delivered[index] < kFrames * 9 / 10)
      std::cerr << "under-delivered packet " << index << ": " << delivered[index] << " skew=" << skew << '\n';
  }
  std::cout << "device periods=" << periods << " shared latency=" << common / GST_MSECOND
            << " ms; pause=" << pause_midway << "; minimum delivered=" << minimum << "/" << kFrames
            << "; max A/V skew=" << maximum_skew << " ms; signed range=[" << minimum_signed_skew << ","
            << maximum_signed_skew << "]; worst packet=" << worst_packet << "\n";
  // GstAudioSink publishes physical-clock progress in device-period units.
  // Video wakeup can lag an audible sample by one period plus scheduler jitter;
  // retain a half-period scheduling margin. Sample conservation remains exact.
  const auto skew_bound_ms = std::chrono::duration<double, std::milli>(kPeriod).count() * 1.5;
  return shared_latency && minimum == kFrames && maximum_skew < skew_bound_ms;
}

// A missing peer must never hold a shutdown or READY reconstruction hostage.
// Both flush branches may cancel the same epoch, and late packets from that
// epoch are ordinary flushing rather than new stream failures.
bool RunCancellation(
    GstElement* pipeline,
    GstElement* audio,
    GstElement* video,
    const std::shared_ptr<hm::gst::PacedPlayback>& owner,
    bool audio_first) {
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    return Failure(__LINE__);
  auto* buffer = gst_buffer_new_allocate(nullptr, audio_first ? kFrames * kChannels * sizeof(float) : 1, nullptr);
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
  if (gst_app_src_push_buffer(GST_APP_SRC(audio_first ? audio : video), buffer) != GST_FLOW_OK)
    return Failure(__LINE__);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto start = Clock::now();
  owner->Cancel();
  owner->Cancel();
  if (!Push(audio, video, 1))
    return Failure(__LINE__);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (gst_element_set_state(pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE ||
      Clock::now() - start > std::chrono::seconds(1))
    return Failure(__LINE__);
  auto* bus = gst_element_get_bus(pipeline);
  auto* error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (error) {
    GError* detail{};
    gchar* debug{};
    gst_message_parse_error(error, &detail, &debug);
    std::cerr << "Cancellation posted a stream error: " << detail->message << " / " << (debug ? debug : "") << '\n';
    g_clear_error(&detail);
    g_free(debug);
    gst_message_unref(error);
    return Failure(__LINE__);
  }
  std::cout << "Cancellation released unmatched " << (audio_first ? "audio" : "video") << " without stream errors\n";
  return true;
}

// AAC packets span more than one 59.94 fps video interval. A large shared
// processing stall can therefore fall inside one original PCM packet.
bool RunPacketBoundaries(
    GstElement* pipeline,
    GstElement* audio,
    GstElement* video,
    GstElement* sink,
    Observations& observations,
    const std::shared_ptr<hm::gst::PacedPlayback>& owner,
    bool pause_midway = false,
    bool rounded_duration = false,
    bool late_device_completion = false,
    guint fps_n = 60000,
    guint fps_d = 1001,
    guint video_frames = 120,
    guint shorter_tail_samples = 0,
    bool stall_inside_audio_interval = false,
    bool same_stage_restart = false) {
  constexpr guint kAudioPacketSamples = 1024;
  const auto frame_time = [=](guint index) { return gst_util_uint64_scale(index, fps_d * GST_SECOND, fps_n); };
  const auto total_samples = gst_util_uint64_scale(frame_time(video_frames), kRate, GST_SECOND) - shorter_tail_samples;
  observations.archived_pts.clear();
  observations.archived_pcm.clear();
  observations.video_targets.clear();
  observations.video_pts.clear();
  observations.video_time.clear();
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  device.periods = 4;
  guint64 audio_samples = 0;
  bool audio_stalled = false;
  std::vector<GstClockTime> audio_pts;
  const auto push = [&](guint index) {
    auto* picture = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(picture) = frame_time(index);
    // Native fixed-rate caps supply a rounded duration while PTS retains the
    // precise cadence, leaving occasional one-nanosecond gaps between frames.
    GST_BUFFER_DURATION(picture) = rounded_duration ? frame_time(1) : frame_time(index + 1) - frame_time(index);
    if (gst_app_src_push_buffer(GST_APP_SRC(video), picture) != GST_FLOW_OK)
      return Failure(__LINE__);
    const auto admitted = gst_util_uint64_scale(frame_time(index + 1), kRate, GST_SECOND);
    while (audio_samples < total_samples) {
      const auto count = std::min<guint64>(kAudioPacketSamples, total_samples - audio_samples);
      if (audio_samples + count > admitted)
        break;
      auto* packet = gst_buffer_new_allocate(nullptr, count * kChannels * sizeof(float), nullptr);
      GstMapInfo map{};
      gst_buffer_map(packet, &map, GST_MAP_WRITE);
      auto* pcm = reinterpret_cast<float*>(map.data);
      for (guint64 sample = 0; sample < count; ++sample)
        for (guint channel = 0; channel < kChannels; ++channel)
          pcm[sample * kChannels + channel] = float(audio_samples + sample + 1);
      gst_buffer_unmap(packet, &map);
      GST_BUFFER_PTS(packet) = gst_util_uint64_scale(audio_samples, GST_SECOND, kRate);
      GST_BUFFER_DURATION(packet) =
          gst_util_uint64_scale(audio_samples + count, GST_SECOND, kRate) - GST_BUFFER_PTS(packet);
      audio_pts.push_back(GST_BUFFER_PTS(packet));
      if (gst_app_src_push_buffer(GST_APP_SRC(audio), packet) != GST_FLOW_OK)
        return Failure(__LINE__);
      audio_samples += count;
      if (stall_inside_audio_interval && index == video_frames / 2 && !audio_stalled) {
        // Delay the remaining PCM inside an already submitted long video
        // interval. Opening its mapping on the first packet loses late PCM.
        audio_stalled = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    return true;
  };
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE || !push(0) || !push(1) ||
      gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS)
    return Failure(__LINE__);
  const auto acquired_deadline = Clock::now() + std::chrono::seconds(2);
  while (!GST_AUDIO_BASE_SINK(sink)->ringbuffer ||
         !gst_audio_ring_buffer_is_acquired(GST_AUDIO_BASE_SINK(sink)->ringbuffer)) {
    if (Clock::now() >= acquired_deadline)
      return Failure(__LINE__);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!gst_bin_recalculate_latency(GST_BIN(pipeline)) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return Failure(__LINE__);
  auto start = Clock::now();
  for (guint index = 2; index < video_frames; ++index) {
    bool submitted = false;
    if (pause_midway && index == video_frames * 2 / 3) {
      const auto before_pause = Clock::now();
      const auto pause_base = gst_element_get_base_time(pipeline);
      const auto prior_start = gst_element_get_start_time(pipeline);
      if (same_stage_restart) {
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        for (;;) {
          const auto result = hm::gst::PreparePacedPipelinePause(pipeline);
          if (result == GST_STATE_CHANGE_SUCCESS)
            break;
          if (result == GST_STATE_CHANGE_FAILURE || Clock::now() >= deadline)
            return Failure(__LINE__);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      if (!same_stage_restart && !owner->PreparePause())
        return Failure(__LINE__);
      CompletionRelease release{late_device_completion ? &device : nullptr};
      guint64 before_completion = 0;
      if (late_device_completion) {
        std::unique_lock<std::mutex> lock(device.mutex);
        device.hold_completion = true;
        if (!device.changed.wait_for(lock, std::chrono::seconds(1), [&] { return device.completion_waiting; }))
          return Failure(__LINE__);
        before_completion = gst_audio_ring_buffer_samples_done(GST_AUDIO_BASE_SINK(sink)->ringbuffer);
      }
      if (same_stage_restart) {
        // Exercise the production asynchronous same-stage peer entrypoint,
        // including the completed device period still awaiting publication.
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        for (;;) {
          if (hm::gst::RequestPacedPipelinePause(pipeline) == GST_STATE_CHANGE_FAILURE)
            return Failure(__LINE__);
          GstState current, pending;
          const auto result = gst_element_get_state(pipeline, &current, &pending, 0);
          if (result == GST_STATE_CHANGE_SUCCESS && current == GST_STATE_PAUSED)
            break;
          if (Clock::now() >= deadline)
            return Failure(__LINE__);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else if (
          gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
          gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_SUCCESS) {
        return Failure(__LINE__);
      }
      if (late_device_completion) {
        {
          std::lock_guard<std::mutex> lock(device.mutex);
          device.release_completion = true;
          device.changed.notify_all();
        }
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (gst_audio_ring_buffer_samples_done(GST_AUDIO_BASE_SINK(sink)->ringbuffer) == before_completion) {
          if (Clock::now() >= deadline)
            return Failure(__LINE__);
          std::this_thread::yield();
        }
      }
      if (!push(index))
        return Failure(__LINE__);
      submitted = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      const auto resume = same_stage_restart ? hm::gst::RequestPacedPipelineResume(pipeline)
                                             : gst_element_set_state(pipeline, GST_STATE_PLAYING);
      if (resume == GST_STATE_CHANGE_FAILURE ||
          gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
          (!same_stage_restart && !owner->ResumeAudioClock()))
        return Failure(__LINE__);
      if (late_device_completion &&
          (gst_element_get_base_time(pipeline) != pause_base || gst_element_get_start_time(pipeline) != prior_start)) {
        std::cerr << "Pause rebased preserved PCM: base=" << pause_base << " -> " << gst_element_get_base_time(pipeline)
                  << "; start=" << prior_start << " -> " << gst_element_get_start_time(pipeline) << '\n';
        return Failure(__LINE__);
      }
      start += Clock::now() - before_pause;
    }
    const auto wall = std::chrono::nanoseconds(frame_time(index) * 115 / 100);
    const auto stall = index >= video_frames / 2 ? std::chrono::milliseconds(100) : std::chrono::milliseconds(0);
    std::this_thread::sleep_until(start + wall + stall);
    if (!DrainLatency(pipeline) || (!submitted && !push(index)))
      return Failure(__LINE__);
  }
  if (audio_samples != total_samples)
    return Failure(__LINE__);
  gst_app_src_end_of_stream(GST_APP_SRC(audio));
  gst_app_src_end_of_stream(GST_APP_SRC(video));
  auto* bus = gst_element_get_bus(pipeline);
  auto* message =
      gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  const bool ended = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message)
    gst_message_unref(message);
  gst_object_unref(bus);
  if (!ended)
    return Failure(__LINE__);
  if (late_device_completion) {
    const auto start_time = gst_element_get_start_time(pipeline);
    if (!owner->PreparePause() || gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
        gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND) != GST_STATE_CHANGE_SUCCESS)
      return Failure(__LINE__);
    owner->Cancel();
    if (gst_element_get_start_time(pipeline) != start_time)
      return Failure(__LINE__);
  }
  if (gst_element_set_state(pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE ||
      observations.archived_pts != audio_pts || observations.video_pts.size() != video_frames ||
      observations.archived_pcm.size() != total_samples * kChannels)
    return Failure(__LINE__);
  for (guint64 sample = 0; sample < total_samples; ++sample)
    for (guint channel = 0; channel < kChannels; ++channel)
      if (observations.archived_pcm[sample * kChannels + channel] != float(sample + 1))
        return Failure(__LINE__);
  int64_t maximum_sample_difference = 0, minimum_sample_difference = 0;
  guint64 delivered = 0, worst_sample = 0;
  size_t worst_frame = 0;
  double maximum_skew = 0, minimum_signed_skew = 0, maximum_signed_skew = 0;
  std::lock_guard<std::mutex> lock(device.mutex);
  for (size_t sample = 0; sample < device.pcm.size(); ++sample) {
    const auto marker = device.pcm[sample];
    if (marker == 0)
      continue; // Honest shared stalls produce silence between original samples.
    if (marker != float(delivered + 1)) {
      std::cerr << "PCM sample order/loss at " << delivered << ": " << marker << '\n';
      return Failure(__LINE__);
    }
    const auto time = gst_util_uint64_scale(delivered, GST_SECOND, kRate);
    const auto next = std::upper_bound(observations.video_pts.begin(), observations.video_pts.end(), time);
    const auto index = std::distance(observations.video_pts.begin(), next) - 1;
    if (index < 0)
      return Failure(__LINE__);
    // Compare the common active presentation clock. A pause can land between
    // a device period and the matching video handoff; wall time would count
    // the deliberate frozen interval as A/V skew on one side of that boundary.
    const auto active = [&](Clock::time_point time) {
      auto elapsed = time.time_since_epoch();
      for (const auto& [paused_at, resumed_at] : device.pauses)
        if (time > paused_at)
          elapsed -= std::min(time, resumed_at) - paused_at;
      return elapsed;
    };
    const auto expected =
        active(observations.video_time[index]) + std::chrono::nanoseconds(time - observations.video_pts[index]);
    const auto skew = std::chrono::duration<double, std::milli>(active(device.audible[sample]) - expected).count();
    const auto& target = observations.video_targets[index];
    // Convert the complete target once. Adding separately floored frame and
    // within-frame sample positions loses a second sample at 14.985 fps.
    const auto target_sample = gst_util_uint64_scale(
        target.physical_time +
            gst_util_uint64_scale(time - observations.video_pts[index], target.rate_denom, target.rate_num),
        kRate,
        GST_SECOND);
    const auto difference = int64_t(sample) - int64_t(target_sample);
    maximum_sample_difference = std::max(maximum_sample_difference, difference);
    minimum_sample_difference = std::min(minimum_sample_difference, difference);
    if (std::abs(skew) > maximum_skew) {
      worst_sample = delivered;
      worst_frame = index;
    }
    maximum_skew = std::max(maximum_skew, std::abs(skew));
    minimum_signed_skew = std::min(minimum_signed_skew, skew);
    maximum_signed_skew = std::max(maximum_signed_skew, skew);
    ++delivered;
  }
  std::cout << "AAC/video boundaries: fps=" << fps_n << '/' << fps_d << "; frames=" << video_frames
            << "; shorter-tail-samples=" << shorter_tail_samples << "; pause=" << pause_midway
            << "; inside-audio-stall=" << stall_inside_audio_interval << "; same-stage-restart=" << same_stage_restart
            << "; rounded-duration=" << rounded_duration << "; late-device-completion=" << late_device_completion
            << "; delivered=" << delivered << "/" << total_samples << "; max A/V skew=" << maximum_skew
            << " ms; signed range=[" << minimum_signed_skew << "," << maximum_signed_skew
            << "]; worst sample=" << worst_sample << "; frame=" << worst_frame << "\n";
  std::cout << "Device sample vs video target: pause=" << pause_midway << " late-completion=" << late_device_completion
            << " range=[" << minimum_sample_difference << "," << maximum_sample_difference << "]\n";
  // The physical-device transcript must place each original sample on the
  // same clock target as video. Integer timestamp conversions may differ by
  // one sample; a complete device period must never hide inside wall jitter.
  return delivered == total_samples && maximum_skew < 15 && minimum_sample_difference >= -1 &&
      maximum_sample_difference <= 1;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstElement* pipeline = gst_pipeline_new("paced-audio-latency-test");
  GstElement* audio = gst_element_factory_make("appsrc", "audio");
  GstElement* video = gst_element_factory_make("appsrc", "video");
  GstElement* video_queue = gst_element_factory_make("queue", "video-presentation-queue");
  g_object_set(video_queue, "max-size-buffers", 30U, "max-size-bytes", 0U, "max-size-time", guint64{0}, nullptr);
  GstElement* tee = gst_element_factory_make("tee", nullptr);
  GstElement* archive_queue = gst_element_factory_make("queue", nullptr);
  GstElement* monitor_queue = gst_element_factory_make("queue", nullptr);
  GstElement* archive = gst_element_factory_make("fakesink", nullptr);
  GstElement* output = gst_element_factory_make("fakesink", "video-output");
  auto* sink = GST_ELEMENT(g_object_new(
      buffered_sink_get_type(),
      "sync",
      TRUE,
      "async",
      FALSE,
      "provide-clock",
      FALSE,
      "max-lateness",
      gint64{-1},
      "qos",
      FALSE,
      nullptr));
  if (!pipeline || !audio || !video || !tee || !archive_queue || !monitor_queue || !archive || !output || !sink)
    return 2;
  GstCaps* caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
  gst_app_src_set_caps(GST_APP_SRC(audio), caps);
  gst_caps_unref(caps);
  caps = gst_caps_from_string("application/octet-stream");
  gst_app_src_set_caps(GST_APP_SRC(video), caps);
  gst_caps_unref(caps);
  g_object_set(audio, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(video, "format", GST_FORMAT_TIME, nullptr);
  g_object_set(archive, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, nullptr);
  g_object_set(output, "sync", TRUE, "async", TRUE, "signal-handoffs", TRUE, nullptr);
  Observations observations;
  observations.physical_sink = sink;
  g_signal_connect(archive, "handoff", G_CALLBACK(Archived), &observations);
  g_signal_connect(output, "handoff", G_CALLBACK(Presented), &observations);
  gst_bin_add_many(
      GST_BIN(pipeline), audio, video, tee, archive_queue, monitor_queue, archive, output, sink, video_queue, nullptr);
  const bool linked = gst_element_link(audio, tee) && gst_element_link_many(tee, archive_queue, archive, nullptr) &&
      gst_element_link_many(tee, monitor_queue, sink, nullptr) &&
      gst_element_link_many(video, video_queue, output, nullptr);
  // One installed probe must survive a completed run and READY reconstruction,
  // where the new device negotiates a different latency.
  GstPad* video_pad = gst_element_get_static_pad(video, "src");
  auto owner = hm::gst::PacedPlayback::Create(
      pipeline, video_pad, {16683, 4170, 0, 64, 3 * GST_SECOND, 1, 16384, 2, 3 * GST_SECOND});
  const bool duplicate_rejected = !hm::gst::PacedPlayback::Create(
      pipeline, video_pad, {16683, 4170, 0, 64, 3 * GST_SECOND, 1, 16384, 2, 3 * GST_SECOND});
  gst_object_unref(video_pad);
  bool ok = duplicate_rejected && linked && owner && owner->AttachVideoSink(output, 30) &&
      owner->AttachAudioSink(sink) && Run(pipeline, audio, video, sink, observations, 20, owner, true) &&
      Run(pipeline, audio, video, sink, observations, 12, owner) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner) &&
      RunCancellation(pipeline, audio, video, owner, true) && RunCancellation(pipeline, audio, video, owner, false) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, true) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, true, true) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, true, true, true) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, false, false, false, 10000, 1001, 20) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, false, false, false, 15000, 1001, 30) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, false, false, false, 24000, 1001, 24) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, false, false, false, 24000, 1001, 20) &&
      RunPacketBoundaries(pipeline, audio, video, sink, observations, owner, false, false, false, 24000, 1001, 24, 1) &&
      RunPacketBoundaries(
                pipeline, audio, video, sink, observations, owner, false, false, false, 10000, 1001, 20, 0, true) &&
      RunPacketBoundaries(
                pipeline, audio, video, sink, observations, owner, true, true, true, 60000, 1001, 120, 0, false, true);
  if (owner)
    owner->Cancel();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
