#include "hstream/src/apps/apps-common/DecoderPadLink.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::abort();
  }
}
gboolean Query(GstPad* pad, GstObject*, GstQuery* query) {
  const auto* text = static_cast<const char*>(g_object_get_data(G_OBJECT(pad), "caps"));
  auto* caps = gst_caps_from_string(text);
  bool success = true;
  if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS) {
    GstCaps* filter = nullptr;
    gst_query_parse_caps(query, &filter);
    auto* result = filter ? gst_caps_intersect(filter, caps) : gst_caps_ref(caps);
    gst_query_set_caps_result(query, result);
    gst_caps_unref(result);
  } else if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps* value = nullptr;
    gst_query_parse_accept_caps(query, &value);
    gst_query_set_accept_caps_result(query, gst_caps_is_subset(value, caps));
  } else {
    success = false;
  }
  gst_caps_unref(caps);
  return success;
}
void Sticky(GstPad* pad, const char* text) {
  auto* caps = gst_caps_from_string(text);
  auto* event = gst_event_new_caps(caps);
  Check(gst_pad_store_sticky_event(pad, event) == GST_FLOW_OK, "Could not store negotiated decoder CAPS");
  gst_event_unref(event);
  gst_caps_unref(caps);
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  auto* source = gst_pad_new("decoder", GST_PAD_SRC);
  auto* sink = gst_pad_new("native-preview", GST_PAD_SINK);
  gst_pad_set_active(source, TRUE);
  gst_pad_set_active(sink, TRUE);
  gst_pad_set_query_function(source, Query);
  gst_pad_set_query_function(sink, Query);
  g_object_set_data(G_OBJECT(source), "caps", const_cast<char*>("video/x-raw,format=NV12"));
  g_object_set_data(G_OBJECT(sink), "caps", const_cast<char*>("video/x-raw(memory:NVMM),format=NV12"));
  Sticky(source, "video/x-raw(memory:NVMM),format=NV12,width=1920,height=1080,framerate=30/1");
  Check(
      hm::gst::LinkDecodedPad(source, sink, false) == GST_PAD_LINK_NOFORMAT,
      "Unknown decoder bypassed query validation");
  Check(
      hm::gst::LinkDecodedPad(source, sink, true) == GST_PAD_LINK_OK,
      "Native decoder's accepted negotiated NVMM caps did not resolve its misleading query");
  gst_pad_unlink(source, sink);
  Sticky(source, "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1");
  Check(
      hm::gst::LinkDecodedPad(source, sink, true) == GST_PAD_LINK_NOFORMAT,
      "System-memory negotiated video bypassed NVMM admission");
  Sticky(source, "video/x-raw(memory:NVMM),format=RGBA,width=1920,height=1080,framerate=30/1");
  Check(
      hm::gst::LinkDecodedPad(source, sink, true) == GST_PAD_LINK_NOFORMAT,
      "Native NVMM retry bypassed downstream format rejection");
  gst_object_unref(source);
  gst_object_unref(sink);
  return 0;
}
