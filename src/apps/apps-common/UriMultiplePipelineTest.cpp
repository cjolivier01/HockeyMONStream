#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"

#include <gst/gst.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace fs = std::filesystem;

namespace {

struct BusState {
  GMainLoop* loop{nullptr};
  int exit_code{0};
  guint timeout_id{0};
};

gboolean timeout_call(gpointer data) {
  BusState* state = static_cast<BusState*>(data);
  std::cerr << "ERROR: timed out waiting for EOS/error\n";
  state->exit_code = 1;
  state->timeout_id = 0;
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

struct BufferCounter {
  guint64 buffers{0};
};

GstPadProbeReturn count_buffers_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_OK;
  }
  auto* counter = static_cast<BufferCounter*>(user_data);
  ++counter->buffers;
  return GST_PAD_PROBE_OK;
}

std::string shell_quote(const fs::path& path) {
  std::string in = path.string();
  std::string out = "'";
  for (char c : in) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

bool run_shell(const std::string& cmd) {
  int status = std::system(cmd.c_str());
  return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool make_synthetic_mp4(const fs::path& path, int seconds, int hue_degrees, int audio_frequency) {
  std::stringstream cmd;
  cmd << "ffmpeg -hide_banner -loglevel error -y "
      << "-f lavfi -i "
      << "'testsrc2=size=256x144:rate=15,format=yuv420p,hue=h=" << hue_degrees << "' ";
  if (audio_frequency > 0) {
    cmd << "-f lavfi -i 'sine=frequency=" << audio_frequency << ":sample_rate=48000' ";
  }
  cmd << "-t " << seconds << " ";
  if (audio_frequency > 0) {
    cmd << "-shortest -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 64k ";
  } else {
    cmd << "-an -c:v libx264 -preset ultrafast -pix_fmt yuv420p ";
  }
  cmd << "-movflags +faststart " << shell_quote(path);
  return run_shell(cmd.str());
}

std::string to_file_uri(const fs::path& path) {
  return "file://" + fs::absolute(path).string();
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

bool add_encoder_chain(GstElement* pipeline, const fs::path& output_path, std::vector<GstElement*>& chain) {
  GstElement* conv = gst_element_factory_make("nvvideoconvert", nullptr);
  GstElement* capsfilter = gst_element_factory_make("capsfilter", nullptr);
  GstElement* enc = gst_element_factory_make("nvv4l2h264enc", nullptr);
  GstElement* parse = gst_element_factory_make("h264parse", nullptr);
  GstElement* mux = gst_element_factory_make("matroskamux", nullptr);
  GstElement* sink = gst_element_factory_make("filesink", nullptr);
  if (!conv || !capsfilter || !enc || !parse || !mux || !sink) {
    std::cerr << "Failed to create encode chain elements\n";
    return false;
  }

  GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=NV12");
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);
  g_object_set(G_OBJECT(enc), "bitrate", 2000000, NULL);
  g_object_set(G_OBJECT(sink), "location", output_path.c_str(), "sync", FALSE, "async", FALSE, NULL);

  gst_bin_add_many(GST_BIN(pipeline), conv, capsfilter, enc, parse, mux, sink, NULL);
  if (!gst_element_link_many(conv, capsfilter, enc, parse, mux, sink, NULL)) {
    std::cerr << "Failed to link encode chain\n";
    return false;
  }
  chain = {conv, capsfilter, enc, parse, mux, sink};
  return true;
}

int run_pipeline(GstElement* pipeline, int timeout_seconds) {
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  BusState bus_state{.loop = loop, .exit_code = 0};

  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  const guint bus_watch_id = gst_bus_add_watch(bus, bus_call, &bus_state);
  gst_object_unref(bus);
  bus_state.timeout_id = g_timeout_add_seconds(timeout_seconds, timeout_call, &bus_state);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to set pipeline to PLAYING\n";
    g_main_loop_unref(loop);
    return 2;
  }

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (bus_state.timeout_id) {
    g_source_remove(bus_state.timeout_id);
  }
  if (bus_watch_id) {
    g_source_remove(bus_watch_id);
  }
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref(loop);

  return bus_state.exit_code;
}

void configure_uri_multiple_source(NvDsSourceConfig& cfg, const std::vector<std::string>& uris, guint source_id) {
  cfg = {};
  cfg.type = NV_DS_SOURCE_URI_MULTIPLE;
  cfg.enable = TRUE;
  cfg.gpu_id = 0;
  cfg.nvbuf_memory_type = 0;
  cfg.cuda_memory_type = 0;
  cfg.uri_list_loop = FALSE;
  cfg.camera_id = source_id;
  cfg.source_id = source_id;
  const std::string uri_list = join_semicolon(uris);
  cfg.uri_list = g_strdup(uri_list.c_str());
  cfg.uri = g_strdup(uris.front().c_str());
}

int expect_encoded_file(
    const fs::path& path,
    bool expect_audio,
    double min_audio_pts_seconds = 0.0,
    double min_video_pts_seconds = 0.0) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size < 1024) {
    std::cerr << "Expected encoded output file at " << path << ", got size=" << (ec ? 0 : size) << "\n";
    return 1;
  }
  if (!run_shell("ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 " +
                 shell_quote(path) + " | grep -qx video")) {
    std::cerr << "Expected video stream in " << path << "\n";
    return 2;
  }
  if (expect_audio &&
      !run_shell("ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 " +
                 shell_quote(path) + " | grep -qx audio")) {
    std::cerr << "Expected audio stream in " << path << "\n";
    return 3;
  }
  if (expect_audio && min_audio_pts_seconds > 0.0) {
    std::stringstream cmd;
    cmd << "ffprobe -v error -select_streams a:0 -show_packets -show_entries packet=pts_time "
        << "-of csv=p=0 " << shell_quote(path)
        << " | awk 'BEGIN { ok = 0 } { if (($1 + 0) >= " << min_audio_pts_seconds
        << ") ok = 1 } END { exit ok ? 0 : 1 }'";
    if (!run_shell(cmd.str())) {
      std::cerr << "Expected audio packets after " << min_audio_pts_seconds << "s in " << path << "\n";
      return 4;
    }
  }
  if (min_video_pts_seconds > 0.0) {
    std::stringstream cmd;
    cmd << "ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pts_time "
        << "-of csv=p=0 " << shell_quote(path)
        << " | awk 'BEGIN { ok = 0 } { if (($1 + 0) >= " << min_video_pts_seconds
        << ") ok = 1 } END { exit ok ? 0 : 1 }'";
    if (!run_shell(cmd.str())) {
      std::cerr << "Expected video packets after " << min_video_pts_seconds << "s in " << path << "\n";
      return 5;
    }
  }
  return 0;
}

int expect_audio_file(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size < 512) {
    std::cerr << "Expected audio output file at " << path << ", got size=" << (ec ? 0 : size) << "\n";
    return 1;
  }
  if (!run_shell("ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 " +
                 shell_quote(path) + " | grep -qx audio")) {
    std::cerr << "Expected audio stream in " << path << "\n";
    return 2;
  }
  return 0;
}

bool add_audio_mux_chain(GstElement* pipeline, const fs::path& output_path, GstElement** mux_out) {
  GstElement* mux = gst_element_factory_make("matroskamux", nullptr);
  GstElement* sink = gst_element_factory_make("filesink", nullptr);
  if (!mux || !sink) {
    std::cerr << "Failed to create audio mux chain\n";
    return false;
  }
  g_object_set(G_OBJECT(sink), "location", output_path.c_str(), "sync", FALSE, "async", FALSE, NULL);
  gst_bin_add_many(GST_BIN(pipeline), mux, sink, NULL);
  if (!gst_element_link(mux, sink)) {
    std::cerr << "Failed to link audio mux chain\n";
    return false;
  }
  *mux_out = mux;
  return true;
}

void configure_file_hmaudio(NvDsHmAudioConfig& audio_cfg, const fs::path& audio_path) {
  audio_cfg = {};
  audio_cfg.enable = TRUE;
  audio_cfg.src = SRC_FILE;
  audio_cfg.dest = DEST_SINK;
  audio_cfg.sink_id = -1;
  for (gint& sink_id : audio_cfg.multi_sink_ids) {
    sink_id = -1;
  }
  std::strncpy(audio_cfg.audio_location, fs::absolute(audio_path).c_str(), sizeof(audio_cfg.audio_location) - 1);
}

int run_file_audio_fanout_to_two_file_sinks(const fs::path& tmpdir, const fs::path& audio_path) {
  GstElement* pipeline = gst_pipeline_new("hmaudio-file-fanout-test");
  GstElement* mux0 = nullptr;
  GstElement* mux1 = nullptr;
  const fs::path out0 = tmpdir / "audio_fanout_0.mkv";
  const fs::path out1 = tmpdir / "audio_fanout_1.mkv";
  if (!pipeline || !add_audio_mux_chain(pipeline, out0, &mux0) || !add_audio_mux_chain(pipeline, out1, &mux1)) {
    return 2;
  }

  NvDsHmAudioConfig audio_cfg{};
  configure_file_hmaudio(audio_cfg, audio_path);

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
  sink_configs[0].type = NV_DS_SINK_ENCODE_FILE;
  sink_configs[1].enable = TRUE;
  sink_configs[1].sink_id = 1;
  sink_configs[1].type = NV_DS_SINK_ENCODE_FILE;

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].mux = mux0;
  sink_bin.sub_bins[1].mux = mux1;

  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed to create HMAudio fanout bin for two file sinks\n";
    return 3;
  }

  int rc = run_pipeline(pipeline, 15);
  if (rc != 0) {
    return rc;
  }
  rc = expect_audio_file(out0);
  if (rc != 0) {
    return rc;
  }
  return expect_audio_file(out1);
}

int run_file_audio_fanout_to_file_and_rtsp(const fs::path& tmpdir, const fs::path& audio_path) {
  GstElement* pipeline = gst_pipeline_new("hmaudio-file-rtsp-fanout-test");
  GstElement* mux = nullptr;
  const fs::path out = tmpdir / "audio_fanout_file_rtsp.mkv";
  if (!pipeline || !add_audio_mux_chain(pipeline, out, &mux)) {
    return 2;
  }

  NvDsHmAudioConfig audio_cfg{};
  configure_file_hmaudio(audio_cfg, audio_path);

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
  sink_configs[0].type = NV_DS_SINK_ENCODE_FILE;
  sink_configs[1].enable = TRUE;
  sink_configs[1].sink_id = 1;
  sink_configs[1].type = NV_DS_SINK_UDPSINK;
  sink_configs[1].encoder_config.udp_port = 15000 + (::getpid() % 10000);

  GstElement* dummy_video_payloader = gst_element_factory_make("identity", "dummy_rtsp_video_payloader");
  if (!dummy_video_payloader) {
    std::cerr << "Failed to create dummy RTSP video payloader\n";
    return 3;
  }

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].mux = mux;
  sink_bin.sub_bins[1].rtppay_or_flvmux = dummy_video_payloader;

  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed to create HMAudio fanout bin for file plus RTSP sinks\n";
    gst_object_unref(dummy_video_payloader);
    return 4;
  }
  gst_object_unref(dummy_video_payloader);

  int rc = run_pipeline(pipeline, 15);
  if (rc != 0) {
    return rc;
  }
  return expect_audio_file(out);
}

int run_multi_sink_without_ids_disables_audio(const fs::path& tmpdir, const fs::path& audio_path) {
  GstElement* pipeline = gst_pipeline_new("hmaudio-empty-multi-sink-test");
  GstElement* mux = nullptr;
  const fs::path out = tmpdir / "empty_multi_sink.mkv";
  if (!pipeline || !add_audio_mux_chain(pipeline, out, &mux)) {
    return 2;
  }

  NvDsHmAudioConfig audio_cfg{};
  configure_file_hmaudio(audio_cfg, audio_path);
  audio_cfg.dest = DEST_MULTI_SINK;

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
  sink_configs[0].type = NV_DS_SINK_ENCODE_FILE;

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].mux = mux;

  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed while creating HMAudio with empty multi-sink-ids\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 3;
  }
  if (audio_bin.bin) {
    std::cerr << "Expected empty multi-sink-ids to disable HMAudio instead of falling back to sink-id\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 4;
  }
  gst_object_unref(GST_OBJECT(pipeline));
  return 0;
}

int count_linked_sink_pads(GstElement* element) {
  int count = 0;
  GstIterator* iterator = gst_element_iterate_sink_pads(element);
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(iterator, &item)) {
      case GST_ITERATOR_OK: {
        GstPad* pad = GST_PAD(g_value_get_object(&item));
        if (gst_pad_is_linked(pad)) {
          ++count;
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(iterator);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  g_value_unset(&item);
  gst_iterator_free(iterator);
  return count;
}

bool have_gst_factory(const char* factory_name) {
  GstElementFactory* factory = gst_element_factory_find(factory_name);
  if (!factory) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

int run_webrtc_audio_branch_graph_test(const fs::path& audio_path) {
  if (!have_gst_factory("webrtcbin") || !have_gst_factory("opusenc") || !have_gst_factory("rtpopuspay")) {
    std::cerr << "Skipping WebRTC audio graph test because required GStreamer factories are missing\n";
    return 0;
  }

  GstElement* pipeline = gst_pipeline_new("hmaudio-webrtc-graph-test");
  GstElement* webrtc_parent = gst_bin_new("test_webrtc_parent");
  GstElement* webrtc = gst_element_factory_make("webrtcbin", "test_webrtcbin");
  if (!pipeline || !webrtc_parent || !webrtc) {
    std::cerr << "Failed to create WebRTC graph test elements\n";
    return 2;
  }
  gst_bin_add(GST_BIN(pipeline), webrtc_parent);
  gst_bin_add(GST_BIN(webrtc_parent), webrtc);

  NvDsHmAudioConfig audio_cfg{};
  configure_file_hmaudio(audio_cfg, audio_path);

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 4;
  sink_configs[0].type = NV_DS_SINK_WEBRTC;

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].bin = webrtc_parent;
  sink_bin.sub_bins[0].sink = webrtc;

  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed to create HMAudio WebRTC audio branch\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 3;
  }
  if (count_linked_sink_pads(webrtc) == 0) {
    std::cerr << "Expected HMAudio to link an RTP audio pad into webrtcbin\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 4;
  }
  gst_object_unref(GST_OBJECT(pipeline));
  return 0;
}

int run_decode_encode(
    const fs::path& tmpdir,
    const std::vector<std::string>& uris,
    const std::string& output_name = "decode_encode.mkv",
    double min_audio_pts_seconds = 1.5,
    double min_video_pts_seconds = 1.5) {
  NvDsSourceConfig cfg{};
  configure_uri_multiple_source(cfg, uris, /*source_id=*/0);

  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsSrcBin& src_bin = src_bins[0];
  if (!create_source_bin(&cfg, &src_bin)) {
    std::cerr << "Failed to create URI-MULTIPLE source bin\n";
    return 2;
  }

  GstElement* pipeline = gst_pipeline_new("uri-multiple-decode-encode-test");
  std::vector<GstElement*> enc_chain;
  const fs::path out = tmpdir / output_name;
  if (!pipeline || !add_encoder_chain(pipeline, out, enc_chain)) {
    return 3;
  }

  gst_bin_add(GST_BIN(pipeline), src_bin.bin);
  if (!gst_element_link(src_bin.bin, enc_chain.front())) {
    std::cerr << "Failed to link source to encoder\n";
    return 4;
  }

  NvDsHmAudioConfig audio_cfg{};
  audio_cfg.enable = TRUE;
  audio_cfg.src = SRC_SOURCE_BIN;
  audio_cfg.source_id = 0;
  audio_cfg.dest = DEST_SINK;
  audio_cfg.sink_id = 0;

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
  sink_configs[0].type = NV_DS_SINK_ENCODE_FILE;

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].mux = enc_chain[4];

  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed to create HMAudio bin for URI-MULTIPLE source\n";
    return 6;
  }

  BufferCounter counter{};
  GstPad* srcpad = gst_element_get_static_pad(src_bin.bin, "src");
  gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, count_buffers_probe, &counter, NULL);
  gst_object_unref(srcpad);

  int rc = run_pipeline(pipeline, 25);
  if (rc != 0) {
    return rc;
  }
  const guint expected_switches = static_cast<guint>(uris.size() - 1);
  if (src_bin.uri_switch_count != expected_switches) {
    std::cerr << "Expected " << expected_switches << " URI-MULTIPLE switches, got " << src_bin.uri_switch_count
              << "\n";
    return 7;
  }
  if (counter.buffers == 0) {
    std::cerr << "Expected URI-MULTIPLE video buffers\n";
    return 7;
  }
  return expect_encoded_file(out, /*expect_audio=*/true, min_audio_pts_seconds, min_video_pts_seconds);
}

int run_decode_compose_encode(
    const fs::path& tmpdir,
    const std::vector<std::string>& left_uris,
    const std::vector<std::string>& right_uris) {
  NvDsSourceConfig configs[2]{};
  configure_uri_multiple_source(configs[0], left_uris, /*source_id=*/0);
  configure_uri_multiple_source(configs[1], right_uris, /*source_id=*/1);

  NvDsSrcParentBin src_parent{};
  if (!create_multi_source_bin(2, configs, &src_parent)) {
    std::cerr << "Failed to create multi-source URI-MULTIPLE bin\n";
    return 2;
  }
  g_object_set(
      G_OBJECT(src_parent.streammux),
      "batch-size",
      2,
      "width",
      256,
      "height",
      144,
      "batched-push-timeout",
      40000,
      "live-source",
      FALSE,
      NULL);

  GstElement* pipeline = gst_pipeline_new("uri-multiple-decode-compose-encode-test");
  GstElement* tiler = gst_element_factory_make("nvmultistreamtiler", "test-compose-tiler");
  if (!pipeline || !tiler) {
    std::cerr << "Failed to create pipeline/tiler\n";
    return 3;
  }
  g_object_set(G_OBJECT(tiler), "rows", 1, "columns", 2, "width", 512, "height", 144, NULL);

  std::vector<GstElement*> enc_chain;
  const fs::path out = tmpdir / "decode_compose_encode.mkv";
  if (!add_encoder_chain(pipeline, out, enc_chain)) {
    return 4;
  }

  gst_bin_add_many(GST_BIN(pipeline), src_parent.bin, tiler, NULL);
  if (!gst_element_link(src_parent.bin, tiler) || !gst_element_link(tiler, enc_chain.front())) {
    std::cerr << "Failed to link decode-compose-encode pipeline\n";
    return 5;
  }

  NvDsHmAudioConfig audio_cfg{};
  audio_cfg.enable = TRUE;
  audio_cfg.src = SRC_SOURCE_BIN;
  audio_cfg.source_id = 0;
  audio_cfg.dest = DEST_SINK;
  audio_cfg.sink_id = 0;

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
  sink_configs[0].type = NV_DS_SINK_ENCODE_FILE;

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].mux = enc_chain[4];

  NvDsHmAudioBin audio_bin{};
  if (!create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_parent.sub_bins, sink_configs, &sink_bin)) {
    std::cerr << "Failed to create HMAudio bin for multi-source URI-MULTIPLE source\n";
    return 6;
  }

  BufferCounter counter{};
  GstPad* tiler_src = gst_element_get_static_pad(tiler, "src");
  gst_pad_add_probe(tiler_src, GST_PAD_PROBE_TYPE_BUFFER, count_buffers_probe, &counter, NULL);
  gst_object_unref(tiler_src);

  int rc = run_pipeline(pipeline, 30);
  if (rc != 0) {
    return rc;
  }
  const guint expected_switches = static_cast<guint>(left_uris.size() - 1);
  if (src_parent.sub_bins[0].uri_switch_count != expected_switches ||
      src_parent.sub_bins[1].uri_switch_count != expected_switches) {
    std::cerr << "Expected " << expected_switches << " URI-MULTIPLE switches per source, got "
              << src_parent.sub_bins[0].uri_switch_count << " and " << src_parent.sub_bins[1].uri_switch_count << "\n";
    return 7;
  }
  if (counter.buffers == 0) {
    std::cerr << "Expected composed URI-MULTIPLE buffers\n";
    return 7;
  }
  return expect_encoded_file(out, /*expect_audio=*/true, /*min_audio_pts_seconds=*/1.5);
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  const fs::path tmpdir = fs::temp_directory_path() / ("uri_multiple_pipeline_test_" + std::to_string(::getpid()));
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);

  const fs::path a0 = tmpdir / "left_0.mp4";
  const fs::path a1 = tmpdir / "left_1.mp4";
  const fs::path b0 = tmpdir / "right_0.mp4";
  const fs::path b1 = tmpdir / "right_1.mp4";
  if (!make_synthetic_mp4(a0, 1, 0, 440) || !make_synthetic_mp4(a1, 1, 45, 494) ||
      !make_synthetic_mp4(b0, 1, 90, 0) || !make_synthetic_mp4(b1, 1, 135, 0)) {
    std::cerr << "Failed to generate synthetic mp4 chapters with ffmpeg\n";
    fs::remove_all(tmpdir);
    return 2;
  }

  const std::vector<std::string> left_uris{to_file_uri(a0), to_file_uri(a1)};
  const std::vector<std::string> right_uris{to_file_uri(b0), to_file_uri(b1)};

  int rc = run_decode_encode(tmpdir, left_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  const std::vector<std::string> final_without_audio_uris{to_file_uri(a0), to_file_uri(b0)};
  rc = run_decode_encode(
      tmpdir,
      final_without_audio_uris,
      "decode_encode_final_without_audio.mkv",
      /*min_audio_pts_seconds=*/0.5,
      /*min_video_pts_seconds=*/1.5);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_decode_compose_encode(tmpdir, left_uris, right_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_file_audio_fanout_to_two_file_sinks(tmpdir, a0);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_file_audio_fanout_to_file_and_rtsp(tmpdir, a0);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_multi_sink_without_ids_disables_audio(tmpdir, a0);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_webrtc_audio_branch_graph_test(a0);
  fs::remove_all(tmpdir);
  return rc;
}
