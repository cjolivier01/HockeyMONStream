#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include <gst/gst.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "nvdsmeta.h"

GST_DEBUG_CATEGORY(NVDS_APP);

namespace fs = std::filesystem;

namespace {

struct BusState {
  GMainLoop* loop{nullptr};
  int exit_code{0};
  guint timeout_id{0};
  NvDsSrcParentBin* source_parent{nullptr};
};

gboolean timeout_call(gpointer data) {
  BusState* state = static_cast<BusState*>(data);
  std::cerr << "ERROR: timed out waiting for EOS/error\n";
  state->exit_code = 2;
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
      if (state->source_parent) {
        cancel_uri_playlist_frame_barrier(state->source_parent);
      }
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
  guint64 sleep_time_us{0};
  guint64 sleep_on_buffer{0};
};

struct AudioTimelineStats {
  guint64 buffers{0};
  guint64 eos_events{0};
  guint64 invalid_timestamps{0};
  guint64 discontinuities{0};
  GstClockTime first_pts{GST_CLOCK_TIME_NONE};
  GstClockTime previous_end{GST_CLOCK_TIME_NONE};
  GstClockTime final_end{GST_CLOCK_TIME_NONE};
};

struct MuxBatchStats {
  guint64 batches{0};
  guint64 incomplete_batches{0};
  guint64 invalid_metadata_batches{0};
  guint64 missing_decoded_sequence_meta{0};
  guint64 decode_mux_sequence_mismatches{0};
  bool exact_frame_pairs{true};
  std::map<guint, guint64> frames_by_source;
  std::map<guint, guint64> next_decoded_sequence_by_source;
  std::vector<std::vector<std::pair<guint, guint>>> batch_frames;
};

enum class ExpectedPipelineFailure {
  kNone,
  kBusError,
  kStateChangeFailure,
};

GstPadProbeReturn count_buffers_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_OK;
  }
  auto* counter = static_cast<BufferCounter*>(user_data);
  ++counter->buffers;
  if (counter->sleep_time_us > 0 && (counter->sleep_on_buffer == 0 || counter->buffers == counter->sleep_on_buffer)) {
    g_usleep(counter->sleep_time_us);
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn inspect_audio_timeline_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
  auto* stats = static_cast<AudioTimelineStats*>(user_data);
  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0 &&
      GST_EVENT_TYPE(GST_EVENT(info->data)) == GST_EVENT_EOS) {
    ++stats->eos_events;
    return GST_PAD_PROBE_OK;
  }
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_OK;
  }
  GstBuffer* buffer = GST_BUFFER(info->data);
  const GstClockTime pts = GST_BUFFER_PTS(buffer);
  const GstClockTime duration = GST_BUFFER_DURATION(buffer);
  ++stats->buffers;
  if (!GST_CLOCK_TIME_IS_VALID(pts) || !GST_CLOCK_TIME_IS_VALID(duration)) {
    ++stats->invalid_timestamps;
    return GST_PAD_PROBE_OK;
  }
  if (!GST_CLOCK_TIME_IS_VALID(stats->first_pts)) {
    stats->first_pts = pts;
  }
  // AAC decoder priming can overlap a chapter boundary by one packet. A forward jump larger than two 48 kHz AAC
  // packets, however, means audio was skipped while the camera playlist advanced.
  constexpr GstClockTime kMaximumAudioGap = 2 * 1024 * GST_SECOND / 48000;
  if (GST_CLOCK_TIME_IS_VALID(stats->previous_end) && pts > stats->previous_end + kMaximumAudioGap) {
    ++stats->discontinuities;
  }
  const GstClockTime end = pts + duration;
  stats->previous_end = GST_CLOCK_TIME_IS_VALID(stats->previous_end) ? std::max(stats->previous_end, end) : end;
  stats->final_end = GST_CLOCK_TIME_IS_VALID(stats->final_end) ? std::max(stats->final_end, end) : end;
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn inspect_mux_batches_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_OK;
  }
  auto* stats = static_cast<MuxBatchStats*>(user_data);
  GstBuffer* buffer = GST_BUFFER(info->data);
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buffer);
  if (!batch_meta) {
    ++stats->invalid_metadata_batches;
    return GST_PAD_PROBE_OK;
  }

  ++stats->batches;
  if (batch_meta->num_frames_in_batch != 2) {
    ++stats->incomplete_batches;
  }

  std::set<guint> source_ids;
  std::set<gint> frame_numbers;
  std::vector<std::pair<guint, guint>> frames;
  guint frame_meta_count = 0;
  for (NvDsFrameMetaList* item = batch_meta->frame_meta_list; item != nullptr; item = item->next) {
    auto* frame_meta = static_cast<NvDsFrameMeta*>(item->data);
    if (!frame_meta) {
      ++stats->invalid_metadata_batches;
      continue;
    }
    ++frame_meta_count;
    ++stats->frames_by_source[frame_meta->source_id];
    const std::optional<hm::DecodedFrameSequence> decoded_sequence = hm::decoded_frame_sequence(frame_meta);
    if (!decoded_sequence.has_value()) {
      ++stats->missing_decoded_sequence_meta;
    } else if (
        decoded_sequence->source_id != frame_meta->source_id ||
        decoded_sequence->sequence != static_cast<uint64_t>(frame_meta->frame_num) ||
        decoded_sequence->sequence != stats->next_decoded_sequence_by_source[frame_meta->source_id]) {
      ++stats->decode_mux_sequence_mismatches;
    } else {
      ++stats->next_decoded_sequence_by_source[frame_meta->source_id];
    }
    source_ids.insert(frame_meta->source_id);
    frame_numbers.insert(frame_meta->frame_num);
    frames.emplace_back(frame_meta->source_id, frame_meta->frame_num);
  }
  stats->batch_frames.push_back(std::move(frames));
  if (frame_meta_count != 2 || source_ids != std::set<guint>{0, 1}) {
    ++stats->incomplete_batches;
  }
  if (frame_numbers.size() != 1) {
    stats->exact_frame_pairs = false;
  }
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

bool make_synthetic_mp4(const fs::path& path, double seconds, int hue_degrees, int audio_frequency) {
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

bool make_synthetic_audio(const fs::path& path, int seconds) {
  std::stringstream cmd;
  cmd << "ffmpeg -hide_banner -loglevel error -y -f lavfi -i "
      << "'sine=frequency=440:sample_rate=48000' -t " << seconds << " -c:a aac -b:a 64k " << shell_quote(path);
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

int run_pipeline(GstElement* pipeline, int timeout_seconds, NvDsSrcParentBin* source_parent = nullptr) {
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  BusState bus_state{.loop = loop, .exit_code = 0, .source_parent = source_parent};

  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  const guint bus_watch_id = gst_bus_add_watch(bus, bus_call, &bus_state);
  gst_object_unref(bus);
  bus_state.timeout_id = g_timeout_add_seconds(timeout_seconds, timeout_call, &bus_state);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to set pipeline to PLAYING\n";
    if (source_parent) {
      cancel_uri_playlist_frame_barrier(source_parent);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_source_remove(bus_state.timeout_id);
    g_source_remove(bus_watch_id);
    gst_object_unref(GST_OBJECT(pipeline));
    g_main_loop_unref(loop);
    return 3;
  }

  g_main_loop_run(loop);

  if (source_parent) {
    cancel_uri_playlist_frame_barrier(source_parent);
  }
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

int run_pipeline_with_preroll(GstElement* pipeline, int timeout_seconds, NvDsSrcParentBin* source_parent) {
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to set initially positioned pipeline to PAUSED\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 3;
  }
  constexpr GstClockTime kPrerollTimeout = 10 * GST_SECOND;
  const GstStateChangeReturn preroll = gst_element_get_state(pipeline, nullptr, nullptr, kPrerollTimeout);
  if (preroll == GST_STATE_CHANGE_FAILURE || preroll == GST_STATE_CHANGE_ASYNC) {
    std::cerr << "Initially positioned pipeline did not complete PAUSED preroll\n";
    cancel_uri_playlist_frame_barrier(source_parent);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    return 3;
  }
  return run_pipeline(pipeline, timeout_seconds, source_parent);
}

void configure_uri_multiple_source(
    NvDsSourceConfig& cfg,
    const std::vector<std::string>& uris,
    guint source_id,
    bool include_uri_list = true) {
  cfg = {};
  cfg.type = NV_DS_SOURCE_URI_MULTIPLE;
  cfg.enable = TRUE;
  cfg.gpu_id = 0;
  cfg.nvbuf_memory_type = 0;
  cfg.cuda_memory_type = 0;
  cfg.uri_list_loop = FALSE;
  cfg.camera_id = source_id;
  cfg.source_id = source_id;
  if (include_uri_list) {
    const std::string uri_list = join_semicolon(uris);
    cfg.uri_list = g_strdup(uri_list.c_str());
  }
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
  if (!run_shell(
          "ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 " + shell_quote(path) +
          " | grep -qx video")) {
    std::cerr << "Expected video stream in " << path << "\n";
    return 2;
  }
  if (expect_audio &&
      !run_shell(
          "ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 " + shell_quote(path) +
          " | grep -qx audio")) {
    std::cerr << "Expected audio stream in " << path << "\n";
    return 3;
  }
  if (expect_audio && min_audio_pts_seconds > 0.0) {
    std::stringstream cmd;
    cmd << "ffprobe -v error -select_streams a:0 -show_packets -show_entries packet=pts_time "
        << "-of csv=p=0 " << shell_quote(path) << " | awk 'BEGIN { ok = 0 } { if (($1 + 0) >= " << min_audio_pts_seconds
        << ") ok = 1 } END { exit ok ? 0 : 1 }'";
    if (!run_shell(cmd.str())) {
      std::cerr << "Expected audio packets after " << min_audio_pts_seconds << "s in " << path << "\n";
      return 4;
    }
  }
  if (min_video_pts_seconds > 0.0) {
    std::stringstream cmd;
    cmd << "ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pts_time "
        << "-of csv=p=0 " << shell_quote(path) << " | awk 'BEGIN { ok = 0 } { if (($1 + 0) >= " << min_video_pts_seconds
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
  if (!run_shell(
          "ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 " + shell_quote(path) +
          " | grep -qx audio")) {
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
      "batched-push-timeout",
      1000000,
      "sync-inputs",
      FALSE,
      "frame-num-reset-on-stream-reset",
      FALSE,
      "frame-num-reset-on-eos",
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

  int rc = run_pipeline(pipeline, 30, &src_parent);
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

int run_single_uri_multiple_source(const std::string& uri) {
  NvDsSourceConfig config{};
  configure_uri_multiple_source(config, {uri}, /*source_id=*/0, /*include_uri_list=*/false);

  NvDsSrcParentBin src_parent{};
  if (!create_multi_source_bin(1, &config, &src_parent)) {
    std::cerr << "Failed to create single URI-MULTIPLE source bin\n";
    return 2;
  }
  GstElementFactory* mux_factory = gst_element_get_factory(src_parent.streammux);
  const gchar* mux_name = mux_factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(mux_factory)) : nullptr;
  if (src_parent.uri_playlist_exact_pairing_enabled || g_strcmp0(mux_name, "hstreamlosslessmux") == 0) {
    std::cerr << "Single stitched-output playback incorrectly enabled the two-camera lossless barrier\n";
    return 3;
  }
  g_object_set(
      G_OBJECT(src_parent.streammux),
      "batch-size",
      1,
      "batched-push-timeout",
      1000000,
      "sync-inputs",
      FALSE,
      "frame-num-reset-on-stream-reset",
      FALSE,
      "frame-num-reset-on-eos",
      FALSE,
      NULL);

  GstElement* pipeline = gst_pipeline_new("single-uri-multiple-source-test");
  GstElement* sink = gst_element_factory_make("fakesink", "single-uri-multiple-sink");
  if (!pipeline || !sink) {
    return 4;
  }
  g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, NULL);
  gst_bin_add_many(GST_BIN(pipeline), src_parent.bin, sink, NULL);
  if (!gst_element_link(src_parent.bin, sink)) {
    std::cerr << "Failed to link single URI-MULTIPLE source pipeline\n";
    return 4;
  }

  BufferCounter counter{};
  GstPad* source_pad = gst_element_get_static_pad(src_parent.sub_bins[0].bin, "src");
  gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffers_probe, &counter, nullptr);
  gst_object_unref(source_pad);
  const int rc = run_pipeline(pipeline, 15, &src_parent);
  if (rc != 0) {
    return rc;
  }
  if (counter.buffers != 15) {
    std::cerr << "Single URI-MULTIPLE source emitted " << counter.buffers << " frames instead of 15\n";
    return 5;
  }
  return 0;
}

int run_lossless_two_camera_mux(
    const std::vector<std::string>& left_uris,
    const std::vector<std::string>& right_uris,
    bool include_right_uri_list = true,
    guint audio_source_id = 0,
    guint64 audio_sleep_time_us = 0,
    ExpectedPipelineFailure expected_failure = ExpectedPipelineFailure::kNone,
    guint64 video_sleep_time_us = 0,
    gint video_sleep_source_id = -1,
    guint64 video_sleep_on_buffer = 0,
    GstClockTime left_initial_offset = 0,
    GstClockTime right_initial_offset = 0,
    GstClockTime start_time = 0,
    bool pause_before_playing = false) {
  NvDsSourceConfig configs[2]{};
  configure_uri_multiple_source(configs[0], left_uris, /*source_id=*/0);
  configure_uri_multiple_source(configs[1], right_uris, /*source_id=*/1, include_right_uri_list);

  NvDsSrcParentBin src_parent{};
  if (!create_multi_source_bin(2, configs, &src_parent)) {
    std::cerr << "Failed to create lossless two-camera source bin\n";
    return 2;
  }
  if ((left_initial_offset || right_initial_offset || start_time) &&
      !configure_uri_playlist_initial_offsets(
          &src_parent, left_initial_offset, right_initial_offset, audio_source_id, start_time)) {
    std::cerr << "Failed to configure initial camera positions before preroll\n";
    return 2;
  }
  GstElementFactory* mux_factory = gst_element_get_factory(src_parent.streammux);
  if (!mux_factory ||
      g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(mux_factory)), "hstreamlosslessmux") != 0) {
    std::cerr << "Two-camera URI-MULTIPLE did not select the full-batch-only HStream mux\n";
    return 2;
  }
  g_object_set(
      G_OBJECT(src_parent.streammux),
      "batch-size",
      2,
      "batched-push-timeout",
      1000000,
      "sync-inputs",
      FALSE,
      "frame-num-reset-on-stream-reset",
      FALSE,
      "frame-num-reset-on-eos",
      FALSE,
      NULL);

  GstElement* pipeline = gst_pipeline_new("uri-multiple-lossless-mux-test");
  GstElement* sink = gst_element_factory_make("fakesink", "lossless-mux-sink");
  GstElement* audio_sink = gst_element_factory_make("fakesink", "lossless-audio-sink");
  GstElement* audio_delay =
      audio_sleep_time_us > 0 ? gst_element_factory_make("identity", "lossless-audio-delay") : nullptr;
  if (!pipeline || !sink || !audio_sink || (audio_sleep_time_us > 0 && !audio_delay)) {
    std::cerr << "Failed to create lossless mux pipeline/sinks\n";
    return 3;
  }
  g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, NULL);
  g_object_set(G_OBJECT(audio_sink), "sync", FALSE, "async", FALSE, NULL);
  gst_bin_add_many(GST_BIN(pipeline), src_parent.bin, sink, audio_sink, NULL);
  if (audio_delay) {
    g_object_set(G_OBJECT(audio_delay), "sleep-time", static_cast<guint>(audio_sleep_time_us), NULL);
    gst_bin_add(GST_BIN(pipeline), audio_delay);
    if (!gst_element_link(audio_delay, audio_sink)) {
      std::cerr << "Failed to link delayed source audio\n";
      return 4;
    }
  }
  if (!gst_element_link(src_parent.bin, sink)) {
    std::cerr << "Failed to link lossless mux pipeline\n";
    return 4;
  }
  if (!link_uri_source_audio_src(&src_parent.sub_bins[audio_source_id], audio_delay ? audio_delay : audio_sink)) {
    std::cerr << "Failed to link source audio for lossless mux pipeline\n";
    return 4;
  }

  BufferCounter source_counters[2]{};
  MuxBatchStats mux_stats{};
  AudioTimelineStats audio_stats{};
  AudioTimelineStats video_timeline_stats[2]{};
  for (guint source_id = 0; source_id < 2; ++source_id) {
    if (video_sleep_source_id < 0 || static_cast<guint>(video_sleep_source_id) == source_id) {
      source_counters[source_id].sleep_time_us = video_sleep_time_us;
      source_counters[source_id].sleep_on_buffer = video_sleep_on_buffer;
    }
    GstPad* source_pad = gst_element_get_static_pad(src_parent.sub_bins[source_id].bin, "src");
    gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffers_probe, &source_counters[source_id], nullptr);
    gst_pad_add_probe(
        source_pad, GST_PAD_PROBE_TYPE_BUFFER, inspect_audio_timeline_probe, &video_timeline_stats[source_id], nullptr);
    gst_object_unref(source_pad);
  }
  GstPad* mux_src = gst_element_get_static_pad(src_parent.streammux, "src");
  gst_pad_add_probe(mux_src, GST_PAD_PROBE_TYPE_BUFFER, inspect_mux_batches_probe, &mux_stats, nullptr);
  gst_object_unref(mux_src);
  GstPad* audio_sink_pad = gst_element_get_static_pad(audio_sink, "sink");
  gst_pad_add_probe(
      audio_sink_pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      inspect_audio_timeline_probe,
      &audio_stats,
      nullptr);
  gst_object_unref(audio_sink_pad);

  const int rc = pause_before_playing ? run_pipeline_with_preroll(pipeline, 30, &src_parent)
                                      : run_pipeline(pipeline, 30, &src_parent);
  if (expected_failure != ExpectedPipelineFailure::kNone) {
    const int expected_rc = expected_failure == ExpectedPipelineFailure::kBusError ? 1 : 3;
    if (rc != expected_rc) {
      std::cerr << "Expected prompt pipeline failure " << expected_rc << ", got run result " << rc << "\n";
      return 10;
    }
    return 0;
  }
  if (rc != 0) {
    std::cerr << "Lossless mux failure diagnostics: audio_buffers=" << audio_stats.buffers
              << ", audio_eos_events=" << audio_stats.eos_events << "\n";
    return rc;
  }
  const size_t expected_complete_chapters = std::min(left_uris.size(), right_uris.size());
  const guint minimum_switches = static_cast<guint>(expected_complete_chapters - 1);
  const guint maximum_left_switches =
      std::min<guint>(left_uris.size() - 1, static_cast<guint>(expected_complete_chapters));
  const guint maximum_right_switches =
      std::min<guint>(right_uris.size() - 1, static_cast<guint>(expected_complete_chapters));
  if (src_parent.sub_bins[0].uri_switch_count < minimum_switches ||
      src_parent.sub_bins[0].uri_switch_count > maximum_left_switches ||
      src_parent.sub_bins[1].uri_switch_count < minimum_switches ||
      src_parent.sub_bins[1].uri_switch_count > maximum_right_switches) {
    std::cerr << "Unexpected URI switch counts before terminal pairing: " << src_parent.sub_bins[0].uri_switch_count
              << " and " << src_parent.sub_bins[1].uri_switch_count << "\n";
    return 5;
  }

  constexpr guint64 kFramesPerChapter = 15;
  const guint64 positioned_frames = std::max(
      src_parent.sub_bins[0].uri_list_initial_positioned_frame_count,
      src_parent.sub_bins[1].uri_list_initial_positioned_frame_count);
  const guint64 expected_frames_per_source = kFramesPerChapter * expected_complete_chapters - positioned_frames;
  for (guint source_id = 0; source_id < 2; ++source_id) {
    const guint64 decoded_before_terminal = src_parent.sub_bins[source_id].uri_list_decoded_frame_count -
        src_parent.sub_bins[source_id].uri_list_terminal_dropped_frame_count;
    if (decoded_before_terminal != expected_frames_per_source ||
        source_counters[source_id].buffers != expected_frames_per_source ||
        mux_stats.frames_by_source[source_id] != expected_frames_per_source ||
        src_parent.sub_bins[source_id].uri_list_mux_delivered_sequence != expected_frames_per_source - 1) {
      std::cerr << "Frame loss for source " << source_id
                << ": decoded=" << src_parent.sub_bins[source_id].uri_list_decoded_frame_count
                << ", terminal_dropped=" << src_parent.sub_bins[source_id].uri_list_terminal_dropped_frame_count
                << ", pre_mux=" << source_counters[source_id].buffers
                << ", mux_delivered_sequence=" << src_parent.sub_bins[source_id].uri_list_mux_delivered_sequence
                << ", post_mux=" << mux_stats.frames_by_source[source_id] << "\n";
      return 6;
    }
  }
  const guint64 expected_left_positioned_frames = ((start_time + left_initial_offset) * 15) / GST_SECOND;
  const guint64 expected_right_positioned_frames = ((start_time + right_initial_offset) * 15) / GST_SECOND;
  if (src_parent.sub_bins[0].uri_list_initial_positioned_frame_count != expected_left_positioned_frames ||
      src_parent.sub_bins[1].uri_list_initial_positioned_frame_count != expected_right_positioned_frames) {
    std::cerr << "Initial positioning consumed unexpected frame counts: left="
              << src_parent.sub_bins[0].uri_list_initial_positioned_frame_count
              << ", right=" << src_parent.sub_bins[1].uri_list_initial_positioned_frame_count << "\n";
    return 6;
  }
  if (mux_stats.batches != expected_frames_per_source || mux_stats.incomplete_batches != 0 ||
      mux_stats.invalid_metadata_batches != 0 || mux_stats.missing_decoded_sequence_meta != 0 ||
      mux_stats.decode_mux_sequence_mismatches != 0 || !mux_stats.exact_frame_pairs) {
    std::cerr << "Expected " << expected_frames_per_source
              << " complete exact camera pairs; batches=" << mux_stats.batches
              << ", incomplete=" << mux_stats.incomplete_batches
              << ", invalid_metadata=" << mux_stats.invalid_metadata_batches
              << ", missing_decode_sequence=" << mux_stats.missing_decoded_sequence_meta
              << ", decode_mux_mismatches=" << mux_stats.decode_mux_sequence_mismatches
              << ", exact_frame_pairs=" << mux_stats.exact_frame_pairs << "\n";
    for (size_t batch_index = 0; batch_index < mux_stats.batch_frames.size(); ++batch_index) {
      std::cerr << "  batch " << batch_index << ":";
      for (const auto& [source_id, frame_num] : mux_stats.batch_frames[batch_index]) {
        std::cerr << " s" << source_id << "/f" << frame_num;
      }
      std::cerr << "\n";
    }
    return 7;
  }
  for (guint source_id = 0; source_id < 2; ++source_id) {
    if (video_timeline_stats[source_id].buffers == 0 ||
        !GST_CLOCK_TIME_IS_VALID(video_timeline_stats[source_id].first_pts) ||
        video_timeline_stats[source_id].first_pts > GST_SECOND / 10) {
      std::cerr << "Initially positioned video source " << source_id
                << " did not start near zero: first_pts=" << video_timeline_stats[source_id].first_pts << "\n";
      return 7;
    }
  }
  if (!src_parent.sub_bins[0].uri_list_permanently_ended || !src_parent.sub_bins[1].uri_list_permanently_ended) {
    std::cerr << "Expected both camera playlists to terminate together when either camera permanently ended\n";
    return 8;
  }
  const GstClockTime expected_audio_duration = expected_frames_per_source * GST_SECOND / 15;
  // One missing 15 fps frame would shorten the terminal frontier by ~66 ms. Keep the tolerance below that so the
  // regression fails if terminal EOS snapshots sequence N-1 after sequence N was already released by the barrier.
  const GstClockTime minimum_audio_duration = expected_audio_duration - GST_SECOND / 50;
  const GstClockTime maximum_audio_duration = expected_audio_duration + GST_SECOND / 10;
  const GstClockTime paired_video_end = src_parent.uri_playlist_paired_video_end;
  if (!GST_CLOCK_TIME_IS_VALID(paired_video_end) || paired_video_end + GST_SECOND / 50 < expected_audio_duration ||
      paired_video_end > expected_audio_duration + GST_SECOND / 50 || audio_stats.buffers == 0 ||
      audio_stats.eos_events != 1 || audio_stats.invalid_timestamps != 0 || audio_stats.discontinuities != 0 ||
      audio_stats.first_pts > GST_SECOND / 10 || audio_stats.final_end < minimum_audio_duration ||
      audio_stats.final_end > maximum_audio_duration || audio_stats.final_end + GST_SECOND / 50 < paired_video_end) {
    std::cerr << "Source audio was not continuous through the coordinated camera playlist: buffers="
              << audio_stats.buffers << ", eos_events=" << audio_stats.eos_events
              << ", invalid_timestamps=" << audio_stats.invalid_timestamps
              << ", discontinuities=" << audio_stats.discontinuities
              << ", first_pts=" << GST_TIME_AS_SECONDS(audio_stats.first_pts)
              << "s, final_end=" << GST_TIME_AS_SECONDS(audio_stats.final_end)
              << "s, paired_video_end=" << GST_TIME_AS_SECONDS(paired_video_end)
              << "s, expected=" << expected_complete_chapters << "s\n";
    return 9;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  g_setenv("USE_NEW_NVSTREAMMUX", "yes", TRUE);
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  const fs::path tmpdir = fs::temp_directory_path() / ("uri_multiple_pipeline_test_" + std::to_string(::getpid()));
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);

  const fs::path a0 = tmpdir / "left_0.mp4";
  const fs::path a1 = tmpdir / "left_1.mp4";
  const fs::path a2 = tmpdir / "left_2.mp4";
  const fs::path b0 = tmpdir / "right_0.mp4";
  const fs::path b1 = tmpdir / "right_1.mp4";
  const fs::path b2 = tmpdir / "right_2.mp4";
  const fs::path shifted_b0 = tmpdir / "right_shifted_0.mp4";
  const fs::path shifted_b1 = tmpdir / "right_shifted_1.mp4";
  const fs::path shifted_b2 = tmpdir / "right_shifted_2.mp4";
  const fs::path audio = tmpdir / "continuous_audio.m4a";
  if (!make_synthetic_mp4(a0, 1, 0, 440) || !make_synthetic_mp4(a1, 1, 30, 494) ||
      !make_synthetic_mp4(a2, 1, 60, 523) || !make_synthetic_mp4(b0, 1, 90, 587) ||
      !make_synthetic_mp4(b1, 1, 120, 659) || !make_synthetic_mp4(b2, 1, 150, 698) ||
      !make_synthetic_mp4(shifted_b0, 0.8, 180, 740) || !make_synthetic_mp4(shifted_b1, 1.2, 210, 784) ||
      !make_synthetic_mp4(shifted_b2, 1.0, 240, 831) || !make_synthetic_audio(audio, 3)) {
    std::cerr << "Failed to generate synthetic mp4 chapters with ffmpeg\n";
    fs::remove_all(tmpdir);
    return 2;
  }

  const std::vector<std::string> left_uris{to_file_uri(a0), to_file_uri(a1), to_file_uri(a2)};
  const std::vector<std::string> right_uris{to_file_uri(b0), to_file_uri(b1), to_file_uri(b2)};
  const std::vector<std::string> shifted_right_uris{
      to_file_uri(shifted_b0), to_file_uri(shifted_b1), to_file_uri(shifted_b2)};

  int rc = run_decode_compose_encode(tmpdir, left_uris, right_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Drive selected audio ahead while the other camera ends in the middle of this camera's next physical chapter.
  // Hold only the selected camera's final committed video buffer before its mux-sink acknowledgement: peer audio EOS
  // can then reach deferred terminal teardown first, which must still wait rather than flush that final video pair.
  rc = run_lossless_two_camera_mux(
      shifted_right_uris,
      {to_file_uri(b0)},
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/0,
      /*expected_failure=*/ExpectedPipelineFailure::kNone,
      /*video_sleep_time_us=*/500000,
      /*video_sleep_source_id=*/0,
      /*video_sleep_on_buffer=*/15);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_lossless_two_camera_mux(left_uris, right_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Production startup first drives the pipeline through PAUSED to initialize models and video-overlay sinks. The
  // asymmetric synchronization offset must be consumed before sequence zero, not as a post-preroll flushing seek
  // that can revoke one half of an already committed pair. 100312679 ns reproduces sharks-12-1-r4 (~3 frames at
  // 29.97 fps); with this 15 fps source it consumes exactly one complete frame before pair zero.
  rc = run_lossless_two_camera_mux(
      left_uris,
      right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/0,
      /*expected_failure=*/ExpectedPipelineFailure::kNone,
      /*video_sleep_time_us=*/0,
      /*video_sleep_source_id=*/-1,
      /*video_sleep_on_buffer=*/0,
      /*left_initial_offset=*/0,
      /*right_initial_offset=*/100312679,
      /*start_time=*/0,
      /*pause_before_playing=*/true);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // A common --start-time may span chapter boundaries. Consume complete chapters plus the partial frontier before
  // sequence zero, then expose video and selected audio on a new near-zero epoch so PLAYING does not wait for the
  // skipped wall-clock duration.
  rc = run_lossless_two_camera_mux(
      left_uris,
      right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/0,
      /*expected_failure=*/ExpectedPipelineFailure::kNone,
      /*video_sleep_time_us=*/0,
      /*video_sleep_source_id=*/-1,
      /*video_sleep_on_buffer=*/0,
      /*left_initial_offset=*/0,
      /*right_initial_offset=*/0,
      /*start_time=*/GST_SECOND + 200 * GST_MSECOND,
      /*pause_before_playing=*/true);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Either equal-length camera can report permanent EOS first. If the other camera supplies backpressured audio,
  // terminal coordination must still drain its complete paired timeline before retiring that decoder.
  rc = run_lossless_two_camera_mux(
      left_uris,
      right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/30000);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Camera chapter boundaries do not have to occur on the same frame. The total streams still pair exactly: the
  // shorter right chapter continues in its next file while the left camera finishes its current file.
  rc = run_lossless_two_camera_mux(left_uris, shifted_right_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  const std::vector<std::string> shorter_right_uris{to_file_uri(b0), to_file_uri(b1)};
  // Backpressure the audio-bearing shorter camera so video EOS arrives first. Switching must wait for that camera's
  // trailing audio buffers/EOS instead of tearing down uridecodebin and truncating the audio timeline.
  rc = run_lossless_two_camera_mux(
      left_uris,
      shorter_right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/1,
      /*audio_sleep_time_us=*/30000);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // The ending camera may not be the selected audio source. Backpressure the longer peer too: permanent video
  // exhaustion must wait for that peer's audio through the last retained camera pair instead of stopping its decoder.
  rc = run_lossless_two_camera_mux(
      left_uris,
      shorter_right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/30000);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  const std::vector<std::string> single_right_uri{to_file_uri(b0)};
  // Exercise the production fallback too: URI-MULTIPLE configurations historically omitted uri-list when a camera
  // had only one file. That one-file camera must still participate in every exact-sequence barrier rendezvous.
  rc = run_lossless_two_camera_mux(left_uris, single_right_uri, /*include_right_uri_list=*/false);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Canonical stitched-output fallback uses one URI_MULTIPLE source with stitching disabled. It must bypass the
  // exact two-camera rendezvous while retaining ordinary playlist timestamp/EOS handling.
  rc = run_single_uri_multiple_source(to_file_uri(a0));
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // A decoder that fails while its peer is waiting for the next exact frame must cancel/broadcast the barrier before
  // teardown. Without that safeguard this expected resource error blocks GST_STATE_NULL until the barrier timeout.
  const std::vector<std::string> failing_right_uris{
      to_file_uri(b0), to_file_uri(tmpdir / "deliberately_missing_right_1.mp4")};
  rc = run_lossless_two_camera_mux(
      {to_file_uri(a0), to_file_uri(a1)},
      failing_right_uris,
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/0,
      /*expected_failure=*/ExpectedPipelineFailure::kBusError);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Cancellation must also win before sequence zero commits; otherwise no-pair terminal teardown can synthesize EOS
  // ahead of this decoder error and incorrectly report successful completion.
  rc = run_lossless_two_camera_mux(
      {to_file_uri(a0)},
      {to_file_uri(tmpdir / "deliberately_missing_right_0.mp4")},
      /*include_right_uri_list=*/true,
      /*audio_source_id=*/0,
      /*audio_sleep_time_us=*/0,
      /*expected_failure=*/ExpectedPipelineFailure::kStateChangeFailure);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_file_audio_fanout_to_two_file_sinks(tmpdir, audio);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_file_audio_fanout_to_file_and_rtsp(tmpdir, audio);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_multi_sink_without_ids_disables_audio(tmpdir, audio);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  fs::remove_all(tmpdir);
  return 0;
}
