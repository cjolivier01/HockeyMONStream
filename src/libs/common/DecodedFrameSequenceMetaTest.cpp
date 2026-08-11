#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include <gst/gst.h>

#include <cstdint>
#include <iostream>

namespace {

struct PreRegisteredSequenceMeta {
  GstMeta meta;
  guint source_id;
  guint64 sequence;
};

gboolean initialize_meta(GstMeta* meta, gpointer /*params*/, GstBuffer* /*buffer*/) {
  auto* sequence_meta = reinterpret_cast<PreRegisteredSequenceMeta*>(meta);
  sequence_meta->source_id = 0;
  sequence_meta->sequence = 0;
  return TRUE;
}

gboolean transform_meta(GstBuffer* destination, GstMeta* meta, GstBuffer* /*source*/, GQuark type, gpointer /*data*/) {
  if (!GST_META_TRANSFORM_IS_COPY(type)) {
    return FALSE;
  }
  const auto* source_meta = reinterpret_cast<const PreRegisteredSequenceMeta*>(meta);
  auto* destination_meta =
      reinterpret_cast<PreRegisteredSequenceMeta*>(gst_buffer_add_meta(destination, meta->info, nullptr));
  if (!destination_meta) {
    return FALSE;
  }
  destination_meta->source_id = source_meta->source_id;
  destination_meta->sequence = source_meta->sequence;
  return TRUE;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  static const gchar* tags[] = {"hstream", "decoded-frame-sequence", nullptr};
  const GType api = gst_meta_api_type_register("GstHmDecodedFrameSequenceMetaAPI", tags);
  if (api == G_TYPE_INVALID) {
    std::cerr << "Could not pre-register decoded-frame sequence API\n";
    return 1;
  }
  const GstMetaInfo* info = gst_meta_register(
      api,
      "GstHmDecodedFrameSequenceMeta",
      sizeof(PreRegisteredSequenceMeta),
      initialize_meta,
      nullptr,
      transform_meta);
  if (!info) {
    std::cerr << "Could not pre-register decoded-frame sequence implementation\n";
    return 1;
  }

  GstBuffer* buffer = gst_buffer_new();
  const bool added = hm::add_decoded_frame_sequence_meta(buffer, 7, 1234);
  const std::optional<hm::DecodedFrameSequence> sequence = hm::decoded_frame_sequence(buffer);
  GstBuffer* copy = gst_buffer_copy_deep(buffer);
  const std::optional<hm::DecodedFrameSequence> copied_sequence = hm::decoded_frame_sequence(copy);
  gst_buffer_unref(copy);
  gst_buffer_unref(buffer);

  if (!added || !sequence.has_value() || sequence->source_id != 7 || sequence->sequence != 1234 ||
      !copied_sequence.has_value() || copied_sequence->source_id != 7 || copied_sequence->sequence != 1234) {
    std::cerr << "Did not reuse the process-global decoded-frame sequence metadata registration\n";
    return 1;
  }
  return 0;
}
