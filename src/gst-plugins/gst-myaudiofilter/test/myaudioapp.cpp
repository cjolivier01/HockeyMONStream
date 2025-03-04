#include <gst/gst.h>

#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "gst-nvdssr.h"
#include "gst-nvevent.h"
#include "nvdsgstutils.h"

#include <iostream>

static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data) {
  GMainLoop* loop = (GMainLoop*)data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_print("End-of-stream\n");
      g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_ERROR: {
      gchar* debug = NULL;
      GError* err = NULL;

      gst_message_parse_error(msg, &err, &debug);

      g_print("Error: %s\n", err->message);
      g_error_free(err);

      if (debug) {
        g_print("Debug details: %s\n", debug);
        g_free(debug);
      }

      g_main_loop_quit(loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

void print_pads(GstElement* element) {
  GstIterator* iter;
  GstPad* pad;
  GValue item = G_VALUE_INIT;
  GEnumValue* pad_direction;

  if (!element) {
    return;
  }

  g_print("Pads for element: %s\n", GST_ELEMENT_NAME(element));

  iter = gst_element_iterate_pads(element);
  while (gst_iterator_next(iter, &item) == GST_ITERATOR_OK) {
    pad = GST_PAD(g_value_get_object(&item));
    pad_direction =
        g_enum_get_value((GEnumClass*)g_type_class_peek(GST_TYPE_PAD_DIRECTION), gst_pad_get_direction(pad));

    g_print("  Pad: %s (%s)\n", GST_PAD_NAME(pad), pad_direction->value_nick);
    g_value_unset(&item);
  }
  gst_iterator_free(iter);
}

static void on_decode_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  GstElement* convert = (GstElement*)data;
  GstPad* sinkpad = gst_element_get_static_pad(convert, "sink");
  GstPadLinkReturn ret;

  ret = gst_pad_link(pad, sinkpad);
  if (GST_PAD_LINK_FAILED(ret)) {
    g_printerr("Decoder pad link failed: %d\n", ret);
  }
  gst_object_unref(sinkpad);
}

static void on_demuxer_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  GstElement* decoder = (GstElement*)data;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);

  if (g_str_has_prefix(gst_structure_get_name(str), "audio/")) {
    GstPad* sinkpad = gst_element_get_static_pad(decoder, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link demuxer to decoder\n");
    }
    gst_object_unref(sinkpad);
  }

  gst_caps_unref(caps);
}

gint main(gint argc, gchar* argv[]) {
  GstStateChangeReturn ret;
  GstElement *pipeline, *filesrc, *demuxer{nullptr}, *decoder{nullptr}, *filter, *sink;
  GstElement *convert1, *convert2, *resample;
  GMainLoop* loop;
  GstBus* bus;
  guint watch_id;

  /* initialization */
  gst_init(&argc, &argv);
  loop = g_main_loop_new(/*context=*/NULL, /*is_running=*/FALSE);
  if (argc != 2) {
    g_print("Usage: %s <mp3 filename>\n", argv[0]);
    return 01;
  }

  /* create elements */
  pipeline = gst_pipeline_new("my_pipeline");

  /* watch for messages on the pipeline's bus (note that this will only
   * work like this when a GLib main loop is running) */
  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  watch_id = gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  const bool is_uri = g_strrstr(argv[1], "file:/") != 0;

  if (is_uri) {
    filesrc = gst_element_factory_make("uridecodebin", "my_urisource");
    g_object_set(filesrc, "uri", argv[1], NULL);
  } else {
    filesrc = gst_element_factory_make("filesrc", "my_filesource");
    g_object_set(G_OBJECT(filesrc), "location", argv[1], NULL);
    demuxer = gst_element_factory_make("qtdemux", "my_demuxer");
    decoder = gst_element_factory_make("decodebin", "decodebin");
    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_demuxer_pad_added), decoder);
  }

  /* putting an audioconvert element here to convert the output of the
   * decoder into a format that myaudiofilter can handle (we are assuming it
   * will handle any sample rate here though) */
  convert1 = gst_element_factory_make("audioconvert", "audioconvert1");

  /* use "identity" here for a filter that does nothing */
  // filter = gst_element_factory_make("myaudiofilter", "myaudiofilter");
  filter = gst_element_factory_make("audioconvert", "audioconvert_tst");

  /* there should always be audioconvert and audioresample elements before
   * the audio sink, since the capabilities of the audio sink usually vary
   * depending on the environment (output used, sound card, driver etc.) */
  convert2 = gst_element_factory_make("audioconvert", "audioconvert2");
  resample = gst_element_factory_make("audioresample", "audioresample");
  sink = gst_element_factory_make("pulsesink", "audiosink");

  if (!sink || (!is_uri && !decoder)) {
    g_print("Decoder or output could not be found - check your install\n");
    return -1;
  } else if (!convert1 || !convert2 || !resample) {
    g_print(
        "Could not create audioconvert or audioresample element, "
        "check your installation\n");
    return -1;
  } else if (!filter) {
    g_print(
        "Your self-written filter could not be found. Make sure it "
        "is installed correctly in $(libdir)/gstreamer-1.0/ or "
        "~/.gstreamer-1.0/plugins/ and that gst-inspect-1.0 lists it. "
        "If it doesn't, check with 'GST_DEBUG=*:2 gst-inspect-1.0' for "
        "the reason why it is not being loaded.");
    return -1;
  }

  g_signal_connect(decoder, "pad-added", G_CALLBACK(on_decode_pad_added), convert1);

  if (!demuxer && !decoder) {
    gst_bin_add_many(GST_BIN(pipeline), filesrc, convert1, filter, convert2, resample, sink, NULL);
  } else {
    gst_bin_add_many(GST_BIN(pipeline), filesrc, demuxer, decoder, convert1, filter, convert2, resample, sink, NULL);
  }

  print_pads(filesrc);
  print_pads(demuxer);
  print_pads(decoder);
  print_pads(convert1);

  /* link everything together */
  if (demuxer) {
    if (!gst_element_link_many(filesrc, demuxer, NULL)) {
      g_print("Failed to link one or more elements!\n");
      return -1;
    }
  } else {
    g_signal_connect(filesrc, "pad-added", G_CALLBACK(on_demuxer_pad_added), convert1);
  }

  if (!gst_element_link_many(convert1, filter, convert2, resample, sink, NULL)) {
    g_print("Failed to link one or more elements!\n");
    return -1;
  }

  /* run */
  ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    GstMessage* msg;

    g_print("Failed to start up pipeline!\n");

    /* check if there is an error message with details on the bus */
    msg = gst_bus_poll(bus, GST_MESSAGE_ERROR, 0);
    if (msg) {
      GError* err = NULL;

      gst_message_parse_error(msg, &err, NULL);
      g_print("ERROR: %s\n", err->message);
      g_error_free(err);
      gst_message_unref(msg);
    }
    return -1;
  }

  g_main_loop_run(loop);

  /* clean up */
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_source_remove(watch_id);
  g_main_loop_unref(loop);

  return 0;
}
