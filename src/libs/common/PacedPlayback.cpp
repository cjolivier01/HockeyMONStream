#include "hstream/src/libs/common/PacedPlayback.h"

#include <gst/audio/audio.h>
#include <gst/audio/gstaudiobasesink.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "hstream/src/libs/common/PacedAudioLatency.h"

GST_DEBUG_CATEGORY_STATIC(paced_playback_debug);
#define GST_CAT_DEFAULT paced_playback_debug

namespace hm::gst {
namespace {
using Clock = std::chrono::steady_clock;
struct Interval {
  GstClockTime start, end;
  GstClockTimeDiff offset;
};
struct Mapping {
  GstClockTime end;
  GstClockTimeDiff offset;
  GstClockTime duration;
};
struct AudioQueue {
  GWeakRef element;
  guint rate{0}, bytes_per_frame{0};
  explicit AudioQueue(GstElement* queue) {
    g_weak_ref_init(&element, queue);
  }
  ~AudioQueue() {
    g_weak_ref_clear(&element);
  }
};
struct VideoProbe {
  std::shared_ptr<PacedPlayback> owner;
  GstSegment segment;
  GstClockTime duration{GST_CLOCK_TIME_NONE};
  guint retained_frames{0};
  guint64 epoch{0};
  explicit VideoProbe(std::shared_ptr<PacedPlayback> owner) : owner(std::move(owner)) {
    gst_segment_init(&segment, GST_FORMAT_TIME);
  }
};
GstClockTime RunningTime(const GstSegment& segment, GstClockTime pts) {
  return segment.format == GST_FORMAT_TIME && GST_CLOCK_TIME_IS_VALID(pts)
      ? gst_segment_to_running_time(&segment, GST_FORMAT_TIME, pts)
      : GST_CLOCK_TIME_NONE;
}
void Fail(GstElement* element, const char* detail) {
  GST_ELEMENT_ERROR(element, STREAM, FAILED, ("Could not preserve paced audio/video presentation"), ("%s", detail));
}
} // namespace

struct PacedPlayback::Impl {
  PacedPlaybackSettings settings;
  GWeakRef pipeline, audio_sink;
  std::vector<std::shared_ptr<AudioQueue>> audio_queues;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Interval> history;
  guint64 epoch{1};
  bool active{false}, ended{false}, cancelled{false};
  bool audio_attached{false}, audio_ended{false}, pause_pending{false};
  guint audio_pushes{0};
  GstClockTime pause_start_time{GST_CLOCK_TIME_NONE};
  bool preserve_pause_base{false};
  GstClockTime audio_end{GST_CLOCK_TIME_NONE};
  GstClockTime maximum_interval{0};
  GstClockTimeDiff offset{0};

  Impl(GstElement* pipeline, const PacedPlaybackSettings& settings) : settings(settings) {
    g_weak_ref_init(&this->pipeline, pipeline);
    g_weak_ref_init(&audio_sink, nullptr);
  }
  ~Impl() {
    g_weak_ref_clear(&pipeline);
    g_weak_ref_clear(&audio_sink);
  }
  void RestorePauseStartTimeLocked() {
    if (!preserve_pause_base)
      return;
    auto* element = GST_ELEMENT(g_weak_ref_get(&pipeline));
    if (element) {
      gst_element_set_start_time(element, pause_start_time);
      gst_object_unref(element);
    }
    preserve_pause_base = false;
  }
  bool PreservePauseBaseLocked() {
    if (cancelled || Stopping())
      return false;
    auto* element = GST_ELEMENT(g_weak_ref_get(&pipeline));
    if (!element)
      return false;
    auto* physical = g_weak_ref_get(&audio_sink);
    if (!physical) {
      // A transport follows the system clock. GstPipeline must adjust its
      // ordinary base time on resume; only a physical cursor needs retention.
      gst_object_unref(element);
      return true;
    }
    gst_object_unref(physical);
    if (!preserve_pause_base) {
      // A physical period can publish after GstPipeline's pause snapshot.
      // Retain the base so that period cannot move video behind retained PCM.
      pause_start_time = gst_element_get_start_time(element);
      gst_element_set_start_time(element, GST_CLOCK_TIME_NONE);
      preserve_pause_base = true;
    }
    gst_object_unref(element);
    return true;
  }
  void Cancel() {
    std::lock_guard<std::mutex> lock(mutex);
    RestorePauseStartTimeLocked();
    if (!cancelled)
      ++epoch;
    active = false;
    cancelled = true;
    pause_pending = false;
    ended = false;
    history.clear();
    audio_end = GST_CLOCK_TIME_NONE;
    audio_ended = false;
    offset = 0;
    changed.notify_all();
  }
  guint64 Start() {
    std::lock_guard<std::mutex> lock(mutex);
    // A new unflushed segment also invalidates outstanding audio waits. The
    // initial segment retains the epoch so audio may preroll ahead of video.
    if (active) {
      ++epoch;
      audio_end = GST_CLOCK_TIME_NONE;
      audio_ended = false;
    }
    active = true;
    cancelled = false;
    ended = false;
    history.clear();
    offset = 0;
    changed.notify_all();
    return epoch;
  }
  bool Stopping() {
    auto* element = GST_ELEMENT(g_weak_ref_get(&pipeline));
    if (!element)
      return true;
    GST_OBJECT_LOCK(element);
    const auto target = GST_STATE_TARGET(element);
    GST_OBJECT_UNLOCK(element);
    gst_object_unref(element);
    return target <= GST_STATE_READY;
  }
  GstClockTime RunningNow() {
    auto* element = GST_ELEMENT(g_weak_ref_get(&pipeline));
    if (!element)
      return GST_CLOCK_TIME_NONE;
    GST_OBJECT_LOCK(element);
    const bool playing = GST_STATE(element) == GST_STATE_PLAYING;
    GST_OBJECT_UNLOCK(element);
    auto* clock = playing ? gst_element_get_clock(element) : nullptr;
    const auto now = clock ? gst_clock_get_time(clock) : GST_CLOCK_TIME_NONE;
    const auto base = gst_element_get_base_time(element);
    const auto running = GST_CLOCK_TIME_IS_VALID(now) && now >= base ? now - base : GST_CLOCK_TIME_NONE;
    if (clock)
      gst_object_unref(clock);
    gst_object_unref(element);
    return running;
  }
  bool Paused() {
    auto* element = GST_ELEMENT(g_weak_ref_get(&pipeline));
    if (!element)
      return false;
    GST_OBJECT_LOCK(element);
    const bool paused = GST_STATE_TARGET(element) == GST_STATE_PAUSED;
    GST_OBJECT_UNLOCK(element);
    gst_object_unref(element);
    return paused;
  }
  bool UpdateAudioQueueLocked() {
    if (!maximum_interval)
      return true;
    // Each private branch can admit one complete video interval before its
    // mapper waits. This prevents a tee branch from blocking its own input.
    for (const auto& audio : audio_queues) {
      if (!audio->rate || !audio->bytes_per_frame)
        continue;
      const auto samples = gst_util_uint64_scale_ceil(maximum_interval, audio->rate, GST_SECOND);
      if (samples > (G_MAXUINT - settings.audio_queue_bytes) / audio->bytes_per_frame ||
          samples > G_MAXUINT - settings.audio_queue_buffers)
        return false;
      auto* queue = GST_ELEMENT(g_weak_ref_get(&audio->element));
      if (!queue)
        return false;
      g_object_set(
          queue,
          "max-size-buffers",
          guint(samples + settings.audio_queue_buffers),
          "max-size-bytes",
          guint(samples * audio->bytes_per_frame + settings.audio_queue_bytes),
          nullptr);
      gst_object_unref(queue);
    }
    return true;
  }
  bool AudioFormat(const std::shared_ptr<AudioQueue>& audio, guint rate, guint bytes_per_frame) {
    std::lock_guard<std::mutex> lock(mutex);
    audio->rate = rate;
    audio->bytes_per_frame = bytes_per_frame;
    return rate && bytes_per_frame && UpdateAudioQueueLocked();
  }
  void FinalizeLocked(GstClockTime now) {
    for (auto& interval : history) {
      if (interval.offset >= 0)
        continue;
      if (audio_attached && !audio_ended && (!GST_CLOCK_TIME_IS_VALID(audio_end) || interval.end > audio_end))
        break;
      if (GST_CLOCK_TIME_IS_VALID(now) && now > interval.start && now - interval.start <= guint64(INT64_MAX))
        offset = std::max(offset, GstClockTimeDiff(now - interval.start));
      interval.offset = offset;
    }
    changed.notify_all();
  }
  GstFlowReturn Record(guint64 expected_epoch, GstClockTime start, GstClockTime duration) {
    if (!GST_CLOCK_TIME_IS_VALID(start) || !GST_CLOCK_TIME_IS_VALID(duration) || !duration ||
        duration > guint64(INT64_MAX) || start > guint64(INT64_MAX) - duration)
      return GST_FLOW_ERROR;
    const auto now = RunningNow();
    std::lock_guard<std::mutex> lock(mutex);
    if (epoch != expected_epoch || cancelled)
      return GST_FLOW_FLUSHING;
    if (!active || (!history.empty() && start <= history.back().start))
      return GST_FLOW_ERROR;
    if (duration > maximum_interval) {
      maximum_interval = duration;
      if (audio_attached && !UpdateAudioQueueLocked())
        return GST_FLOW_ERROR;
    }
    history.push_back({start, start + duration, -1});
    GST_LOG(
        "record epoch=%" G_GUINT64_FORMAT " start=%" G_GUINT64_FORMAT " end=%" G_GUINT64_FORMAT
        " audio-end=%" G_GUINT64_FORMAT,
        epoch,
        start,
        start + duration,
        audio_end);
    while (history.size() > settings.history_frames)
      history.pop_front();
    FinalizeLocked(now);
    return GST_FLOW_OK;
  }
  GstFlowReturn AdmitAudio(guint64 expected_epoch, GstClockTime end, bool eos = false) {
    if (!eos && (!GST_CLOCK_TIME_IS_VALID(end) || end > guint64(INT64_MAX)))
      return GST_FLOW_ERROR;
    const auto now = RunningNow();
    std::lock_guard<std::mutex> lock(mutex);
    if (epoch != expected_epoch)
      return GST_FLOW_FLUSHING;
    if (eos) {
      audio_ended = true;
    } else {
      // All private consumers read the same source PCM. An earlier tee branch
      // may already have admitted this interval; each branch checks its own
      // monotonic input before contributing to the common availability bound.
      if (!GST_CLOCK_TIME_IS_VALID(audio_end) || end > audio_end)
        audio_end = end;
      GST_LOG("admit epoch=%" G_GUINT64_FORMAT " audio-end=%" G_GUINT64_FORMAT, epoch, end);
    }
    FinalizeLocked(now);
    return GST_FLOW_OK;
  }
  GstFlowReturn BeginAudioPush(guint64 expected_epoch) {
    std::unique_lock<std::mutex> lock(mutex);
    while (pause_pending && epoch == expected_epoch && !cancelled && !Stopping())
      changed.wait_for(lock, std::chrono::milliseconds(20));
    if (epoch != expected_epoch || cancelled || Stopping())
      return GST_FLOW_FLUSHING;
    ++audio_pushes;
    return GST_FLOW_OK;
  }
  void EndAudioPush() {
    std::lock_guard<std::mutex> lock(mutex);
    --audio_pushes;
    changed.notify_all();
  }
  guint64 AudioSegment() {
    std::lock_guard<std::mutex> lock(mutex);
    // Audio may begin the next READY/flush epoch before video prerolls. It
    // opens waits, but only video may establish the presentation mapping.
    cancelled = false;
    return epoch;
  }
  guint64 Epoch() {
    std::lock_guard<std::mutex> lock(mutex);
    return epoch;
  }
  // OK, FLUSHING and ERROR are deliberately distinct: cancellation cannot
  // masquerade as a successful presentation or discard a live packet silently.
  GstFlowReturn Lookup(
      guint64 expected_epoch,
      GstClockTime time,
      Mapping* mapping,
      const std::atomic<bool>* retired = nullptr) {
    if (!GST_CLOCK_TIME_IS_VALID(time))
      return GST_FLOW_ERROR;
    auto deadline = Clock::now() + std::chrono::nanoseconds(settings.mapping_timeout_ns);
    std::unique_lock<std::mutex> lock(mutex);
    GST_LOG(
        "lookup epoch=%" G_GUINT64_FORMAT " time=%" G_GUINT64_FORMAT " history=%zu last-end=%" G_GUINT64_FORMAT
        " audio-end=%" G_GUINT64_FORMAT,
        expected_epoch,
        time,
        history.size(),
        history.empty() ? GST_CLOCK_TIME_NONE : history.back().end,
        audio_end);
    for (;;) {
      if (epoch != expected_epoch || cancelled || (retired && retired->load(std::memory_order_acquire)))
        return GST_FLOW_FLUSHING;
      if (!history.empty()) {
        if (time < history.front().start)
          return GST_FLOW_ERROR; // Required mapping was evicted or input precedes video.
        const auto next =
            std::upper_bound(history.begin(), history.end(), time, [](auto time, const Interval& interval) {
              return time < interval.start;
            });
        const auto& interval = *std::prev(next);
        // Keep displaying the previous frame until the next actual PTS. Fixed
        // caps durations can round down while precise frame PTS rounds up;
        // audio sample boundaries can land in that gap, including a single ns.
        const auto boundary = next == history.end() ? interval.end : next->start;
        if (interval.offset >= 0 && (time < boundary || ended)) {
          *mapping = {
              ended && next == history.end() ? GST_CLOCK_TIME_NONE : boundary,
              interval.offset,
              interval.end - interval.start};
          return GST_FLOW_OK;
        }
      }
      if ((ended && audio_ended) || Stopping())
        return GST_FLOW_FLUSHING;
      if (Paused())
        deadline = Clock::now() + std::chrono::nanoseconds(settings.mapping_timeout_ns);
      if (Clock::now() >= deadline)
        return GST_FLOW_ERROR;
      // GstBin can begin changing sinks to READY before the audio adapter's
      // state callback runs. Inspect the pipeline target at bounded intervals
      // so a probe never prevents that transition from reaching its canceller.
      changed.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
    }
  }
};

namespace {
GstPadProbeReturn RecordVideo(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto& probe = *static_cast<VideoProbe*>(data);
  auto& owner = probe.owner->impl();
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    switch (GST_EVENT_TYPE(event)) {
      case GST_EVENT_FLUSH_START:
        owner.Cancel();
        break;
      case GST_EVENT_SEGMENT: {
        const GstSegment* segment{};
        gst_event_parse_segment(event, &segment);
        probe.segment = *segment;
        probe.epoch = owner.Start();
        break;
      }
      case GST_EVENT_CAPS: {
        GstCaps* caps{};
        gst_event_parse_caps(event, &caps);
        GstVideoInfo video{};
        probe.duration = GST_CLOCK_TIME_NONE;
        if (gst_video_info_from_caps(&video, caps) && video.fps_n > 0 && video.fps_d > 0)
          probe.duration = gst_util_uint64_scale(video.fps_d, GST_SECOND, video.fps_n);
        break;
      }
      case GST_EVENT_EOS: {
        std::lock_guard<std::mutex> lock(owner.mutex);
        owner.ended = true;
        owner.changed.notify_all();
        break;
      }
      default:
        break;
    }
  }
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    auto duration = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(duration))
      duration = probe.duration;
    const auto result = owner.Record(probe.epoch, RunningTime(probe.segment, GST_BUFFER_PTS(buffer)), duration);
    if (result == GST_FLOW_FLUSHING)
      return GST_PAD_PROBE_DROP;
    if (result != GST_FLOW_OK) {
      auto* element = gst_pad_get_parent_element(pad);
      Fail(element, "Video requires increasing finite source intervals in the current epoch");
      gst_object_unref(element);
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn ApplyVideo(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto& probe = *static_cast<VideoProbe*>(data);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      const GstSegment* segment{};
      gst_event_parse_segment(event, &segment);
      probe.segment = *segment;
      probe.epoch = probe.owner->impl().Epoch();
    }
  }
  if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;
  auto& owner = probe.owner->impl();
  auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  Mapping mapping{};
  const auto result = owner.Lookup(probe.epoch, RunningTime(probe.segment, GST_BUFFER_PTS(buffer)), &mapping);
  if (result != GST_FLOW_OK) {
    if (result == GST_FLOW_ERROR) {
      auto* element = gst_pad_get_parent_element(pad);
      Fail(element, "Video presentation mapping is missing or exceeded its bounded history");
      gst_object_unref(element);
    }
    return GST_PAD_PROBE_DROP;
  }
  auto* sink = gst_pad_get_parent_element(pad);
  const auto latency = gst_base_sink_get_latency(GST_BASE_SINK(sink));
  if (mapping.duration <= G_MAXUINT64 / probe.retained_frames && latency > mapping.duration * probe.retained_frames) {
    Fail(sink, "Negotiated device lead exceeds this preview's bounded GPU retention");
    gst_object_unref(sink);
    return GST_PAD_PROBE_DROP;
  }
  gst_base_sink_set_ts_offset(GST_BASE_SINK(sink), mapping.offset);
  gst_object_unref(sink);
  return GST_PAD_PROBE_OK;
}

struct AudioAdmission {
  std::shared_ptr<PacedPlayback> owner;
  GstAudioInfo audio;
  GstSegment segment;
  guint64 epoch;
  GstClockTime last_end{GST_CLOCK_TIME_NONE};
  std::shared_ptr<AudioQueue> queue;
  AudioAdmission(std::shared_ptr<PacedPlayback> owner, std::shared_ptr<AudioQueue> queue)
      : owner(std::move(owner)), queue(std::move(queue)) {
    gst_audio_info_init(&audio);
    gst_segment_init(&segment, GST_FORMAT_TIME);
    epoch = this->owner->impl().Epoch();
  }
};
GstPadProbeReturn AdmitAudio(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
    // These private queues retain ordinary CPU PCM and the mapper creates its
    // own sliced buffer headers. A serialized allocation query must not wait
    // behind PCM whose presentation needs the upstream producer's next input:
    // that same producer can be waiting for this query before admitting it.
    // Let upstream converters use their normal system-memory allocator. Other
    // queries and every buffer/event keep the queue's existing ordering.
    if (GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) == GST_QUERY_ALLOCATION)
      return GST_PAD_PROBE_DROP;
    return GST_PAD_PROBE_OK;
  }
  auto& admission = *static_cast<AudioAdmission*>(data);
  auto& owner = admission.owner->impl();
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    switch (GST_EVENT_TYPE(event)) {
      case GST_EVENT_CAPS: {
        GstCaps* caps{};
        gst_event_parse_caps(event, &caps);
        if (!gst_audio_info_from_caps(&admission.audio, caps) ||
            GST_AUDIO_INFO_LAYOUT(&admission.audio) != GST_AUDIO_LAYOUT_INTERLEAVED ||
            !owner.AudioFormat(
                admission.queue, GST_AUDIO_INFO_RATE(&admission.audio), GST_AUDIO_INFO_BPF(&admission.audio))) {
          auto* element = gst_pad_get_parent_element(pad);
          Fail(element, "PCM format and video interval exceed finite audio admission bounds");
          gst_object_unref(element);
          return GST_PAD_PROBE_DROP;
        }
        break;
      }
      case GST_EVENT_SEGMENT: {
        const GstSegment* segment{};
        gst_event_parse_segment(event, &segment);
        admission.segment = *segment;
        admission.epoch = owner.AudioSegment();
        admission.last_end = GST_CLOCK_TIME_NONE;
        break;
      }
      case GST_EVENT_FLUSH_START:
        owner.Cancel();
        break;
      case GST_EVENT_EOS:
        owner.AdmitAudio(admission.epoch, 0, true);
        break;
      default:
        break;
    }
  }
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    const auto pts = GST_BUFFER_PTS(buffer);
    const auto bytes_per_frame = GST_AUDIO_INFO_BPF(&admission.audio);
    const auto rate = GST_AUDIO_INFO_RATE(&admission.audio);
    const auto bytes = gst_buffer_get_size(buffer);
    const auto duration = bytes_per_frame && rate && bytes && bytes % bytes_per_frame == 0
        ? gst_util_uint64_scale_ceil(bytes / bytes_per_frame, GST_SECOND, rate)
        : GST_CLOCK_TIME_NONE;
    const auto end = GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(duration) &&
            duration <= guint64(INT64_MAX) && pts <= guint64(INT64_MAX) - duration
        ? RunningTime(admission.segment, pts + duration)
        : GST_CLOCK_TIME_NONE;
    const auto result = !GST_CLOCK_TIME_IS_VALID(end) ||
            (GST_CLOCK_TIME_IS_VALID(admission.last_end) && end <= admission.last_end) ||
            !GST_CLOCK_TIME_IS_VALID(pts) || !GST_CLOCK_TIME_IS_VALID(duration) || duration > guint64(INT64_MAX) ||
            pts > guint64(INT64_MAX) - duration
        ? GST_FLOW_ERROR
        : owner.AdmitAudio(admission.epoch, end);
    if (result == GST_FLOW_OK)
      admission.last_end = end;
    if (result == GST_FLOW_FLUSHING)
      return GST_PAD_PROBE_DROP;
    if (result != GST_FLOW_OK) {
      auto* element = gst_pad_get_parent_element(pad);
      Fail(element, "Audio admission requires finite increasing interleaved PCM intervals");
      gst_object_unref(element);
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}

struct AudioState {
  std::shared_ptr<PacedPlayback> owner;
  GstAudioInfo audio;
  GstSegment segment;
  GstClockTimeDiff offset{-1};
  guint64 epoch{0};
  bool transport{false};
  GstClockTime transport_start{GST_CLOCK_TIME_NONE};
  guint64 transport_frames{0};
  AudioState() {
    gst_audio_info_init(&audio);
    gst_segment_init(&segment, GST_FORMAT_TIME);
  }
};
struct PacedAudio {
  GstElement parent;
  GstPad *input, *output;
  AudioState* state;
};
struct PacedAudioClass {
  GstElementClass parent;
};
G_DEFINE_TYPE(PacedAudio, paced_audio, GST_TYPE_ELEMENT)

GstFlowReturn PushTransport(PacedAudio* element, GstBuffer* buffer, guint64 frames) {
  auto& state = *element->state;
  const auto rate = GST_AUDIO_INFO_RATE(&state.audio);
  if (!rate || frames > G_MAXUINT64 - state.transport_frames) {
    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
  }
  const auto start = gst_util_uint64_scale(state.transport_frames, GST_SECOND, rate);
  const auto end = gst_util_uint64_scale(state.transport_frames + frames, GST_SECOND, rate);
  if (end > G_MAXUINT64 - state.transport_start) {
    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
  }
  GST_BUFFER_PTS(buffer) = state.transport_start + start;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = end - start;
  auto& owner = state.owner->impl();
  const auto ready = owner.BeginAudioPush(state.epoch);
  if (ready != GST_FLOW_OK) {
    gst_buffer_unref(buffer);
    return ready;
  }
  const auto result = gst_pad_push(element->output, buffer);
  owner.EndAudioPush();
  if (result == GST_FLOW_OK)
    state.transport_frames += frames;
  return result;
}
GstFlowReturn TransportPiece(PacedAudio* element, GstBuffer* piece, guint64 frames, GstClockTime target) {
  auto& state = *element->state;
  const auto rate = GST_AUDIO_INFO_RATE(&state.audio);
  const auto bpf = GST_AUDIO_INFO_BPF(&state.audio);
  if (!GST_CLOCK_TIME_IS_VALID(state.transport_start))
    state.transport_start = target;
  if (target < state.transport_start || !rate || !bpf) {
    gst_buffer_unref(piece);
    return GST_FLOW_ERROR;
  }
  const auto target_frame = gst_util_uint64_scale_round(target - state.transport_start, rate, GST_SECOND);
  const auto chunk_frames = state.owner->settings().audio_queue_bytes / bpf;
  if (!chunk_frames) {
    gst_buffer_unref(piece);
    return GST_FLOW_ERROR;
  }
  while (target_frame > state.transport_frames) {
    const auto count = std::min<guint64>(target_frame - state.transport_frames, chunk_frames);
    auto* silence = gst_buffer_new_allocate(nullptr, count * bpf, nullptr);
    GstMapInfo mapped{};
    if (!silence || !gst_buffer_map(silence, &mapped, GST_MAP_WRITE)) {
      if (silence)
        gst_buffer_unref(silence);
      gst_buffer_unref(piece);
      return GST_FLOW_ERROR;
    }
    gst_audio_format_info_fill_silence(state.audio.finfo, mapped.data, mapped.size);
    gst_buffer_unmap(silence, &mapped);
    const auto result = PushTransport(element, silence, count);
    if (result != GST_FLOW_OK) {
      gst_buffer_unref(piece);
      return result;
    }
  }
  return PushTransport(element, piece, frames);
}

GstFlowReturn AudioChain(GstPad*, GstObject* object, GstBuffer* original) {
  auto* element = reinterpret_cast<PacedAudio*>(object);
  auto& state = *element->state;
  auto& owner = state.owner->impl();
  const auto epoch = state.epoch;
  const auto bytes_per_frame = GST_AUDIO_INFO_BPF(&state.audio);
  const auto rate = GST_AUDIO_INFO_RATE(&state.audio);
  const auto bytes = gst_buffer_get_size(original);
  const auto pts = GST_BUFFER_PTS(original);
  if (!bytes_per_frame || !rate || bytes % bytes_per_frame || !GST_CLOCK_TIME_IS_VALID(pts)) {
    gst_buffer_unref(original);
    Fail(GST_ELEMENT(element), "Monitor audio requires interleaved PCM and finite original timestamps");
    return GST_FLOW_ERROR;
  }
  const auto frames = bytes / bytes_per_frame;
  size_t consumed = 0;
  GstFlowReturn result = GST_FLOW_OK;
  while (consumed < frames) {
    // Packet PTS occupies one integral-nanosecond cell. Use ceiling sample
    // offsets consistently so splitting cannot round a boundary sample back
    // into the preceding video's presentation interval.
    const auto delta = gst_util_uint64_scale_ceil(consumed, GST_SECOND, rate);
    if (pts > guint64(INT64_MAX) - delta) {
      result = GST_FLOW_ERROR;
      break;
    }
    const auto piece_pts = pts + delta;
    const auto time = RunningTime(state.segment, piece_pts);
    Mapping mapping{};
    result = owner.Lookup(epoch, time, &mapping);
    if (result != GST_FLOW_OK)
      break;
    size_t count = frames - consumed;
    if (GST_CLOCK_TIME_IS_VALID(mapping.end)) {
      const auto boundary = gst_segment_position_from_running_time(&state.segment, GST_FORMAT_TIME, mapping.end);
      if (!GST_CLOCK_TIME_IS_VALID(boundary) || boundary <= pts) {
        result = GST_FLOW_ERROR;
        break;
      }
      // ceil(n * SECOND / rate) < boundary - pts iff
      // n <= floor((boundary - pts - 1) * rate / SECOND). Computing a
      // rounded remaining duration instead admits one extra sample when
      // packet PTS and a video boundary share a fractional sample clock.
      const auto boundary_sample = gst_util_uint64_scale(boundary - pts - 1, rate, GST_SECOND) + 1;
      if (boundary_sample <= consumed) {
        result = GST_FLOW_ERROR;
        break;
      }
      count = std::min<guint64>(count, boundary_sample - consumed);
    }
    if (!count) {
      result = GST_FLOW_ERROR;
      break;
    }
    // Copy only the private monitor header and retain its PCM memory. No video
    // memory is touched. The tee's archive header, PTS and samples stay exact.
    auto* piece = gst_buffer_copy_region(
        original,
        static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_MEMORY),
        consumed * bytes_per_frame,
        count * bytes_per_frame);
    if (!piece) {
      result = GST_FLOW_ERROR;
      break;
    }
    GST_BUFFER_PTS(piece) = piece_pts;
    GST_BUFFER_DTS(piece) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(piece) = gst_util_uint64_scale_ceil(consumed + count, GST_SECOND, rate) - delta;
    if (consumed)
      GST_BUFFER_FLAG_UNSET(piece, GST_BUFFER_FLAG_DISCONT);
    if (state.offset != mapping.offset)
      GST_BUFFER_FLAG_SET(piece, GST_BUFFER_FLAG_RESYNC);
    if (state.transport) {
      if (time > G_MAXUINT64 - mapping.offset) {
        gst_buffer_unref(piece);
        result = GST_FLOW_ERROR;
        break;
      }
      result = TransportPiece(element, piece, count, time + mapping.offset);
      if (result != GST_FLOW_OK)
        break;
      state.offset = mapping.offset;
      consumed += count;
      continue;
    }
    auto* peer = gst_pad_get_peer(element->output);
    auto* sink = peer ? gst_pad_get_parent_element(peer) : nullptr;
    if (peer)
      gst_object_unref(peer);
    if (!sink || !GST_IS_AUDIO_BASE_SINK(sink)) {
      if (sink)
        gst_object_unref(sink);
      gst_buffer_unref(piece);
      result = GST_FLOW_ERROR;
      break;
    }
    result = owner.BeginAudioPush(epoch);
    if (result != GST_FLOW_OK) {
      gst_object_unref(sink);
      gst_buffer_unref(piece);
      break;
    }
    gst_base_sink_set_ts_offset(GST_BASE_SINK(sink), mapping.offset);
    gst_object_unref(sink);
    state.offset = mapping.offset;
    result = gst_pad_push(element->output, piece);
    owner.EndAudioPush();
    if (result != GST_FLOW_OK)
      break;
    consumed += count;
  }
  gst_buffer_unref(original);
  if (result == GST_FLOW_ERROR)
    Fail(GST_ELEMENT(element), "Monitor audio mapping is invalid, unavailable or exceeded its bounded history");
  return result;
}

gboolean AudioEvent(GstPad* pad, GstObject* object, GstEvent* event) {
  auto* element = reinterpret_cast<PacedAudio*>(object);
  auto& state = *element->state;
  if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps* caps{};
    gst_event_parse_caps(event, &caps);
    if (!gst_audio_info_from_caps(&state.audio, caps) ||
        GST_AUDIO_INFO_LAYOUT(&state.audio) != GST_AUDIO_LAYOUT_INTERLEAVED) {
      gst_event_unref(event);
      return FALSE;
    }
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
    const GstSegment* segment{};
    gst_event_parse_segment(event, &segment);
    state.segment = *segment;
    state.epoch = state.owner->impl().Epoch();
    state.offset = -1;
    state.transport_start = GST_CLOCK_TIME_NONE;
    state.transport_frames = 0;
    if (state.transport) {
      gst_event_unref(event);
      GstSegment transport;
      gst_segment_init(&transport, GST_FORMAT_TIME);
      event = gst_event_new_segment(&transport);
    }
  }
  return gst_pad_event_default(pad, object, event);
}
GstStateChangeReturn AudioChangeState(GstElement* element, GstStateChange change) {
  auto& state = *reinterpret_cast<PacedAudio*>(element)->state;
  if (state.owner && change == GST_STATE_CHANGE_PAUSED_TO_READY)
    state.owner->Cancel();
  return GST_ELEMENT_CLASS(paced_audio_parent_class)->change_state(element, change);
}
void AudioFinalize(GObject* object) {
  delete reinterpret_cast<PacedAudio*>(object)->state;
  G_OBJECT_CLASS(paced_audio_parent_class)->finalize(object);
}
void paced_audio_class_init(PacedAudioClass* klass) {
  auto* element = GST_ELEMENT_CLASS(klass);
  element->change_state = AudioChangeState;
  G_OBJECT_CLASS(klass)->finalize = AudioFinalize;
  gst_element_class_set_static_metadata(
      element, "Shared paced PCM presentation", "Filter/Audio", "Local monitor", "HStream");
  auto* caps = gst_caps_from_string("audio/x-raw,layout=interleaved");
  gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
}
void paced_audio_init(PacedAudio* element) {
  element->state = new AudioState;
  auto* klass = GST_ELEMENT_GET_CLASS(element);
  element->input = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "sink"), "sink");
  element->output = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "src"), "src");
  GST_PAD_SET_PROXY_CAPS(element->input);
  GST_PAD_SET_PROXY_CAPS(element->output);
  GST_PAD_SET_PROXY_ALLOCATION(element->input);
  gst_pad_set_chain_function(element->input, AudioChain);
  gst_pad_set_event_function(element->input, AudioEvent);
  gst_element_add_pad(GST_ELEMENT(element), element->input);
  gst_element_add_pad(GST_ELEMENT(element), element->output);
}
} // namespace

PacedPlayback::PacedPlayback(GstElement* pipeline, const PacedPlaybackSettings& settings)
    : impl_(std::make_unique<Impl>(pipeline, settings)) {}
PacedPlayback::~PacedPlayback() = default;
std::shared_ptr<PacedPlayback> PacedPlayback::Create(
    GstElement* pipeline,
    GstPad* video_ingress,
    const PacedPlaybackSettings& settings) {
  static std::once_flag debug_once;
  std::call_once(debug_once, [] {
    GST_DEBUG_CATEGORY_INIT(paced_playback_debug, "paced-playback", 0, "Shared local presentation");
  });
  if (!GST_IS_PIPELINE(pipeline) || !video_ingress || !settings.device_buffer_us || !settings.device_period_us ||
      settings.device_period_us > settings.device_buffer_us ||
      settings.device_buffer_us > guint64(INT64_MAX) / GST_USECOND || settings.history_frames < 2 ||
      !settings.audio_queue_buffers || !settings.audio_queue_bytes || settings.preview_frames < 2 ||
      settings.mapping_timeout_ns == 0 || settings.mapping_timeout_ns > guint64(INT64_MAX) ||
      settings.processing_deadline_ns > guint64(INT64_MAX) || !settings.pause_timeout_ns ||
      settings.pause_timeout_ns > guint64(INT64_MAX) || Find(pipeline))
    return {};
  auto owner = std::shared_ptr<PacedPlayback>(new PacedPlayback(pipeline, settings));
  auto* probe = new VideoProbe(owner);
  const auto installed = gst_pad_add_probe(
      video_ingress,
      static_cast<GstPadProbeType>(
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
      RecordVideo,
      probe,
      +[](gpointer data) { delete static_cast<VideoProbe*>(data); });
  if (installed) {
    g_object_set_qdata_full(
        G_OBJECT(pipeline),
        g_quark_from_static_string("hstream-paced-playback-owner-v1"),
        new std::shared_ptr<PacedPlayback>(owner),
        +[](gpointer data) { delete static_cast<std::shared_ptr<PacedPlayback>*>(data); });
  }
  return installed ? owner : nullptr;
}
std::shared_ptr<PacedPlayback> PacedPlayback::Find(GstElement* pipeline) {
  if (!GST_IS_PIPELINE(pipeline))
    return {};
  const auto* pointer = static_cast<std::shared_ptr<PacedPlayback>*>(
      g_object_get_qdata(G_OBJECT(pipeline), g_quark_from_static_string("hstream-paced-playback-owner-v1")));
  return pointer ? *pointer : nullptr;
}
bool PacedPlayback::AttachVideoSink(GstElement* sink, guint retained_frames) {
  if (!GST_IS_BASE_SINK(sink) || !retained_frames)
    return false;
  auto* pad = gst_element_get_static_pad(sink, "sink");
  if (!pad)
    return false;
  auto* probe = new VideoProbe(shared_from_this());
  probe->retained_frames = retained_frames;
  const auto installed = gst_pad_add_probe(
      pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      ApplyVideo,
      probe,
      +[](gpointer data) { delete static_cast<VideoProbe*>(data); });
  gst_object_unref(pad);
  g_object_set(sink, "processing-deadline", impl_->settings.processing_deadline_ns, nullptr);
  return installed != 0;
}
bool PacedPlayback::AttachAudioSink(GstElement* sink) {
  return GST_IS_AUDIO_BASE_SINK(sink) && AttachAudioElement(sink, false);
}
bool PacedPlayback::AttachAudioTransport(GstElement* encoder) {
  return encoder && AttachAudioElement(encoder, true);
}
bool PacedPlayback::AttachAudioElement(GstElement* sink, bool transport) {
  auto* pad = gst_element_get_static_pad(sink, "sink");
  auto* peer = pad ? gst_pad_get_peer(pad) : nullptr;
  auto* parent = gst_element_get_parent(sink);
  if (!pad || !peer || !GST_IS_BIN(parent)) {
    if (pad)
      gst_object_unref(pad);
    if (peer)
      gst_object_unref(peer);
    if (parent)
      gst_object_unref(parent);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* physical = g_weak_ref_get(&impl_->audio_sink);
    if (!transport && physical) {
      gst_object_unref(physical);
      gst_object_unref(pad);
      gst_object_unref(peer);
      gst_object_unref(parent);
      return false;
    }
    if (physical)
      gst_object_unref(physical);
  }
  auto* queue = gst_element_factory_make("queue", nullptr);
  if (!queue) {
    gst_object_unref(pad);
    gst_object_unref(peer);
    gst_object_unref(parent);
    return false;
  }
  auto* adapter = reinterpret_cast<PacedAudio*>(g_object_new(paced_audio_get_type(), nullptr));
  adapter->state->owner = shared_from_this();
  adapter->state->transport = transport;
  g_object_set(
      queue,
      "max-size-buffers",
      impl_->settings.audio_queue_buffers,
      "max-size-bytes",
      impl_->settings.audio_queue_bytes,
      "max-size-time",
      guint64{0},
      "leaky",
      0,
      nullptr);
  const bool added_queue = gst_bin_add(GST_BIN(parent), queue);
  const bool added = gst_bin_add(GST_BIN(parent), GST_ELEMENT(adapter));
  auto* queue_input = gst_element_get_static_pad(queue, "sink");
  const bool unlinked = added_queue && added && gst_pad_unlink(peer, pad);
  const bool linked = unlinked && gst_pad_link(peer, queue_input) == GST_PAD_LINK_OK &&
      gst_element_link(queue, GST_ELEMENT(adapter)) && gst_pad_link(adapter->output, pad) == GST_PAD_LINK_OK;
  if (!linked) {
    gst_pad_unlink(peer, queue_input);
    gst_pad_unlink(adapter->output, pad);
    if (unlinked)
      gst_pad_link(peer, pad);
    if (added)
      gst_bin_remove(GST_BIN(parent), GST_ELEMENT(adapter));
    else
      gst_object_unref(adapter);
    if (added_queue)
      gst_bin_remove(GST_BIN(parent), queue);
    else
      gst_object_unref(queue);
  } else {
    auto admission_queue = std::make_shared<AudioQueue>(queue);
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->audio_attached = true;
      impl_->audio_queues.push_back(admission_queue);
    }
    gst_pad_add_probe(
        queue_input,
        static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH |
            GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
        AdmitAudio,
        new AudioAdmission(shared_from_this(), admission_queue),
        +[](gpointer data) { delete static_cast<AudioAdmission*>(data); });
  }
  gst_object_unref(queue_input);
  gst_object_unref(pad);
  gst_object_unref(peer);
  gst_object_unref(parent);
  if (!linked)
    return false;
  if (transport)
    return true;
  g_object_set(
      sink,
      "buffer-time",
      gint64(impl_->settings.device_buffer_us),
      "latency-time",
      gint64(impl_->settings.device_period_us),
      "processing-deadline",
      impl_->settings.processing_deadline_ns,
      nullptr);
  if (!InstallPacedAudioLatency(sink))
    return false;
  g_weak_ref_set(&impl_->audio_sink, sink);
  // One physical presentation clock preserves the device's sample positions
  // across pause. Slaving it independently to the system clock recalibrates
  // queued PCM backwards on resume. The negotiated lead is checked against
  // every video route's retained frames before playback can advance.
  auto* pipeline = GST_ELEMENT(g_weak_ref_get(&impl_->pipeline));
  if (!pipeline)
    return false;
  g_object_set(sink, "provide-clock", TRUE, nullptr);
  gst_pipeline_use_clock(GST_PIPELINE(pipeline), GST_AUDIO_BASE_SINK(sink)->provided_clock);
  gst_object_unref(pipeline);
  return true;
}
bool PacedPlayback::PreparePause() {
  auto& state = *impl_;
  const auto deadline = Clock::now() + std::chrono::nanoseconds(state.settings.pause_timeout_ns);
  std::unique_lock<std::mutex> lock(state.mutex);
  state.pause_pending = true;
  while (state.audio_pushes && !state.cancelled && !state.Stopping()) {
    if (state.changed.wait_until(lock, deadline) == std::cv_status::timeout) {
      state.pause_pending = false;
      state.changed.notify_all();
      return false;
    }
  }
  return state.PreservePauseBaseLocked();
}
bool PacedPlayback::ResumeAudioClock() {
  auto* sink = GST_AUDIO_BASE_SINK(g_weak_ref_get(&impl_->audio_sink));
  if (!sink) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pause_pending = false;
    impl_->changed.notify_all();
    return true;
  }
  GST_OBJECT_LOCK(sink);
  auto* ring = sink->ringbuffer;
  if (ring)
    gst_object_ref(ring);
  GST_OBJECT_UNLOCK(sink);
  // A process pause can occur during startup before PCM caps acquire the
  // device. There is no preserved cursor to restart in that case.
  const bool resumed = !ring || !gst_audio_ring_buffer_is_acquired(ring) || gst_audio_ring_buffer_start(ring);
  if (ring)
    gst_object_unref(ring);
  gst_object_unref(sink);
  if (resumed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RestorePauseStartTimeLocked();
    impl_->pause_pending = false;
    impl_->changed.notify_all();
  }
  return resumed;
}
GstFlowReturn PacedPlayback::PresentationTime(
    const GstSegment& segment,
    GstClockTime pts,
    GstClockTime* presentation,
    const std::atomic<bool>* cancelled) {
  if (!presentation)
    return GST_FLOW_ERROR;
  const auto time = RunningTime(segment, pts);
  Mapping mapping{};
  const auto result = impl_->Lookup(impl_->Epoch(), time, &mapping, cancelled);
  if (result != GST_FLOW_OK)
    return result;
  if (time > G_MAXUINT64 - mapping.offset)
    return GST_FLOW_ERROR;
  *presentation = time + mapping.offset;
  return GST_FLOW_OK;
}

void PacedPlayback::WakePresentationWaiters() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->changed.notify_all();
}

void PacedPlayback::Cancel() {
  impl_->Cancel();
}
const PacedPlaybackSettings& PacedPlayback::settings() const {
  return impl_->settings;
}
GstStateChangeReturn PreparePacedPipelinePause(GstElement* pipeline) {
  if (!GST_IS_PIPELINE(pipeline))
    return GST_STATE_CHANGE_FAILURE;
  const auto owner = PacedPlayback::Find(pipeline);
  if (owner) {
    GST_OBJECT_LOCK(pipeline);
    const bool playing = GST_STATE(pipeline) == GST_STATE_PLAYING;
    GST_OBJECT_UNLOCK(pipeline);
    if (playing) {
      auto& state = owner->impl();
      std::lock_guard<std::mutex> lock(state.mutex);
      state.pause_pending = true;
      if (state.cancelled || state.Stopping())
        return GST_STATE_CHANGE_FAILURE;
      if (state.audio_pushes)
        return GST_STATE_CHANGE_ASYNC;
      if (!state.PreservePauseBaseLocked())
        return GST_STATE_CHANGE_FAILURE;
    }
  }
  return GST_STATE_CHANGE_SUCCESS;
}
GstStateChangeReturn RequestPacedPipelinePause(GstElement* pipeline) {
  const auto preparation = PreparePacedPipelinePause(pipeline);
  if (preparation != GST_STATE_CHANGE_SUCCESS)
    return preparation;
  const auto result = gst_element_set_state(pipeline, GST_STATE_PAUSED);
  const auto owner = PacedPlayback::Find(pipeline);
  if (result == GST_STATE_CHANGE_FAILURE && owner)
    owner->Cancel();
  return result;
}
GstStateChangeReturn RequestPacedPipelineResume(GstElement* pipeline) {
  if (!GST_IS_PIPELINE(pipeline))
    return GST_STATE_CHANGE_FAILURE;
  const auto result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  const auto owner = PacedPlayback::Find(pipeline);
  if (result != GST_STATE_CHANGE_FAILURE && owner && !owner->ResumeAudioClock())
    return GST_STATE_CHANGE_FAILURE;
  return result;
}
} // namespace hm::gst
