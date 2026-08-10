#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include <gst/gst.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>

namespace {

enum class InvalidRegistration {
  kWrongSize,
  kWrongApi,
  kMissingTransform,
};

struct TestSequenceMeta {
  GstMeta meta;
  guint source_id;
  guint64 sequence;
};

gboolean initialize_meta(GstMeta* /*meta*/, gpointer /*params*/, GstBuffer* /*buffer*/) {
  return TRUE;
}

gboolean transform_meta(
    GstBuffer* /*destination*/,
    GstMeta* /*meta*/,
    GstBuffer* /*source*/,
    GQuark /*type*/,
    gpointer /*data*/) {
  return TRUE;
}

bool run_case(InvalidRegistration registration, int argc, char** argv) {
  gst_init(&argc, &argv);

  static const gchar* tags[] = {"hstream", "decoded-frame-sequence", nullptr};
  const char* api_name = registration == InvalidRegistration::kWrongApi ? "GstHmDecodedFrameSequenceMetaWrongAPI"
                                                                        : "GstHmDecodedFrameSequenceMetaAPI";
  const GType api = gst_meta_api_type_register(api_name, tags);
  if (api == G_TYPE_INVALID) {
    return false;
  }

  const gsize size = registration == InvalidRegistration::kWrongSize ? sizeof(GstMeta) : sizeof(TestSequenceMeta);
  const GstMetaTransformFunction transform =
      registration == InvalidRegistration::kMissingTransform ? nullptr : transform_meta;
  if (!gst_meta_register(api, "GstHmDecodedFrameSequenceMeta", size, initialize_meta, nullptr, transform)) {
    return false;
  }

  GstBuffer* buffer = gst_buffer_new();
  const bool added = hm::add_decoded_frame_sequence_meta(buffer, 7, 1234);
  const bool found = hm::decoded_frame_sequence(buffer).has_value();
  gst_buffer_unref(buffer);
  return !added && !found;
}

bool run_in_child(InvalidRegistration registration, int argc, char** argv) {
  const pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    _exit(run_case(registration, argc, argv) ? 0 : 1);
  }

  int status = 0;
  return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main(int argc, char** argv) {
  if (!run_in_child(InvalidRegistration::kWrongSize, argc, argv) ||
      !run_in_child(InvalidRegistration::kWrongApi, argc, argv) ||
      !run_in_child(InvalidRegistration::kMissingTransform, argc, argv)) {
    std::cerr << "Decoded-frame metadata accepted an incompatible process-global registration\n";
    return 1;
  }
  return 0;
}
