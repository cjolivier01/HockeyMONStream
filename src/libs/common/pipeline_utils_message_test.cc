// Simple unit tests for stitch-configured message helpers.
// Focuses on round-trip creation, detection, and parsing.

#include "hstream/src/libs/common/pipeline_utils.h"

#include <cassert>
#include <cstring>
#include <gst/gst.h>

int main() {
  gst_init(nullptr, nullptr);

  // Create a pipeline as message source context
  GstElement* pipeline = gst_pipeline_new("test-pipeline");
  assert(pipeline);

  const int width = 1920;
  const int height = 1080;
  const char* cfg_dir = "/tmp/game_dir";

  // Create message and validate detection/parsing
  GstMessage* m = hm::gst_nvmessage_stitch_configured(GST_OBJECT(pipeline), width, height, cfg_dir);
  assert(m);
  assert(hm::gst_message_is_stitch_configured(m));

  int pw = 0, ph = 0;
  const char* pdir = nullptr;
  bool ok = hm::gst_message_parse_stitch_configured(m, &pw, &ph, &pdir);
  assert(ok);
  assert(pw == width);
  assert(ph == height);
  assert(pdir && std::strcmp(pdir, cfg_dir) == 0);

  gst_message_unref(m);

  // Negative test: other message should not match
  GstMessage* eos = gst_message_new_eos(GST_OBJECT(pipeline));
  assert(eos);
  assert(!hm::gst_message_is_stitch_configured(eos));
  int w2 = 0, h2 = 0; const char* d2 = nullptr;
  bool parsed = hm::gst_message_parse_stitch_configured(eos, &w2, &h2, &d2);
  assert(parsed == false);
  gst_message_unref(eos);

  gst_object_unref(pipeline);
  return 0;
}

