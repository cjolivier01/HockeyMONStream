#include "hstream/src/apps/apps-common/HStreamLosslessMux.h"

#include <gst/gst.h>

#include <iostream>

namespace {

bool template_accepts_p010(GstElementFactory* factory, const char* name_template, GstPadDirection direction) {
  const GList* templates = gst_element_factory_get_static_pad_templates(factory);
  for (const GList* item = templates; item; item = item->next) {
    const auto* pad_template = static_cast<const GstStaticPadTemplate*>(item->data);
    if (!pad_template || pad_template->direction != direction ||
        g_strcmp0(pad_template->name_template, name_template) != 0) {
      continue;
    }
    GstCaps* caps = gst_static_caps_get(const_cast<GstStaticCaps*>(&pad_template->static_caps));
    GstCaps* required = gst_caps_from_string("video/x-raw(memory:NVMM),format=P010_10LE");
    const bool accepts = caps && required && gst_caps_can_intersect(caps, required);
    if (required) {
      gst_caps_unref(required);
    }
    if (caps) {
      gst_caps_unref(caps);
    }
    return accepts;
  }
  return false;
}

bool queried_sink_caps_preserve_p010() {
  GstElement* mux = gst_element_factory_make("hstreamlosslessmux", nullptr);
  GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
  if (!mux || !filter) {
    if (filter) {
      gst_object_unref(filter);
    }
    if (mux) {
      gst_object_unref(mux);
    }
    return false;
  }

  GstCaps* downstream_caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=(string){P010_10LE,NV12,RGBA},width=(int)[1,MAX],height=(int)[1,MAX]");
  g_object_set(filter, "caps", downstream_caps, nullptr);
  gst_caps_unref(downstream_caps);

  GstPad* mux_sink = gst_element_request_pad_simple(mux, "sink_0");
  GstPad* mux_src = gst_element_get_static_pad(mux, "src");
  GstPad* filter_sink = gst_element_get_static_pad(filter, "sink");
  bool accepts = false;
  if (mux_sink && mux_src && filter_sink && gst_pad_link(mux_src, filter_sink) == GST_PAD_LINK_OK) {
    GstCaps* queried = gst_pad_query_caps(mux_sink, nullptr);
    GstCaps* required = gst_caps_from_string("video/x-raw(memory:NVMM),format=P010_10LE");
    accepts = queried && required && gst_caps_can_intersect(queried, required);
    if (!accepts && queried) {
      gchar* text = gst_caps_to_string(queried);
      std::cerr << "Requested sink caps query dropped P010: " << (text ? text : "(null)") << '\n';
      g_free(text);
    }
    if (required) {
      gst_caps_unref(required);
    }
    if (queried) {
      gst_caps_unref(queried);
    }
    gst_pad_unlink(mux_src, filter_sink);
  }
  if (mux_sink) {
    gst_element_release_request_pad(mux, mux_sink);
    gst_object_unref(mux_sink);
  }
  if (filter_sink) {
    gst_object_unref(filter_sink);
  }
  if (mux_src) {
    gst_object_unref(mux_src);
  }
  gst_object_unref(filter);
  gst_object_unref(mux);
  return accepts;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!register_hstream_lossless_nvstreammux()) {
    std::cerr << "Could not register the HStream lossless mux\n";
    return 1;
  }
  GstElementFactory* factory = gst_element_factory_find("hstreamlosslessmux");
  if (!factory) {
    std::cerr << "Could not find the registered HStream lossless mux\n";
    return 1;
  }
  const bool sink_accepts = template_accepts_p010(factory, "sink_%u", GST_PAD_SINK);
  const bool source_accepts = template_accepts_p010(factory, "src", GST_PAD_SRC);
  gst_object_unref(factory);
  if (!sink_accepts || !source_accepts || !queried_sink_caps_preserve_p010()) {
    std::cerr << "The lossless mux must preserve P010 in templates and requested-pad caps queries\n";
    return 1;
  }
  return 0;
}
