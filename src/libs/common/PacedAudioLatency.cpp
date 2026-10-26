#include "hstream/src/libs/common/PacedAudioLatency.h"

#include <gst/audio/gstaudiobasesink.h>

namespace hm::gst {

bool InstallPacedAudioLatency(GstElement* audio_sink) {
  if (!GST_IS_AUDIO_BASE_SINK(audio_sink))
    return false;
  GstPad* input = gst_element_get_static_pad(audio_sink, "sink");
  GstPad* peer = input ? gst_pad_get_peer(input) : nullptr;
  if (input)
    gst_object_unref(input);
  if (!peer)
    return false;
  const auto probe = gst_pad_add_probe(
      peer,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_QUERY_UPSTREAM | GST_PAD_PROBE_TYPE_PULL),
      +[](GstPad*, GstPadProbeInfo* info, gpointer) {
        GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
          gboolean live = FALSE;
          GstClockTime minimum = 0, maximum = GST_CLOCK_TIME_NONE;
          gst_query_parse_latency(query, &live, &minimum, &maximum);
          // Only modify the reply. Preserve the producer's actual requirements;
          // GstAudioBaseSink adds its own negotiated physical device latency.
          // Without this constraint file-backed input reports zero latency and
          // the device's ringbuffer skips on-time samples behind its write head,
          // independently of the sink's max-lateness policy.
          gst_query_set_latency(query, TRUE, minimum, maximum);
        }
        return GST_PAD_PROBE_OK;
      },
      nullptr,
      nullptr);
  gst_object_unref(peer);
  return probe != 0;
}

} // namespace hm::gst
