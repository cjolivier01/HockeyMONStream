#include <gst/gst.h>

int main(int argc, char* argv[]) {
  gst_init(&argc, &argv);

  // Construct a pipeline with nvmultisrcbin.
  // Assume that nvmultisrcbin is given a name ("multi") and that its internal source for video 2
  // is named "src1" (video 1 is "src0"). (You may need to set these names explicitly when constructing
  // the bin or inspect its children.)
  const char* pipeline_desc =
      "nvmultisrcbin "
      "src0-location=/mnt/data/Videos/mlk-heat-1/GX010016.MP4 "
      "src1-location=/mnt/data/Videos/mlk-heat-1/GX010097.MP4 "
      "! queue ! decodebin ! videoconvert ! autovideosink";

  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(pipeline_desc, &error);
  if (!pipeline) {
    g_printerr("Failed to create pipeline: %s\n", error->message);
    g_clear_error(&error);
    return -1;
  }

  // Set the pipeline to PAUSED so that we can perform seeking.
  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to PAUSED state.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Wait until the pipeline is actually PAUSED
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_timed_pop_filtered(
      bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType>(GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR));

  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError* err;
      gchar* dbg;
      gst_message_parse_error(msg, &err, &dbg);
      g_printerr("Error: %s\n", err->message);
      g_error_free(err);
      g_free(dbg);
      gst_message_unref(msg);
      gst_object_unref(bus);
      gst_object_unref(pipeline);
      return -1;
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);

  // Get the nvmultisrcbin element by name ("multi")
  GstElement* multiSrc = gst_bin_get_by_name(GST_BIN(pipeline), "multi");
  if (!multiSrc) {
    g_printerr("Could not find nvmultisrcbin element 'multi'\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Now get the second source element within the bin.
  // This assumes that the internal element is named "src1".  (This naming may vary based on your
  // pipeline setup; you might need to query or set the name when building the pipeline.)
  GstElement* src2 = gst_bin_get_by_name(GST_BIN(multiSrc), "src1");
  if (!src2) {
    g_printerr("Could not find the second source element 'src1' inside nvmultisrcbin\n");
    gst_object_unref(multiSrc);
    gst_object_unref(pipeline);
    return -1;
  }

  // Calculate the seek position for frame 200 at 30 fps.
  // (200 frames) * (1/30 seconds per frame) = ~6.667 seconds.
  gint64 seek_time = (200 * GST_SECOND) / 30;

  // Send a seek event to the second source.
  // Using gst_element_seek_simple() with GST_FORMAT_TIME.
  gboolean success = gst_element_seek_simple(
      src2, GST_FORMAT_TIME, (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_KEY_UNIT), seek_time);

  if (!success) {
    g_printerr("Seek on video2 failed.\n");
  } else {
    g_print("Seeked video2 to frame 200 (%.2f seconds).\n", (gdouble)seek_time / GST_SECOND);
  }

  // Cleanup our references to internal elements.
  gst_object_unref(src2);
  gst_object_unref(multiSrc);

  // Now set the pipeline to PLAYING.
  ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to PLAYING state.\n");
    gst_object_unref(pipeline);
    return -1;
  }

  // Run a GLib Main Loop until an error or EOS.
  GMainLoop* main_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(main_loop);

  // Clean up
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_main_loop_unref(main_loop);

  return 0;
}
