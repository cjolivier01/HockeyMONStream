#include <glib.h>
#include <gst/gst.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

struct GstElementDeleter {
  void operator()(GstElement* element) {
    if (element)
      gst_object_unref(GST_OBJECT(element));
  }
};

using GstElementPtr = std::unique_ptr<GstElement, GstElementDeleter>;

class Pipeline {
 private:
  GstElement* pipeline_;
  GMainLoop* loop_;
  std::vector<GstElementPtr> elements_;

  void check_element(GstElement* element, const char* name) {
    if (!element) {
      throw std::runtime_error(std::string("Failed to create element: ") + name);
    }
  }

  void add_element(GstElement* element) {
    elements_.emplace_back(GstElementPtr(element));
  }

 public:
  Pipeline(const char* filepath) {
    if (!filepath) {
      throw std::invalid_argument("Filepath cannot be null");
    }

    gst_init(nullptr, nullptr);

    GstElement* pipeline = gst_pipeline_new("mp4-av-pipeline");
    GstElement* source = gst_element_factory_make("filesrc", "file-source");
    GstElement* demuxer = gst_element_factory_make("qtdemux", "demuxer");

    GstElement* videoQueue = gst_element_factory_make("queue", "video-queue");
    GstElement* videoDecoder = gst_element_factory_make("nvv4l2decoder", "video-decoder");
    GstElement* streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    GstElement* videoConvert = gst_element_factory_make("nvvideoconvert", "converter");
    GstElement* videoSink = gst_element_factory_make("nveglglessink", "video-renderer");

    GstElement* audioQueue = gst_element_factory_make("queue", "audio-queue");
    GstElement* audioDecoder = gst_element_factory_make("avdec_aac", "audio-decoder");
    GstElement* audioConvert = gst_element_factory_make("audioconvert", "aconverter");
    GstElement* audioResample = gst_element_factory_make("audioresample", "resampler");
    GstElement* audioSink = gst_element_factory_make("alsasink", "audio-output");

    check_element(pipeline, "pipeline");
    check_element(source, "source");
    check_element(demuxer, "demuxer");
    check_element(videoQueue, "videoQueue");
    check_element(videoDecoder, "videoDecoder");
    check_element(streammux, "streammux");
    check_element(videoConvert, "videoConvert");
    check_element(videoSink, "videoSink");
    check_element(audioQueue, "audioQueue");
    check_element(audioDecoder, "audioDecoder");
    check_element(audioConvert, "audioConvert");
    check_element(audioResample, "audioResample");
    check_element(audioSink, "audioSink");

    g_object_set(
        G_OBJECT(streammux),
        "width",
        1920,
        "height",
        1080,
        "batch-size",
        1,
        "batched-push-timeout",
        40000,
        "live-source",
        0,
        NULL);

    g_object_set(G_OBJECT(videoSink), "sync", TRUE, NULL);
    g_object_set(G_OBJECT(audioSink), "sync", TRUE, "provide-clock", TRUE, NULL);

    g_object_set(G_OBJECT(videoQueue), "max-size-buffers", 4, "max-size-time", 0, "max-size-bytes", 0, NULL);
    g_object_set(G_OBJECT(audioQueue), "max-size-buffers", 4, "max-size-time", 0, "max-size-bytes", 0, NULL);

    g_object_set(G_OBJECT(source), "location", filepath, NULL);

    pipeline_ = pipeline;

    add_element(source);
    add_element(demuxer);
    add_element(videoQueue);
    add_element(videoDecoder);
    add_element(streammux);
    add_element(videoConvert);
    add_element(videoSink);
    add_element(audioQueue);
    add_element(audioDecoder);
    add_element(audioConvert);
    add_element(audioResample);
    add_element(audioSink);

    gst_bin_add_many(
        GST_BIN(pipeline_),
        source,
        demuxer,
        videoQueue,
        videoDecoder,
        streammux,
        videoConvert,
        videoSink,
        audioQueue,
        audioDecoder,
        audioConvert,
        audioResample,
        audioSink,
        NULL);

    if (!gst_element_link(source, demuxer)) {
      throw std::runtime_error("Failed to link source -> demuxer");
    }
    if (!gst_element_link_many(videoQueue, videoDecoder, NULL)) {
      throw std::runtime_error("Failed to link video queue -> decoder");
    }
    if (!gst_element_link_many(streammux, videoConvert, videoSink, NULL)) {
      throw std::runtime_error("Failed to link streammux -> convert -> sink");
    }
    if (!gst_element_link_many(audioQueue, audioDecoder, audioConvert, audioResample, audioSink, NULL)) {
      throw std::runtime_error("Failed to link audio elements");
    }

    GstPad* sinkpad = gst_element_get_request_pad(streammux, "sink_0");
    if (!sinkpad) {
      throw std::runtime_error("Failed to get sink pad from streammux");
    }
    gst_object_unref(sinkpad);

    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), NULL);

    loop_ = g_main_loop_new(NULL, FALSE);
    if (!loop_) {
      throw std::runtime_error("Failed to create main loop");
    }
  }

  void run() {
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      throw std::runtime_error("Failed to set pipeline to playing state");
    }
    g_main_loop_run(loop_);
  }

  ~Pipeline() {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(GST_OBJECT(pipeline_));
    }
    if (loop_) {
      g_main_loop_unref(loop_);
    }
  }

 private:
  static void on_pad_added(GstElement* element, GstPad* pad, gpointer data) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
      std::cerr << "Failed to get pad caps" << std::endl;
      return;
    }

    GstStructure* str = gst_caps_get_structure(caps, 0);
    const gchar* name = gst_structure_get_name(str);

    GstElement* queue = NULL;
    GstElement* streammux = NULL;
    GstPad* sinkpad = NULL;

    try {
      if (g_str_has_prefix(name, "video/")) {
        queue = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "video-queue");
        if (!queue)
          throw std::runtime_error("Failed to get video queue");

        streammux = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "stream-muxer");
        if (!streammux)
          throw std::runtime_error("Failed to get streammux");

        GstElement* decoder = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "video-decoder");
        if (!decoder)
          throw std::runtime_error("Failed to get video decoder");

        GstPad* srcpad = gst_element_get_static_pad(decoder, "src");
        if (!srcpad)
          throw std::runtime_error("Failed to get decoder src pad");

        GstPad* muxsinkpad = gst_element_get_static_pad(streammux, "sink_0");
        if (!muxsinkpad)
          throw std::runtime_error("Failed to get mux sink pad");

        if (gst_pad_link(srcpad, muxsinkpad) != GST_PAD_LINK_OK) {
          throw std::runtime_error("Failed to link decoder to mux");
        }

        gst_object_unref(srcpad);
        gst_object_unref(muxsinkpad);
        gst_object_unref(decoder);

      } else if (g_str_has_prefix(name, "audio/")) {
        queue = gst_bin_get_by_name(GST_BIN(gst_element_get_parent(element)), "audio-queue");
        if (!queue)
          throw std::runtime_error("Failed to get audio queue");
      }

      if (queue) {
        sinkpad = gst_element_get_static_pad(queue, "sink");
        if (!sinkpad)
          throw std::runtime_error("Failed to get queue sink pad");

        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
          throw std::runtime_error("Failed to link demuxer to queue");
        }
      }

    } catch (const std::exception& e) {
      std::cerr << "Error in pad_added callback: " << e.what() << std::endl;
    }

    if (sinkpad)
      gst_object_unref(sinkpad);
    if (queue)
      gst_object_unref(queue);
    if (streammux)
      gst_object_unref(streammux);
    if (caps)
      gst_caps_unref(caps);
  }
};

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <mp4-file-path>\n";
    return 1;
  }

  try {
    Pipeline pipeline(argv[1]);
    pipeline.run();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
