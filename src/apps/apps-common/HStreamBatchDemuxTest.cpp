#include "hstream/src/apps/apps-common/HStreamBatchDemux.h"

#include <gst-nvevent.h>
#include <gst/gst.h>

#include <iostream>

namespace {

GstPadProbeReturn accept_downstream_events(GstPad*, GstPadProbeInfo*, gpointer) {
  return GST_PAD_PROBE_OK;
}

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
    if (required)
      gst_caps_unref(required);
    if (caps)
      gst_caps_unref(caps);
    return accepts;
  }
  return false;
}

bool expect_stitched_geometry_after_camera_update(gint stitched_width, gint stitched_height) {
  GstElement* demux = gst_element_factory_make("hstreambatchdemux", nullptr);
  if (!demux) {
    return false;
  }

  GstPad* demux_src = gst_element_request_pad_simple(demux, "src_0");
  GstPad* sink_pad = gst_pad_new("test_sink", GST_PAD_SINK);
  GstPad* demux_sink = gst_element_get_static_pad(demux, "sink");
  if (sink_pad)
    gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, accept_downstream_events, nullptr, nullptr);
  bool ok = demux_src && sink_pad && demux_sink && gst_pad_set_active(sink_pad, TRUE) &&
      gst_pad_set_active(demux_sink, TRUE) && gst_pad_link(demux_src, sink_pad) == GST_PAD_LINK_OK;
  if (ok) {
    GstCaps* stitched_caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "P010_10LE",
        "width",
        G_TYPE_INT,
        stitched_width,
        "height",
        G_TYPE_INT,
        stitched_height,
        "framerate",
        GST_TYPE_FRACTION,
        60,
        1,
        "batch-size",
        G_TYPE_UINT,
        2u,
        nullptr);
    gst_caps_set_features(stitched_caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
    ok = gst_pad_send_event(demux_sink, gst_event_new_caps(stitched_caps));
    GstStructure* camera_caps = gst_structure_new_empty("video/x-raw");
    ok =
        ok &&
        gst_pad_send_event(
            demux_sink, gst_nvevent_new_update_caps(0, 3840, 2160, camera_caps, const_cast<gchar*>("camera-0"), FALSE));
    gst_caps_unref(stitched_caps);

    GstCaps* output_caps = gst_pad_get_current_caps(demux_src);
    gint output_width = 0;
    gint output_height = 0;
    guint output_batch_size = 0;
    const GstStructure* output_structure = output_caps ? gst_caps_get_structure(output_caps, 0) : nullptr;
    ok = ok && output_structure && gst_structure_get_int(output_structure, "width", &output_width) &&
        gst_structure_get_int(output_structure, "height", &output_height) &&
        gst_structure_get_uint(output_structure, "batch-size", &output_batch_size) && output_width == stitched_width &&
        output_height == stitched_height && output_batch_size == 1;
    if (!ok && output_caps) {
      gchar* text = gst_caps_to_string(output_caps);
      std::cerr << "Unexpected stitched demux caps: " << (text ? text : "(null)") << '\n';
      g_free(text);
    }
    if (output_caps)
      gst_caps_unref(output_caps);
  }

  if (demux_sink)
    gst_pad_send_event(demux_sink, gst_nvevent_new_stream_eos(0));
  if (demux_src) {
    if (sink_pad)
      gst_pad_unlink(demux_src, sink_pad);
    gst_element_release_request_pad(demux, demux_src);
    gst_object_unref(demux_src);
  }
  if (sink_pad)
    gst_pad_set_active(sink_pad, FALSE);
  if (sink_pad)
    gst_object_unref(sink_pad);
  if (demux_sink) {
    gst_pad_set_active(demux_sink, FALSE);
    gst_object_unref(demux_sink);
  }
  gst_object_unref(demux);
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!register_hstream_batch_demux()) {
    std::cerr << "Could not register the HStream batch demuxer\n";
    return 1;
  }
  GstElementFactory* factory = gst_element_factory_find("hstreambatchdemux");
  if (!factory) {
    std::cerr << "Could not find the registered HStream batch demuxer\n";
    return 1;
  }
  const bool sink_accepts = template_accepts_p010(factory, "sink", GST_PAD_SINK);
  const bool source_accepts = template_accepts_p010(factory, "src_%u", GST_PAD_SRC);
  gst_object_unref(factory);
  if (!sink_accepts || !source_accepts || !expect_stitched_geometry_after_camera_update(3864, 1138)) {
    std::cerr << "The batch demuxer must preserve P010 and stitched geometry while unbatching\n";
    return 1;
  }
  return 0;
}
