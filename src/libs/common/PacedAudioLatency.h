#pragma once

#include <gst/gst.h>

namespace hm::gst {

// A clock-paced producer cannot fill the audio device arbitrarily far ahead,
// even when its input is a file. Report that constraint in latency queries so
// GstAudioBaseSink includes its negotiated device lead in the shared pipeline
// latency. Media timestamps, source live state and archive samples are unchanged.
// Call after linking the audio sink and before starting the graph. The linked
// pad owns the probe, including across READY/seek renegotiation. The pipeline
// owner must recalculate latency on GST_MESSAGE_LATENCY, on its control thread.
bool InstallPacedAudioLatency(GstElement* audio_sink);

} // namespace hm::gst
