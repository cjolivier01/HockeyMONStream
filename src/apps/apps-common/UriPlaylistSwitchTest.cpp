#include "hstream/src/apps/apps-common/deepstream_sources.h"

#include <gst/gst.h>

#include <iostream>
#include <string>
#include <vector>

// DeepStream app bins use this category for logging.
GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

struct BusState {
  GMainLoop* loop{nullptr};
  int exit_code{0};
};

gboolean timeout_call(gpointer data) {
  BusState* state = static_cast<BusState*>(data);
  std::cerr << "ERROR: timed out waiting for EOS/error\n";
  state->exit_code = 1;
  g_main_loop_quit(state->loop);
  return FALSE;
}

gboolean bus_call(GstBus* /*bus*/, GstMessage* msg, gpointer data) {
  BusState* state = static_cast<BusState*>(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_main_loop_quit(state->loop);
      break;
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &err, &debug);
      std::cerr << "GStreamer ERROR: " << (err ? err->message : "(unknown)") << "\n";
      if (debug) {
        std::cerr << "Debug: " << debug << "\n";
      }
      if (err) {
        g_error_free(err);
      }
      if (debug) {
        g_free(debug);
      }
      state->exit_code = 1;
      g_main_loop_quit(state->loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

struct PtsState {
  GstClockTime last_pts{GST_CLOCK_TIME_NONE};
  bool monotonic{true};
  guint64 num_buffers{0};
};

GstPadProbeReturn pts_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
  PtsState* st = static_cast<PtsState*>(user_data);
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_OK;
  }
  GstBuffer* buf = GST_BUFFER(info->data);
  ++st->num_buffers;
  GstClockTime pts = GST_BUFFER_PTS(buf);
  if (pts == GST_CLOCK_TIME_NONE) {
    return GST_PAD_PROBE_OK;
  }
  if (st->last_pts != GST_CLOCK_TIME_NONE && pts < st->last_pts) {
    st->monotonic = false;
  }
  st->last_pts = pts;
  return GST_PAD_PROBE_OK;
}

std::string join_semicolon(const std::vector<std::string>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) {
      out.push_back(';');
    }
    out.append(items[i]);
  }
  return out;
}

int run_one_source(const std::vector<std::string>& uris) {
  NvDsSourceConfig cfg{};
  cfg.type = NV_DS_SOURCE_URI;
  cfg.enable = TRUE;
  cfg.gpu_id = 0;
  cfg.nvbuf_memory_type = 0;
  cfg.cuda_memory_type = 0;
  cfg.uri_list_loop = FALSE;
  cfg.camera_id = 0;
  cfg.source_id = 0;
  const std::string uri_list_joined = join_semicolon(uris);
  cfg.uri_list = strdup(uri_list_joined.c_str());
  cfg.uri = strdup(uris.front().c_str());

  NvDsSrcBin src_bin{};
  if (!create_source_bin(&cfg, &src_bin)) {
    std::cerr << "Failed to create source bin\n";
    return 3;
  }

  GstElement* pipeline = gst_pipeline_new("uri-playlist-switch-test");
  GstElement* sink = gst_element_factory_make("fakesink", "sink");
  if (!pipeline || !sink) {
    std::cerr << "Failed to create pipeline/sink\n";
    return 4;
  }
  g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, NULL);

  gst_bin_add_many(GST_BIN(pipeline), src_bin.bin, sink, NULL);
  if (!gst_element_link(src_bin.bin, sink)) {
    std::cerr << "Failed to link source bin to sink\n";
    return 5;
  }

  PtsState pts_state{};
  GstPad* sinkpad = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, pts_probe, &pts_state, NULL);
  gst_object_unref(sinkpad);

  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  BusState bus_state{.loop = loop, .exit_code = 0};

  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  gst_bus_add_watch(bus, bus_call, &bus_state);
  gst_object_unref(bus);

  // Hard timeout to prevent hangs.
  g_timeout_add_seconds(15, timeout_call, &bus_state);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to set pipeline to PLAYING\n";
    return 6;
  }

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref(loop);

  if (bus_state.exit_code != 0) {
    return bus_state.exit_code;
  }

  if (!pts_state.monotonic) {
    std::cerr << "ERROR: PTS was not monotonic across the playlist.\n";
    return 7;
  }
  if (pts_state.num_buffers == 0) {
    std::cerr << "ERROR: No buffers were received at the sink.\n";
    return 9;
  }

  const guint expected_switches = static_cast<guint>(uris.size() - 1);
  if (src_bin.uri_switch_count != expected_switches) {
    std::cerr << "ERROR: Expected " << expected_switches << " URI switches, got " << src_bin.uri_switch_count << "\n";
    return 8;
  }

  std::cout << "OK: switched " << src_bin.uri_switch_count << " times; PTS monotonic.\n";
  return 0;
}

int run_two_sources(const std::vector<std::string>& left_uris, const std::vector<std::string>& right_uris) {
  NvDsSourceConfig cfg0{};
  cfg0.type = NV_DS_SOURCE_URI;
  cfg0.enable = TRUE;
  cfg0.gpu_id = 0;
  cfg0.nvbuf_memory_type = 0;
  cfg0.cuda_memory_type = 0;
  cfg0.uri_list_loop = FALSE;
  cfg0.camera_id = 0;
  cfg0.source_id = 0;
  const std::string left_joined = join_semicolon(left_uris);
  cfg0.uri_list = strdup(left_joined.c_str());
  cfg0.uri = strdup(left_uris.front().c_str());

  NvDsSourceConfig cfg1{};
  cfg1.type = NV_DS_SOURCE_URI;
  cfg1.enable = TRUE;
  cfg1.gpu_id = 0;
  cfg1.nvbuf_memory_type = 0;
  cfg1.cuda_memory_type = 0;
  cfg1.uri_list_loop = FALSE;
  cfg1.camera_id = 1;
  cfg1.source_id = 1;
  const std::string right_joined = join_semicolon(right_uris);
  cfg1.uri_list = strdup(right_joined.c_str());
  cfg1.uri = strdup(right_uris.front().c_str());

  NvDsSrcBin src0{};
  NvDsSrcBin src1{};
  if (!create_source_bin(&cfg0, &src0) || !create_source_bin(&cfg1, &src1)) {
    std::cerr << "Failed to create source bins\n";
    return 3;
  }

  GstElement* pipeline = gst_pipeline_new("uri-playlist-switch-dual-test");
  GstElement* sink0 = gst_element_factory_make("fakesink", "sink0");
  GstElement* sink1 = gst_element_factory_make("fakesink", "sink1");
  if (!pipeline || !sink0 || !sink1) {
    std::cerr << "Failed to create pipeline/sinks\n";
    return 4;
  }
  g_object_set(G_OBJECT(sink0), "sync", FALSE, "async", FALSE, NULL);
  g_object_set(G_OBJECT(sink1), "sync", FALSE, "async", FALSE, NULL);

  gst_bin_add_many(GST_BIN(pipeline), src0.bin, sink0, src1.bin, sink1, NULL);
  if (!gst_element_link(src0.bin, sink0) || !gst_element_link(src1.bin, sink1)) {
    std::cerr << "Failed to link source bins to sinks\n";
    return 5;
  }

  PtsState pts0{};
  PtsState pts1{};
  GstPad* sinkpad0 = gst_element_get_static_pad(sink0, "sink");
  GstPad* sinkpad1 = gst_element_get_static_pad(sink1, "sink");
  gst_pad_add_probe(sinkpad0, GST_PAD_PROBE_TYPE_BUFFER, pts_probe, &pts0, NULL);
  gst_pad_add_probe(sinkpad1, GST_PAD_PROBE_TYPE_BUFFER, pts_probe, &pts1, NULL);
  gst_object_unref(sinkpad0);
  gst_object_unref(sinkpad1);

  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  BusState bus_state{.loop = loop, .exit_code = 0};
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  gst_bus_add_watch(bus, bus_call, &bus_state);
  gst_object_unref(bus);

  g_timeout_add_seconds(20, timeout_call, &bus_state);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to set pipeline to PLAYING\n";
    return 6;
  }

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref(loop);

  if (bus_state.exit_code != 0) {
    return bus_state.exit_code;
  }

  if (!pts0.monotonic || !pts1.monotonic) {
    std::cerr << "ERROR: PTS was not monotonic (left=" << pts0.monotonic << ", right=" << pts1.monotonic << ")\n";
    return 7;
  }
  if (pts0.num_buffers == 0 || pts1.num_buffers == 0) {
    std::cerr << "ERROR: No buffers received (left=" << pts0.num_buffers << ", right=" << pts1.num_buffers << ")\n";
    return 9;
  }

  const guint expected0 = left_uris.size() ? static_cast<guint>(left_uris.size() - 1) : 0;
  const guint expected1 = right_uris.size() ? static_cast<guint>(right_uris.size() - 1) : 0;
  if (src0.uri_switch_count != expected0 || src1.uri_switch_count != expected1) {
    std::cerr << "ERROR: Expected switches (left=" << expected0 << ", right=" << expected1 << ") got (left="
              << src0.uri_switch_count << ", right=" << src1.uri_switch_count << ")\n";
    return 8;
  }

  std::cout << "OK: dual sources switched (left=" << src0.uri_switch_count << ", right=" << src1.uri_switch_count
            << "); PTS monotonic.\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  // gst_init() will consume/remove some argv entries (notably `--`), so capture a copy first.
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  int sep = -1;
  for (int i = 0; i < static_cast<int>(args.size()); ++i) {
    if (args[i] == "--") {
      sep = i;
      break;
    }
  }

  if (sep == -1) {
    if (args.size() < 2) {
      std::cerr << "Usage: " << argv[0] << " <uri1> <uri2> [uri3 ...]\n";
      std::cerr << "   or: " << argv[0] << " <l1> <l2> [l3 ...] -- <r1> <r2> [r3 ...]\n";
      std::cerr << "Example: " << argv[0] << " file:///path/a.mp4 file:///path/b.mp4\n";
      return 2;
    }
    return run_one_source(args);
  }

  std::vector<std::string> left;
  std::vector<std::string> right;
  for (int i = 0; i < sep; ++i) {
    left.emplace_back(args[i]);
  }
  for (int i = sep + 1; i < static_cast<int>(args.size()); ++i) {
    right.emplace_back(args[i]);
  }
  if (left.empty() || right.empty() || (left.size() < 2 && right.size() < 2)) {
    std::cerr << "Usage: " << argv[0] << " <l1> <l2> [l3 ...] -- <r1> <r2> [r3 ...]\n";
    return 2;
  }
  return run_two_sources(left, right);
}
