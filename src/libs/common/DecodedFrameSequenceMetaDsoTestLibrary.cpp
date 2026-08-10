#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include <gst/gst.h>

extern "C" bool decoded_frame_sequence_meta_dso_initialize() {
  GstBuffer* buffer = gst_buffer_new();
  const bool added = hm::add_decoded_frame_sequence_meta(buffer, 1, 1);
  const std::optional<hm::DecodedFrameSequence> sequence = hm::decoded_frame_sequence(buffer);
  gst_buffer_unref(buffer);
  return added && sequence.has_value() && sequence->source_id == 1 && sequence->sequence == 1;
}

extern "C" bool decoded_frame_sequence_meta_dso_add(GstBuffer* buffer, guint source_id, guint64 sequence) {
  return hm::add_decoded_frame_sequence_meta(buffer, source_id, sequence);
}

extern "C" bool decoded_frame_sequence_meta_dso_read(GstBuffer* buffer, guint* source_id, guint64* sequence) {
  const std::optional<hm::DecodedFrameSequence> decoded = hm::decoded_frame_sequence(buffer);
  if (!decoded.has_value()) {
    return false;
  }
  *source_id = decoded->source_id;
  *sequence = decoded->sequence;
  return true;
}
