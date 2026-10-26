#include "hstream/src/libs/common/PacedAudioLatency.h"

#include <gst/app/gstappsrc.h>
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
constexpr int kBuffers = 120;
constexpr auto kPeriod = std::chrono::milliseconds(10);

struct Device {
  std::mutex mutex;
  std::condition_variable changed;
  int periods = 20;
  bool stopped = false;
  Clock::time_point until{};
  std::vector<float> pcm;
  std::vector<Clock::time_point> audible;
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
  device.until = Clock::now();
  device.pcm.clear();
  device.audible.clear();
  return TRUE;
}

void Stop(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  device.stopped = true;
  device.until = Clock::now();
  device.changed.notify_all();
}

void Resume(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  device.stopped = false;
}

gboolean Unprepare(GstAudioSink* sink) {
  Stop(sink);
  return TRUE;
}

gint Write(GstAudioSink* sink, gpointer data, guint length) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::unique_lock<std::mutex> lock(device.mutex);
  const auto frames = length / (kChannels * sizeof(float));
  const auto duration = std::chrono::nanoseconds(gst_util_uint64_scale(frames, GST_SECOND, kRate));
  const auto ready = device.until + duration - device.periods * kPeriod;
  device.changed.wait_until(lock, ready, [&] { return device.stopped; });
  if (device.stopped)
    return length;
  const auto start = std::max(device.until, Clock::now());
  device.until = start + duration;
  const auto* pcm = static_cast<float*>(data);
  for (size_t frame = 0; frame < frames; ++frame) {
    device.pcm.push_back(pcm[frame * kChannels]);
    device.audible.push_back(start + std::chrono::nanoseconds(gst_util_uint64_scale(frame, GST_SECOND, kRate)));
  }
  return length;
}

guint Delay(GstAudioSink* sink) {
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  std::lock_guard<std::mutex> lock(device.mutex);
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(device.until - Clock::now()).count();
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
  audio->pause = Stop;
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

void Presented(GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
  auto& observations = *static_cast<Observations*>(data);
  std::lock_guard<std::mutex> lock(observations.mutex);
  observations.video_pts.push_back(GST_BUFFER_PTS(buffer));
  observations.video_time.push_back(Clock::now());
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
    return false;
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
    int periods) {
  observations.archived_pts.clear();
  observations.archived_pcm.clear();
  observations.video_pts.clear();
  observations.video_time.clear();
  auto& device = *reinterpret_cast<BufferedSink*>(sink)->device;
  device.periods = periods;
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE || !Push(audio, video, 0) ||
      gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
      !DrainLatency(pipeline))
    return false;
  // The video sink owns asynchronous preroll; the independent audio queue may
  // still be negotiating after that completes.
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!GST_AUDIO_BASE_SINK(sink)->ringbuffer ||
         !gst_audio_ring_buffer_is_acquired(GST_AUDIO_BASE_SINK(sink)->ringbuffer)) {
    if (Clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!gst_bin_recalculate_latency(GST_BIN(pipeline)))
    return false;
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
    return false;
  const auto start = Clock::now();
  for (int index = 1; index < kBuffers; ++index) {
    // Exact-pair/processing work supplies audio near its original presentation
    // time, with a delayed interval inside the negotiated processing deadline.
    // No sample timestamp is adjusted.
    const auto delay = index >= 40 && index < 46 ? std::chrono::milliseconds(15) : kPeriod;
    std::this_thread::sleep_until(start + index * kPeriod + delay);
    if (!DrainLatency(pipeline) || !Push(audio, video, index))
      return false;
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
    return false;

  // Archive remains bit-exact, with every original packet and timestamp.
  if (observations.archived_pts.size() != kBuffers || observations.video_pts != observations.archived_pts ||
      observations.archived_pcm.size() != kBuffers * kFrames * kChannels)
    return false;
  for (int index = 0; index < kBuffers; ++index) {
    if (observations.archived_pts[index] != index * 10 * GST_MSECOND)
      return false;
    for (int sample = 0; sample < kFrames * kChannels; ++sample)
      if (observations.archived_pcm[index * kFrames * kChannels + sample] != float(index + 1))
        return false;
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
  double maximum_skew = 0;
  for (int index = 0; index < kBuffers; ++index) {
    minimum = std::min(minimum, delivered[index]);
    const auto skew =
        std::chrono::duration<double, std::milli>(audible[index] - observations.video_time[index]).count();
    maximum_skew = std::max(maximum_skew, std::abs(skew));
    if (delivered[index] < kFrames * 9 / 10)
      std::cerr << "under-delivered packet " << index << ": " << delivered[index] << " skew=" << skew << '\n';
  }
  std::cout << "device periods=" << periods << " shared latency=" << common / GST_MSECOND
            << " ms; minimum delivered=" << minimum << "/" << kFrames << "; max A/V skew=" << maximum_skew << " ms\n";
  // Permit normal audio clock slaving, while rejecting the prior sustained
  // silence (only the first packet reached the actual device write callback).
  return shared_latency && minimum >= kFrames * 9 / 10 && maximum_skew < 50;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstElement* pipeline = gst_pipeline_new("paced-audio-latency-test");
  GstElement* audio = gst_element_factory_make("appsrc", "audio");
  GstElement* video = gst_element_factory_make("appsrc", "video");
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
  g_signal_connect(archive, "handoff", G_CALLBACK(Archived), &observations);
  g_signal_connect(output, "handoff", G_CALLBACK(Presented), &observations);
  gst_bin_add_many(GST_BIN(pipeline), audio, video, tee, archive_queue, monitor_queue, archive, output, sink, nullptr);
  const bool linked = gst_element_link(audio, tee) && gst_element_link_many(tee, archive_queue, archive, nullptr) &&
      gst_element_link_many(tee, monitor_queue, sink, nullptr) && gst_element_link(video, output);
  // One installed probe must survive a completed run and READY reconstruction,
  // where the new device negotiates a different latency.
  bool ok = linked && hm::gst::InstallPacedAudioLatency(sink) && Run(pipeline, audio, video, sink, observations, 20) &&
      Run(pipeline, audio, video, sink, observations, 12);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
