#pragma once

#include <gst/gst.h>

#include <atomic>
#include <memory>

namespace hm::gst {

// Required resolved bounds. These are policy inputs, never fallback defaults.
struct PacedPlaybackSettings {
  guint64 device_buffer_us;
  guint64 device_period_us;
  guint64 processing_deadline_ns;
  guint history_frames;
  guint64 mapping_timeout_ns;
  // Finite headroom in addition to one actual video interval of negotiated
  // PCM. The sample/byte bounds are derived together to avoid admission cycles.
  guint audio_queue_buffers;
  guint audio_queue_bytes;
  guint preview_frames;
  guint64 pause_timeout_ns;
};

// A local playback epoch maps each original video interval to one presentation
// interval. Slow processing inserts the same stall in audio and video. Original
// buffer timestamps and archive branches are untouched; only monitor sinks use
// the mapping. Video buffers are never mapped or copied to host memory.
class PacedPlayback : public std::enable_shared_from_this<PacedPlayback> {
 public:
  static std::shared_ptr<PacedPlayback> Create(
      GstElement* pipeline,
      GstPad* video_ingress,
      const PacedPlaybackSettings& settings);
  // Control-thread lookup for routes added during source reconstruction.
  static std::shared_ptr<PacedPlayback> Find(GstElement* pipeline);
  ~PacedPlayback();
  PacedPlayback(const PacedPlayback&) = delete;
  PacedPlayback& operator=(const PacedPlayback&) = delete;

  // Video ingress must precede its bounded presentation queue. Attach before
  // starting the graph. Every sink applies its buffer's own mapping, so queued
  // frames cannot accidentally use a later frame's offset.
  bool AttachVideoSink(GstElement* sink, guint retained_frames);
  const PacedPlaybackSettings& settings() const;
  // Insert immediately before a linked GstAudioBaseSink, after conversion and
  // volume. Own the pipeline's physical audio clock and split private monitor
  // headers at source video/sample boundaries. Shared PCM memory and the
  // archive's original headers remain unchanged.
  bool AttachAudioSink(GstElement* sink);
  // Insert the same bounded PCM mapper before a linked raw-audio encoder.
  // A transport has no physical clock: it uses the pipeline clock and encodes
  // explicit silence for shared video stalls, retaining every source sample.
  bool AttachAudioTransport(GstElement* encoder);
  // Resolve a preview header on the same epoch/interval mapping as PCM. This
  // may wait for admitted video/audio, and returns FLUSHING on cancellation.
  GstFlowReturn PresentationTime(
      const GstSegment& segment,
      GstClockTime pts,
      GstClockTime* presentation,
      const std::atomic<bool>* cancelled = nullptr);
  // Wake a retired preview route without cancelling PCM or changing the shared
  // playback epoch. Its cancellation flag must be set before this notification.
  void WakePresentationWaiters();
  // Wake mapping waiters before flush, reconstruction or destruction. The next
  // serialized video SEGMENT opens a fresh epoch with an empty bounded history.
  void Cancel();
  // After a successful PAUSED -> PLAYING transition, start the preserved
  // device cursor immediately, then release quiesced PCM. GstAudioBaseSink
  // otherwise defers physical resume until the next packet arrives.
  bool ResumeAudioClock();
  // Quiesce private PCM pushes while PLAYING before changing pipeline state.
  // Retain pending PCM and the shared clock base until ResumeAudioClock
  // restarts the device cursor. Cancel restores ordinary flush bookkeeping.
  bool PreparePause();

  struct Impl;
  Impl& impl() {
    return *impl_;
  }

 private:
  PacedPlayback(GstElement* pipeline, const PacedPlaybackSettings& settings);
  bool AttachAudioElement(GstElement* element, bool transport);
  std::unique_ptr<Impl> impl_;
};

// Control-loop requests for peers which survive a same-stage restart. Prepare
// quiesces PCM without changing state; Request also asks the graph to pause.
// Neither waits for an active PCM push: ASYNC also means quiescence is pending,
// so retry until PAUSED under the caller's existing restart deadline. Aborting
// the restart must Cancel its owner before retiring the graph.
GstStateChangeReturn PreparePacedPipelinePause(GstElement* pipeline);
GstStateChangeReturn RequestPacedPipelinePause(GstElement* pipeline);
GstStateChangeReturn RequestPacedPipelineResume(GstElement* pipeline);

} // namespace hm::gst
