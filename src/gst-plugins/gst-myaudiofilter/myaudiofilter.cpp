/* GStreamer
 * Copyright (C) 2024 FIXME <fixme@example.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstaudiofilter
 *
 * The myaudiofilter element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v fakesrc ! myaudiofilter ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "myaudiofilter.h"

#include <gst/audio/gstaudiofilter.h>
#include <gst/gst.h>

#include <cassert>

namespace {

GST_DEBUG_CATEGORY_STATIC(gst_gst_my_audio_filter_debug_category);
#define GST_CAT_DEFAULT gst_gst_my_audio_filter_debug_category

/* prototypes */

static void gst_gst_my_audio_filter_set_property(GObject *object,
                                                 guint property_id,
                                                 const GValue *value,
                                                 GParamSpec *pspec);
static void gst_gst_my_audio_filter_get_property(GObject *object,
                                                 guint property_id,
                                                 GValue *value,
                                                 GParamSpec *pspec);
static void gst_gst_my_audio_filter_dispose(GObject *object);
static void gst_gst_my_audio_filter_finalize(GObject *object);

static gboolean gst_gst_my_audio_filter_setup(GstAudioFilter *filter,
                                              const GstAudioInfo *info);
static GstFlowReturn gst_gst_my_audio_filter_transform(GstBaseTransform *trans,
                                                       GstBuffer *inbuf,
                                                       GstBuffer *outbuf);
static GstFlowReturn
gst_gst_my_audio_filter_transform_ip(GstBaseTransform *trans, GstBuffer *buf);
static void gst_gst_my_audio_filter_init(GstGstMyAudioFilter *myaudiofilter);

// Chain
static GstFlowReturn gst_my_filter_chain(GstPad *pad, GstObject *parent,
                                         GstBuffer *buf);
// Events
static gboolean gst_my_filter_sink_event(GstPad *pad, GstObject *parent,
                                         GstEvent *event);

// Queries
static gboolean gst_my_filter_src_query(GstPad *pad, GstObject *parent,
                                        GstQuery *query);
// State change
static void gst_my_filter_change_state(GstElement *element, GstState oldstate,
                                       GstState newstate, GstState pending);

enum { PROP_0 };

/* pad templates */

/* FIXME add/remove the formats that you want to support */
static GstStaticPadTemplate gst_gst_my_audio_filter_src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS,
        GST_STATIC_CAPS("audio/x-raw,format=S16LE,rate=[1,max],"
                        "channels=[1,max],layout=interleaved"));

/* FIXME add/remove the formats that you want to support */
static GstStaticPadTemplate gst_gst_my_audio_filter_sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        GST_STATIC_CAPS("audio/x-raw,format=S16LE,rate=[1,max],"
                        "channels=[1,max],layout=interleaved"));

/* class initialization */

G_DEFINE_TYPE_WITH_CODE(
    GstGstMyAudioFilter, gst_gst_my_audio_filter, GST_TYPE_AUDIO_FILTER,
    GST_DEBUG_CATEGORY_INIT(gst_gst_my_audio_filter_debug_category,
                            "myaudiofilter", 0,
                            "debug category for myaudiofilter element"));

/**
 *   _____ _                   _____       _  _
 *  / ____| |                 |_   _|     (_)| |
 * | |    | | __ _  ___  ___    | |  _ __  _ | |_
 * | |    | |/ _` |/ __|/ __|   | | | '_ \| || __|
 * | |____| | (_| |\__ \\__ \  _| |_| | | | || |_
 *  \_____|_|\__,_||___/|___/ |_____|_| |_|_| \__|
 *
 *
 */
static void
gst_gst_my_audio_filter_class_init(GstGstMyAudioFilterClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
  GstAudioFilterClass *audio_filter_class = GST_AUDIO_FILTER_CLASS(klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template(
      GST_ELEMENT_CLASS(klass), &gst_gst_my_audio_filter_src_template);
  gst_element_class_add_static_pad_template(
      GST_ELEMENT_CLASS(klass), &gst_gst_my_audio_filter_sink_template);

  gst_element_class_set_static_metadata(
      GST_ELEMENT_CLASS(klass), "This is my audio filter", "Generic",
      "FIXME Description", "FIXME <fixme@example.com>");

  gobject_class->set_property = gst_gst_my_audio_filter_set_property;
  gobject_class->get_property = gst_gst_my_audio_filter_get_property;
  gobject_class->dispose = gst_gst_my_audio_filter_dispose;
  gobject_class->finalize = gst_gst_my_audio_filter_finalize;
  audio_filter_class->setup = GST_DEBUG_FUNCPTR(gst_gst_my_audio_filter_setup);
  base_transform_class->transform =
      GST_DEBUG_FUNCPTR(gst_gst_my_audio_filter_transform);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR(gst_gst_my_audio_filter_transform_ip);

  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  element_class->state_changed = gst_my_filter_change_state;
}

/**
 *  ______  _  _ _                _____       _  _
 * |  ____|(_)| | |              |_   _|     (_)| |
 * | |__    _ | | |_  ___  _ __    | |  _ __  _ | |_
 * |  __|  | || | __|/ _ \| '__|   | | | '_ \| || __|
 * | |     | || | |_|  __/| |     _| |_| | | | || |_
 * |_|     |_||_|\__|\___||_|    |_____|_| |_|_| \__|
 *
 *
 */
static void gst_gst_my_audio_filter_init(GstGstMyAudioFilter *myaudiofilter) {
  // Init the pads...
  /* pad through which data comes in to the element */
  myaudiofilter->sinkpad = gst_pad_new_from_static_template(
      &gst_gst_my_audio_filter_src_template, "sink");
  /* pads are configured here with gst_pad_set_*_function () */

  gst_element_add_pad(GST_ELEMENT(myaudiofilter), myaudiofilter->sinkpad);

  /* pad through which data goes out of the element */
  myaudiofilter->srcpad = gst_pad_new_from_static_template(
      &gst_gst_my_audio_filter_sink_template, "src");
  /* pads are configured here with gst_pad_set_*_function () */

  gst_element_add_pad(GST_ELEMENT(myaudiofilter), myaudiofilter->srcpad);

  /* properties initial value */
  myaudiofilter->silent = FALSE;

  /* configure chain function on the pad before adding
   * the pad to the element */
  gst_pad_set_chain_function(myaudiofilter->sinkpad, gst_my_filter_chain);

  gst_pad_set_event_function(myaudiofilter->sinkpad, gst_my_filter_sink_event);

  /* configure event function on the pad before adding
   * the pad to the element */
  gst_pad_set_query_function(myaudiofilter->srcpad, gst_my_filter_src_query);
}

void gst_gst_my_audio_filter_set_property(GObject *object, guint property_id,
                                          const GValue *value,
                                          GParamSpec *pspec) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(object);

  GST_DEBUG_OBJECT(myaudiofilter, "set_property");

  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

void gst_gst_my_audio_filter_get_property(GObject *object, guint property_id,
                                          GValue *value, GParamSpec *pspec) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(object);

  GST_DEBUG_OBJECT(myaudiofilter, "get_property");

  switch (property_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

void gst_gst_my_audio_filter_dispose(GObject *object) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(object);

  GST_DEBUG_OBJECT(myaudiofilter, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS(gst_gst_my_audio_filter_parent_class)->dispose(object);
}

void gst_gst_my_audio_filter_finalize(GObject *object) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(object);

  GST_DEBUG_OBJECT(myaudiofilter, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS(gst_gst_my_audio_filter_parent_class)->finalize(object);
}

static gboolean gst_gst_my_audio_filter_setup(GstAudioFilter *filter,
                                              const GstAudioInfo *info) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(filter);

  GST_DEBUG_OBJECT(myaudiofilter, "setup");

  return TRUE;
}

/* transform */
static GstFlowReturn gst_gst_my_audio_filter_transform(GstBaseTransform *trans,
                                                       GstBuffer *inbuf,
                                                       GstBuffer *outbuf) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(trans);

  GST_DEBUG_OBJECT(myaudiofilter, "transform");

  return GST_FLOW_OK;
}

static GstFlowReturn gst_my_filter_chain(GstPad *pad, GstObject *parent,
                                         GstBuffer *buf) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(parent);

  if (!myaudiofilter->silent) {
    g_print("Have data of size %" G_GSIZE_FORMAT " bytes!\n",
            gst_buffer_get_size(buf));
  }

  return gst_pad_push(myaudiofilter->srcpad, buf);
}

static gboolean
gst_my_filter_stop_processing(GstGstMyAudioFilter *myaudiofilter) {
  if (!myaudiofilter->silent) {
    g_print("gst_my_filter_stop_processing()");
  }
  return true;
}

/**
 *   _____  _       _      ______                  _
 *  / ____|(_)     | |    |  ____|                | |
 * | (___   _ _ __ | | __ | |__ __   __ ___  _ __ | |_
 *  \___ \ | | '_ \| |/ / |  __|\ \ / // _ \| '_ \| __|
 *  ____) || | | | |   <  | |____\ V /|  __/| | | | |_
 * |_____/ |_|_| |_|_|\_\ |______|\_/  \___||_| |_|\__|
 *
 *
 */
static gboolean gst_my_filter_sink_event(GstPad *pad, GstObject *parent,
                                         GstEvent *event) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(parent);
  gboolean ret = false;
  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_CAPS:
    /* we should handle the format here */
    /* we should handle the format here */

    /* push the event downstream */
    ret = gst_pad_push_event(myaudiofilter->srcpad, event);
    break;
  case GST_EVENT_EOS:
    /* end-of-stream, we should close down all stream leftovers here */
    gst_my_filter_stop_processing(myaudiofilter);

    ret = gst_pad_event_default(pad, parent, event);
    break;
  default:
    /* just call the default handler */
    ret = gst_pad_event_default(pad, parent, event);
    break;
  }

  return ret;
}

/**
 *   ____
 *  / __ \
 * | |  | |_   _  ___  _ __ _   _
 * | |  | | | | |/ _ \| '__| | | |
 * | |__| | |_| |  __/| |  | |_| |
 *  \___\_\\__,_|\___||_|   \__, |
 *                           __/ |
 *                          |___/
 */
static gboolean gst_my_filter_src_query(GstPad *pad, GstObject *parent,
                                        GstQuery *query) {
  gboolean ret{false};
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(parent);
  (void)myaudiofilter;

  switch (GST_QUERY_TYPE(query)) {
  case GST_QUERY_POSITION:
    /* we should report the current position */
    g_print("Got query: GST_QUERY_POSITION\n");
    ret = gst_pad_query_default(pad, parent, query);
    break;
  case GST_QUERY_DURATION:
    /* we should report the duration here */
    g_print("Got query: GST_QUERY_DURATION\n");
    ret = gst_pad_query_default(pad, parent, query);
    break;
  case GST_QUERY_CAPS:
    /* we should report the supported caps here */
    g_print("Got query: GST_QUERY_CAPS\n");
    ret = gst_pad_query_default(pad, parent, query);
    break;
  default:
    /* just call the default handler */
    ret = gst_pad_query_default(pad, parent, query);
    break;
  }
  return ret;
}

/**
 *   _____ _         _
 *  / ____| |       | |
 * | (___ | |_  __ _| |_  ___
 *  \___ \| __|/ _` | __|/ _ \
 *  ____) | |_| (_| | |_|  __/
 * |_____/ \__|\__,_|\__|\___|
 *
 *
 */
static void gst_my_filter_change_state(GstElement *element, GstState oldstate,
                                       GstState newstate, GstState pending) {
  // GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  //  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(element);

  // switch (transition) {
  // case GST_STATE_CHANGE_NULL_TO_READY:
  //   // if (!gst_my_filter_allocate_memory(filter))
  //   //   return GST_STATE_CHANGE_FAILURE;
  //   break;
  // default:
  //   break;
  // }

  // ret = element->change_state(element, transition);
  // if (ret == GST_STATE_CHANGE_FAILURE)
  //   return ret;

  // switch (transition) {
  // case GST_STATE_CHANGE_READY_TO_NULL:
  //   gst_my_filter_free_memory(filter);
  //   break;
  // default:
  //   break;
  // }
}

static GstFlowReturn
gst_gst_my_audio_filter_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
  GstGstMyAudioFilter *myaudiofilter = GST_GST_AUDIO_FILTER(trans);

  GST_DEBUG_OBJECT(myaudiofilter, "transform_ip");

  return GST_FLOW_OK;
}

static gboolean plugin_init(GstPlugin *plugin) {

  /* FIXME Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register(plugin, "myaudiofilter", GST_RANK_NONE,
                              GST_TYPE_GST_AUDIO_FILTER);
}

} // namespace

/* FIXME: these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "FIXME_package"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "FIXME_package_name"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://FIXME.org/"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, nvdsgst_myaudiofilter,
                  "My audio filter plugin", plugin_init, VERSION, "LGPL",
                  PACKAGE_NAME, GST_PACKAGE_ORIGIN)
