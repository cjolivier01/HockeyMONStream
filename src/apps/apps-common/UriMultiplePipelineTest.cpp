#include "hstream/src/apps/apps-common/WebRtcEndpoint.h"
#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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
    case GST_MESSAGE_APPLICATION:
      if (state->source_parent && gst_message_has_name(msg, "hstream-uri-playlist-initial-seek-ready")) {
        (void)seek_uri_playlist_initial_positions(state->source_parent);
      }
      break;
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
  guint64 dts_before_pts{0};
  guint64 discontinuities{0};
  GstSegment segment{};
  bool have_segment{false};
  guint64 segment_mismatches{0};
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

struct DecoderSettingsStats {
  guint extra_surfaces = 0;
  gboolean low_latency = FALSE;
  std::atomic<guint> verified{0};
  std::atomic<guint> incorrect{0};
};

void inspect_hardware_decoder(GstBin*, GstBin*, GstElement* element, gpointer data) {
  auto* stats = static_cast<DecoderSettingsStats*>(data);
  GstElementFactory* factory = gst_element_get_factory(element);
  const char* klass = factory ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;
  if (!klass || !strstr(klass, "Decoder") || !strstr(klass, "Video"))
    return;
  if (g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "nvv4l2decoder") != 0) {
    ++stats->incorrect;
    return;
  }
  GstPad* pad = gst_element_get_static_pad(element, "src");
  if (!pad) {
    ++stats->incorrect;
    return;
  }
  gst_pad_add_probe(
      pad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      [](GstPad* pad, GstPadProbeInfo* info, gpointer data) {
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_CAPS)
          return GST_PAD_PROBE_OK;
        auto* stats = static_cast<DecoderSettingsStats*>(data);
        GstElement* decoder = gst_pad_get_parent_element(pad);
        guint extra = G_MAXUINT;
        gboolean latency = !stats->low_latency;
        g_object_get(decoder, "num-extra-surfaces", &extra, "low-latency-mode", &latency, nullptr);
        gst_object_unref(decoder);
        if (extra == stats->extra_surfaces && latency == stats->low_latency)
          ++stats->verified;
        else
          ++stats->incorrect;
        return GST_PAD_PROBE_REMOVE;
      },
      stats,
      nullptr);
  gst_object_unref(pad);
}

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
      GST_EVENT_TYPE(GST_EVENT(info->data)) == GST_EVENT_SEGMENT) {
    const GstSegment* segment = nullptr;
    gst_event_parse_segment(GST_EVENT(info->data), &segment);
    gst_segment_copy_into(segment, &stats->segment);
    stats->have_segment = true;
    return GST_PAD_PROBE_OK;
  }
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
  const GstClockTime dts = GST_BUFFER_DTS(buffer);
  const GstClockTime duration = GST_BUFFER_DURATION(buffer);
  ++stats->buffers;
  if (!GST_CLOCK_TIME_IS_VALID(pts) || !GST_CLOCK_TIME_IS_VALID(duration)) {
    ++stats->invalid_timestamps;
    return GST_PAD_PROBE_OK;
  }
  // The logical playlist starts at zero even after a startup seek. Its segment
  // must agree with its rebased buffers at every physical chapter boundary.
  if (!stats->have_segment || gst_segment_to_running_time(&stats->segment, GST_FORMAT_TIME, pts) != pts) {
    ++stats->segment_mismatches;
  }
  if (GST_CLOCK_TIME_IS_VALID(dts) && dts < pts) {
    ++stats->dts_before_pts;
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
        decoded_sequence->source_id != frame_meta->source_id || decoded_sequence->source_uri == 0 ||
        !GST_CLOCK_TIME_IS_VALID(decoded_sequence->source_pts) ||
        !g_str_has_prefix(g_quark_to_string(decoded_sequence->source_uri), "file://") ||
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

bool make_synthetic_mp4(
    const fs::path& path,
    double seconds,
    int hue_degrees,
    int audio_frequency,
    bool hevc_main10 = false) {
  std::stringstream cmd;
  cmd << "ffmpeg -hide_banner -loglevel error -y "
      << "-f lavfi -i "
      << "'testsrc2=size=256x144:rate=15,format=yuv420p,hue=h=" << hue_degrees << "' ";
  if (audio_frequency > 0) {
    cmd << "-f lavfi -i 'sine=frequency=" << audio_frequency << ":sample_rate=48000' ";
  }
  cmd << "-t " << seconds << " ";
  if (audio_frequency > 0) {
    cmd << "-shortest -c:a aac -b:a 64k ";
  } else {
    cmd << "-an ";
  }
  if (hevc_main10) {
    cmd << "-c:v libx265 -preset ultrafast -pix_fmt yuv420p10le "
        << "-x265-params 'pools=1:frame-threads=1:bframes=0:ref=1:log-level=error' ";
  } else {
    cmd << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p ";
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

int run_file_audio_fanout_to_two_file_sinks(
    const fs::path& tmpdir,
    const fs::path& audio_path,
    bool program_4k = false) {
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
  sink_configs[1].type = program_4k ? NV_DS_SINK_ENCODE_PROGRAM_4K_FILE : NV_DS_SINK_ENCODE_FILE;

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

int run_headless_render_video_sink() {
  const std::vector<NvDsSinkType> render_types = {
#ifndef IS_TEGRA
      NV_DS_SINK_RENDER_EGL,
#else
      NV_DS_SINK_RENDER_3D,
#endif
      NV_DS_SINK_RENDER_DRM,
  };
  for (NvDsSinkType render_type : render_types) {
    NvDsSinkSubBinConfig sink_config{};
    sink_config.enable = TRUE;
    sink_config.source_id = 0;
    sink_config.type = render_type;
    sink_config.sync = TRUE;
    sink_config.render_config.gpu_id = 0;
    sink_config.render_config.qos = TRUE;

    NvDsSinkBin sink_bin{};
    set_embedded_gpu_preview_video_mode(TRUE);
    const bool created = create_sink_bin(1, &sink_config, &sink_bin, 0);
    set_embedded_gpu_preview_video_mode(FALSE);
    if (!created || !sink_bin.sub_bins[0].sink) {
      std::cerr << "Failed to construct a headless render-video sink for type " << render_type << '\n';
      if (sink_bin.bin) {
        gst_object_unref(GST_OBJECT(sink_bin.bin));
      }
      return 2;
    }

    GstElementFactory* factory = gst_element_get_factory(sink_bin.sub_bins[0].sink);
    const gchar* factory_name = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : nullptr;
    gboolean sync = TRUE;
    gboolean qos = TRUE;
    g_object_get(G_OBJECT(sink_bin.sub_bins[0].sink), "sync", &sync, "qos", &qos, NULL);
    if (g_strcmp0(factory_name, "fakesink") != 0 || sync || qos) {
      std::cerr << "Headless render-video type " << render_type << " must use an unsynchronized, non-QoS fakesink; "
                << "factory=" << (factory_name ? factory_name : "(null)") << " sync=" << sync << " qos=" << qos << '\n';
      gst_object_unref(GST_OBJECT(sink_bin.bin));
      return 3;
    }
    gst_object_unref(GST_OBJECT(sink_bin.bin));
    sink_bin = {};
    set_embedded_gpu_preview_video_mode(TRUE, TRUE);
    const bool paced_created = create_sink_bin(1, &sink_config, &sink_bin, 0);
    set_embedded_gpu_preview_video_mode(FALSE);
    if (!paced_created || !sink_bin.sub_bins[0].sink)
      return 4;
    g_object_get(G_OBJECT(sink_bin.sub_bins[0].sink), "sync", &sync, "qos", &qos, NULL);
    gst_object_unref(GST_OBJECT(sink_bin.bin));
    if (!sync || qos) {
      std::cerr << "Calibration-only embedded render must preserve sync without QoS\n";
      return 5;
    }
  }
  return 0;
}

int run_scaled_render_sink_caps() {
  const auto vegas_size = hm::deepstream_sink_internal::fit_render_size_to_aspect(1600, 900, 15287, 6958);
  const auto native_size = hm::deepstream_sink_internal::fit_render_size_to_aspect(1600, 900, 16, 9);
  if (vegas_size.first != 1600 || vegas_size.second != 728 || native_size.first != 1600 || native_size.second != 900) {
    std::cerr << "Scaled render aspect fitting failed; vegas=" << vegas_size.first << 'x' << vegas_size.second
              << " native=" << native_size.first << 'x' << native_size.second << '\n';
    return 1;
  }

#ifndef IS_TEGRA
  const char* previous_render_sink = g_getenv("HM_RENDER_SINK");
  const bool restore_render_sink = previous_render_sink != nullptr;
  const std::string saved_render_sink = restore_render_sink ? previous_render_sink : "";
  g_setenv("HM_RENDER_SINK", "nv3dsink", TRUE);
#endif

  NvDsSinkSubBinConfig sink_config{};
  sink_config.enable = TRUE;
  sink_config.source_id = 0;
#ifndef IS_TEGRA
  sink_config.type = NV_DS_SINK_RENDER_EGL;
#else
  sink_config.type = NV_DS_SINK_RENDER_3D;
#endif
  sink_config.sync = FALSE;
  sink_config.render_config.gpu_id = 0;
  sink_config.render_config.width = 1600;
  sink_config.render_config.height = 900;

  NvDsSinkBin sink_bin{};
  const bool created = create_sink_bin(1, &sink_config, &sink_bin, 0);

#ifndef IS_TEGRA
  if (restore_render_sink) {
    g_setenv("HM_RENDER_SINK", saved_render_sink.c_str(), TRUE);
  } else {
    g_unsetenv("HM_RENDER_SINK");
  }
#endif

  if (!created || !sink_bin.sub_bins[0].cap_filter || !sink_bin.sub_bins[0].transform) {
    std::cerr << "Failed to construct a scaled render sink fixture\n";
    if (sink_bin.bin) {
      gst_object_unref(GST_OBJECT(sink_bin.bin));
    }
    return 2;
  }

  GstCaps* caps = nullptr;
  g_object_get(G_OBJECT(sink_bin.sub_bins[0].cap_filter), "caps", &caps, NULL);
  const GstStructure* structure = caps && gst_caps_get_size(caps) == 1 ? gst_caps_get_structure(caps, 0) : nullptr;
  const GstCapsFeatures* features = caps && gst_caps_get_size(caps) == 1 ? gst_caps_get_features(caps, 0) : nullptr;
  gint width = 0;
  gint height = 0;
  const bool scaled = structure && gst_structure_get_int(structure, "width", &width) &&
      gst_structure_get_int(structure, "height", &height) && width == 1600 && height == 900;
  const bool nvmm = features && gst_caps_features_contains(features, "memory:NVMM");
  if (caps) {
    gst_caps_unref(caps);
  }
  gst_object_unref(GST_OBJECT(sink_bin.bin));
  if (!scaled || !nvmm) {
    std::cerr << "Scaled render sink caps did not carry the requested dimensions; width=" << width
              << " height=" << height << " nvmm=" << nvmm << '\n';
    return 3;
  }
  return 0;
}

// Exercise the actual render-bin policy with a second, unpaced output branch.
// Timestamped system-memory test frames are sufficient for the embedded
// render terminator; production GPU preview/conversion is unchanged.
int run_realtime_render_pacing(bool slow, bool realtime, bool preparing_calibration = false) {
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc num-buffers=24 ! video/x-raw,width=16,height=16,framerate=60/1 ! "
      "tee name=inputs inputs. ! queue ! identity name=compute ! tee name=outputs "
      "inputs. ! queue name=preview_queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
      "fakesink name=preview async=false signal-handoffs=true "
      "outputs. ! queue ! fakesink name=archive sync=false async=false signal-handoffs=true",
      nullptr);
  if (!pipeline)
    return 1;
  NvDsSinkSubBinConfig config{};
  config.enable = TRUE;
  config.type = NV_DS_SINK_RENDER_DRM; // Substituted with the UI's fakesink on every platform.
  config.sync = FALSE; // Explicit playback must override the configured render policy.
  config.render_config.qos = TRUE;
  NvDsSinkBin render{};
  set_embedded_gpu_preview_video_mode(TRUE);
  set_render_playback_realtime(realtime, preparing_calibration);
  const gboolean paced = render_playback_realtime();
  GstElement* preview_queue = gst_bin_get_by_name(GST_BIN(pipeline), "preview_queue");
  g_object_set(preview_queue, "leaky", paced ? 0 : 2, nullptr);
  gst_object_unref(preview_queue);
  GstElement* preview = gst_bin_get_by_name(GST_BIN(pipeline), "preview");
  g_object_set(preview, "sync", paced, "max-lateness", gint64{-1}, "qos", FALSE, nullptr);
  const bool created = create_sink_bin(1, &config, &render, 0);
  set_render_playback_realtime(FALSE);
  set_embedded_gpu_preview_video_mode(FALSE);
  if (!created) {
    gst_object_unref(preview);
    gst_object_unref(pipeline);
    return 2;
  }
  gst_bin_add(GST_BIN(pipeline), render.bin);
  GstElement* outputs = gst_bin_get_by_name(GST_BIN(pipeline), "outputs");
  const bool linked = gst_element_link(outputs, render.bin);
  gst_object_unref(outputs);
  if (!linked) {
    gst_object_unref(preview);
    gst_object_unref(pipeline);
    return 3;
  }
  GstElement* compute = gst_bin_get_by_name(GST_BIN(pipeline), "compute");
  g_object_set(compute, "sleep-time", slow ? 25000U : 0U, nullptr);
  // Calibration needs several inputs before producing any output. A clocked
  // output requiring preroll plus a bounded upstream preview deadlocks here.
  guint calibration_inputs = 0;
  if (preparing_calibration) {
    GstPad* pad = gst_element_get_static_pad(compute, "src");
    gst_pad_add_probe(
        pad,
        GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo*, gpointer data) -> GstPadProbeReturn {
          return ++*static_cast<guint*>(data) <= 9 ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
        },
        &calibration_inputs,
        nullptr);
    gst_object_unref(pad);
  }
  gst_object_unref(compute);
  std::vector<GstClockTime> presented;
  std::vector<GstClockTime> archived;
  std::vector<GstClockTime> previewed;
  const auto record = +[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
    static_cast<std::vector<GstClockTime>*>(data)->push_back(GST_BUFFER_PTS(buffer));
  };
  g_object_set(render.sub_bins[0].sink, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(render.sub_bins[0].sink, "handoff", G_CALLBACK(record), &presented);
  g_signal_connect(preview, "handoff", G_CALLBACK(record), &previewed);
  gst_object_unref(preview);
  GstElement* archive = gst_bin_get_by_name(GST_BIN(pipeline), "archive");
  g_signal_connect(archive, "handoff", G_CALLBACK(record), &archived);
  gst_object_unref(archive);
  const auto start = std::chrono::steady_clock::now();
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
      gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS) {
    std::cerr << "Render/preview preroll stalled; calibration=" << preparing_calibration << '\n';
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 7;
  }
  const int rc = run_pipeline(pipeline, 10);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (rc)
    return rc;
  const guint skipped = preparing_calibration ? 9 : 0;
  if (presented.size() != 24 - skipped || archived != presented || (paced && previewed != presented)) {
    std::cerr << "Paced render must preserve every frame and the archive timeline\n";
    return 4;
  }
  for (guint i = 0; i < archived.size(); ++i) {
    if (archived[i] != gst_util_uint64_scale(i + skipped, GST_SECOND, 60))
      return 5;
  }
  if (paced && elapsed < std::chrono::milliseconds(380)) {
    std::cerr << "Clock-paced 60 fps playback finished too early\n";
    return 6;
  }
  return 0;
}

int run_render_audio_initial_mute(
    const fs::path& audio_path,
    bool initially_muted,
    bool realtime = false,
    bool preparing_calibration = false) {
  GstElement* pipeline =
      gst_pipeline_new(initially_muted ? "hmaudio-muted-render-test" : "hmaudio-audible-render-test");
  GstElement* render_sink_marker = gst_element_factory_make("fakesink", nullptr);
  if (!pipeline || !render_sink_marker) {
    std::cerr << "Failed to create render-audio mute test fixtures\n";
    if (pipeline) {
      gst_object_unref(GST_OBJECT(pipeline));
    }
    if (render_sink_marker) {
      gst_object_unref(GST_OBJECT(render_sink_marker));
    }
    return 2;
  }

  NvDsHmAudioConfig audio_cfg{};
  configure_file_hmaudio(audio_cfg, audio_path);

  NvDsSinkSubBinConfig sink_configs[MAX_SINK_BINS]{};
  sink_configs[0].enable = TRUE;
  sink_configs[0].sink_id = 0;
#ifndef IS_TEGRA
  sink_configs[0].type = NV_DS_SINK_RENDER_EGL;
#else
  sink_configs[0].type = NV_DS_SINK_RENDER_3D;
#endif

  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].sink = render_sink_marker;
  NvDsSrcBin src_bins[MAX_SOURCE_BINS]{};
  NvDsHmAudioBin audio_bin{};
  g_setenv("HSTREAM_RENDER_AUDIO_MUTED", initially_muted ? "1" : "0", TRUE);
  set_render_playback_realtime(realtime, preparing_calibration);
  const gboolean paced = render_playback_realtime();
  const bool created = create_hmaudio_bin(GST_BIN(pipeline), &audio_cfg, &audio_bin, src_bins, sink_configs, &sink_bin);
  set_render_playback_realtime(FALSE);
  g_unsetenv("HSTREAM_RENDER_AUDIO_MUTED");
  gst_object_unref(GST_OBJECT(render_sink_marker));
  if (!created || !audio_bin.bin) {
    std::cerr << "Failed to construct the local render-audio branch\n";
    gst_object_unref(GST_OBJECT(pipeline));
    return 3;
  }

  GstElement* volume = gst_bin_get_by_name(GST_BIN(audio_bin.bin), "hmaudio_render_sink0_volume");
  GstElement* audiosink = gst_bin_get_by_name(GST_BIN(audio_bin.bin), "hmaudio_render_sink0_alsasink");
  if (!volume || !audiosink) {
    std::cerr << "Expected dedicated volume and ALSA elements in the local render-audio branch\n";
    if (volume) {
      gst_object_unref(GST_OBJECT(volume));
    }
    if (audiosink) {
      gst_object_unref(GST_OBJECT(audiosink));
    }
    gst_object_unref(GST_OBJECT(pipeline));
    return 4;
  }
  gboolean actual_muted = FALSE;
  gboolean audio_sync = TRUE;
  gboolean audio_async = TRUE;
  gboolean audio_qos = TRUE;
  gboolean provides_clock = TRUE;
  gint64 max_lateness = 0;
  g_object_get(G_OBJECT(volume), "mute", &actual_muted, NULL);
  g_object_get(
      G_OBJECT(audiosink),
      "sync",
      &audio_sync,
      "async",
      &audio_async,
      "qos",
      &audio_qos,
      "provide-clock",
      &provides_clock,
      "max-lateness",
      &max_lateness,
      NULL);
  GstElement* queue = gst_bin_get_by_name(GST_BIN(audio_bin.bin), "hmaudio_branch_sink0_queue");
  gint leaky = -1;
  if (queue) {
    g_object_get(queue, "leaky", &leaky, nullptr);
    gst_object_unref(queue);
  }
  gst_object_unref(GST_OBJECT(volume));
  gst_object_unref(GST_OBJECT(audiosink));
  gst_object_unref(GST_OBJECT(pipeline));
  if (actual_muted != static_cast<gboolean>(initially_muted)) {
    std::cerr << "Expected initial local render-audio mute=" << initially_muted << ", got " << actual_muted << '\n';
    return 5;
  }
  if (audio_sync != paced || audio_async || audio_qos || max_lateness != -1 || leaky != (paced ? 0 : 2) ||
      (paced && provides_clock)) {
    std::cerr << "Unexpected local audio timing policy; realtime=" << realtime << " sync=" << audio_sync
              << ", async=" << audio_async << " leaky=" << leaky << " provides-clock=" << provides_clock << '\n';
    return 6;
  }
  return 0;
}

int run_webrtc_audio_queue(bool realtime, bool calibration, bool private_preview, bool pressure = false) {
  // Use the production builder and upstream queue. The source is CPU PCM and
  // the endpoint drains Opus locally: no GPU, physical audio or signaling.
  auto* pipeline = gst_pipeline_new("webrtc-audio-queue-test");
  auto* endpoint = hstream::webrtc_endpoint_create("test-endpoint", nullptr);
  auto* source_bin = gst_bin_new("test-source");
  auto* source = gst_element_factory_make("appsrc", "source-pcm");
  auto* source_tee = gst_element_factory_make("tee", "source-audio-tee");
  auto* archive_queue = gst_element_factory_make("queue", "archive-queue");
  auto* archive_sink = gst_element_factory_make("fakesink", "archive-output");
  if (!pipeline || !endpoint || !source_bin || !source || !source_tee || !archive_queue || !archive_sink)
    return 2;
  gst_bin_add_many(GST_BIN(pipeline), source_bin, endpoint, nullptr);
  gst_bin_add_many(GST_BIN(source_bin), source, source_tee, archive_queue, archive_sink, nullptr);
  if (!gst_element_link(source, source_tee) ||
      !gst_element_link_many(source_tee, archive_queue, archive_sink, nullptr)) {
    gst_object_unref(pipeline);
    return 2;
  }
  g_object_set(archive_sink, "sync", FALSE, "async", FALSE, nullptr);
  g_object_set(source, "format", GST_FORMAT_TIME, "block", TRUE, "max-bytes", guint64{8192}, nullptr);
  auto* caps = gst_caps_from_string("audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2");
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  NvDsSourceConfig source_config{};
  source_config.type = NV_DS_SOURCE_URI_MULTIPLE;
  NvDsSrcBin sources[MAX_SOURCE_BINS]{};
  sources[0].source_id = 0;
  sources[0].config = &source_config;
  sources[0].bin = source_bin;
  sources[0].src_elem = source;
  sources[0].uri_audio_tee = source_tee;
  NvDsHmAudioConfig audio_config{};
  audio_config.enable = TRUE;
  audio_config.src = SRC_SOURCE_BIN;
  audio_config.source_id = 0;
  audio_config.dest = DEST_SINK;
  audio_config.sink_id = 0;
  NvDsSinkSubBinConfig sinks[MAX_SINK_BINS]{};
  sinks[0].enable = TRUE;
  sinks[0].sink_id = 0;
  sinks[0].type = NV_DS_SINK_WEBRTC;
  sinks[0].encoder_config.preview_endpoint = private_preview ? endpoint : nullptr;
  NvDsSinkBin sink_bin{};
  sink_bin.sub_bins[0].sink = endpoint;
  NvDsHmAudioBin audio_bin{};
  set_render_playback_realtime(realtime, calibration);
  const bool paced = private_preview && render_playback_realtime();
  const bool created = create_hmaudio_bin(GST_BIN(pipeline), &audio_config, &audio_bin, sources, sinks, &sink_bin);
  set_render_playback_realtime(FALSE);
  if (!created || !audio_bin.bin) {
    gst_object_unref(pipeline);
    return 3;
  }
  auto* queue = gst_bin_get_by_name(GST_BIN(audio_bin.bin), "hmaudio_branch_sink0_queue");
  auto* encoder = gst_bin_get_by_name(GST_BIN(audio_bin.bin), "hmaudio_webrtc_sink0_opusenc");
  if (!queue || !encoder) {
    if (queue)
      gst_object_unref(queue);
    if (encoder)
      gst_object_unref(encoder);
    gst_object_unref(pipeline);
    return 3;
  }
  gint leaky = -1;
  guint buffers = 0, bytes = 0;
  guint64 time = 0;
  g_object_get(
      queue, "leaky", &leaky, "max-size-buffers", &buffers, "max-size-bytes", &bytes, "max-size-time", &time, nullptr);
  bool ok = leaky == (paced ? 0 : 2) && buffers == 30 && bytes == 0 && time == 0;
  if (!ok)
    std::cerr << "WebRTC audio queue policy: realtime=" << realtime << " calibration=" << calibration
              << " private=" << private_preview << " leaky=" << leaky << " capacity=" << buffers << '\n';
  if (pressure) {
    struct State {
      std::mutex mutex;
      std::condition_variable changed;
      bool held{false}, release{false}, full{false}, valid{true};
      std::vector<guint8> transport, archive;
      std::vector<GstClockTime> transport_pts, archive_pts, transport_duration, archive_duration;
    } state;
    struct Capture {
      State* state;
      bool transport;
    } transport{&state, true}, archive{&state, false};
    auto capture = +[](GstPad*, GstPadProbeInfo* info, gpointer data) {
      auto& capture = *static_cast<Capture*>(data);
      auto& state = *capture.state;
      auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      std::unique_lock<std::mutex> lock(state.mutex);
      if (capture.transport && !state.held) {
        state.held = true;
        state.changed.notify_all();
        state.changed.wait(lock, [&] { return state.release; });
      }
      GstMapInfo mapped{};
      if (!gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
        state.valid = false;
        return GST_PAD_PROBE_OK;
      }
      auto& contents = capture.transport ? state.transport : state.archive;
      contents.insert(contents.end(), mapped.data, mapped.data + mapped.size);
      (capture.transport ? state.transport_pts : state.archive_pts).push_back(GST_BUFFER_PTS(buffer));
      (capture.transport ? state.transport_duration : state.archive_duration).push_back(GST_BUFFER_DURATION(buffer));
      gst_buffer_unmap(buffer, &mapped);
      return GST_PAD_PROBE_OK;
    };
    auto* raw_pad = gst_element_get_static_pad(encoder, "sink");
    auto* archive_pad = gst_element_get_static_pad(archive_sink, "sink");
    gst_pad_add_probe(raw_pad, GST_PAD_PROBE_TYPE_BUFFER, capture, &transport, nullptr);
    gst_pad_add_probe(archive_pad, GST_PAD_PROBE_TYPE_BUFFER, capture, &archive, nullptr);
    g_signal_connect(
        queue,
        "overrun",
        G_CALLBACK(+[](GstElement*, gpointer data) {
          auto& state = *static_cast<State*>(data);
          std::lock_guard<std::mutex> lock(state.mutex);
          state.full = true;
          state.changed.notify_all();
        }),
        &state);
    constexpr guint kBuffers = 200, kFrames = 1024;
    std::vector<guint8> expected;
    std::vector<GstClockTime> expected_pts, expected_duration;
    for (guint frame = 0; frame < kBuffers * kFrames; ++frame) {
      const guint16 left = frame % 30000 + 1, right = frame / 30000 + 1;
      expected.insert(expected.end(), {guint8(left), guint8(left >> 8), guint8(right), guint8(right >> 8)});
    }
    for (guint i = 0; i < kBuffers; ++i) {
      const auto start = gst_util_uint64_scale(i * kFrames, GST_SECOND, 48000);
      expected_pts.push_back(start);
      expected_duration.push_back(gst_util_uint64_scale((i + 1) * kFrames, GST_SECOND, 48000) - start);
    }
    ok &= gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    std::atomic<bool> pushed{true};
    std::thread sender([&] {
      for (guint i = 0; i < kBuffers; ++i) {
        auto* buffer = gst_buffer_new_allocate(nullptr, kFrames * 4, nullptr);
        gst_buffer_fill(buffer, 0, expected.data() + i * kFrames * 4, kFrames * 4);
        GST_BUFFER_PTS(buffer) = expected_pts[i];
        GST_BUFFER_DURATION(buffer) = expected_duration[i];
        if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK) {
          pushed.store(false);
          return;
        }
      }
      gst_app_src_end_of_stream(GST_APP_SRC(source));
    });
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      const bool pressure_reached =
          state.changed.wait_for(lock, std::chrono::seconds(2), [&] { return state.held && state.full; });
      ok &= pressure_reached;
      if (!pressure_reached)
        std::cerr << "Production WebRTC branch did not reach sustained bounded queue pressure\n";
    }
    // Hold the mapper boundary after the queue fills. The old leaky policy
    // lets the same producer continue discarding PCM during this interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.release = true;
      state.changed.notify_all();
    }
    auto* bus = gst_element_get_bus(pipeline);
    auto* message =
        gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    ok &= message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
      gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    sender.join();
    ok &= pushed.load() && state.valid && state.transport == expected && state.archive == expected &&
        state.transport_pts == expected_pts && state.archive_pts == expected_pts &&
        state.transport_duration == expected_duration && state.archive_duration == expected_duration;
    std::cout << "WebRTC queue pressure: source-bytes=" << expected.size()
              << " transport-bytes=" << state.transport.size() << " archive-bytes=" << state.archive.size()
              << " transport-packets=" << state.transport_pts.size() << " capacity=" << buffers << '\n';
    gst_object_unref(raw_pad);
    gst_object_unref(archive_pad);
  }
  gst_object_unref(queue);
  gst_object_unref(encoder);
  gst_object_unref(pipeline);
  return ok ? 0 : 4;
}

int run_decode_compose_encode(
    const fs::path& tmpdir,
    const std::vector<std::string>& left_uris,
    const std::vector<std::string>& right_uris,
    GstClockTime start_time = 0,
    guint64 expected_frames = 45) {
  NvDsSourceConfig configs[2]{};
  configure_uri_multiple_source(configs[0], left_uris, /*source_id=*/0);
  configure_uri_multiple_source(configs[1], right_uris, /*source_id=*/1);

  NvDsSrcParentBin src_parent{};
  if (!create_multi_source_bin(2, configs, &src_parent)) {
    std::cerr << "Failed to create multi-source URI-MULTIPLE bin\n";
    return 2;
  }
  if (start_time) {
    // Rejected positions must explain the exhausted camera and leave startup
    // pristine so a corrected position can still be admitted below.
    for (guint source_id = 0; source_id < 2; ++source_id) {
      std::string error;
      const bool accepted = configure_uri_playlist_initial_offsets(
          &src_parent, 0, source_id == 1 ? 86400 * GST_SECOND : 0, 0, source_id == 0 ? 86400 * GST_SECOND : 0, &error);
      if (accepted || error.find("camera " + std::to_string(source_id + 1)) == std::string::npos ||
          error.find("recording's duration") == std::string::npos || error.find("HH:MM:SS") == std::string::npos ||
          src_parent.uri_playlist_initial_offsets_configured || src_parent.sub_bins[0].uri_list_index != 0 ||
          src_parent.sub_bins[1].uri_list_index != 0) {
        std::cerr << "Out-of-range startup failed to preserve source state or report its cause: " << error << '\n';
        return 2;
      }
    }
  }
  if (start_time && !configure_uri_playlist_initial_offsets(&src_parent, 0, 0, 0, start_time)) {
    std::cerr << "Failed to configure initial position for encoded playlist\n";
    return 2;
  }
  if (start_time && !arm_uri_playlist_initial_seeks(&src_parent)) {
    std::cerr << "Failed to arm initial chapter seek for encoded playlist\n";
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
    std::cerr << "Encode transition stalled: composed_frames=" << counter.buffers
              << " switches=" << src_parent.sub_bins[0].uri_switch_count << ','
              << src_parent.sub_bins[1].uri_switch_count << '\n';
    return rc;
  }
  const guint expected_switches = static_cast<guint>(left_uris.size() - 1);
  if (src_parent.sub_bins[0].uri_switch_count != expected_switches ||
      src_parent.sub_bins[1].uri_switch_count != expected_switches) {
    std::cerr << "Expected " << expected_switches << " URI-MULTIPLE switches per source, got "
              << src_parent.sub_bins[0].uri_switch_count << " and " << src_parent.sub_bins[1].uri_switch_count << "\n";
    return 7;
  }
  if (counter.buffers != expected_frames) {
    std::cerr << "Expected " << expected_frames << " composed URI-MULTIPLE frames, got " << counter.buffers << '\n';
    return 7;
  }
  const double minimum_end = static_cast<double>(expected_frames) / 15.0 - 0.1;
  rc = expect_encoded_file(out, /*expect_audio=*/true, minimum_end, minimum_end);
  if (rc != 0 || !start_time) {
    return rc;
  }
  // A final audio timestamp alone can conceal a hole at the chapter switch.
  // Decode the output and require all expected 48 kHz samples, allowing a
  // small bounded difference for AAC encoder priming and frame rounding.
  const std::string command =
      "ffprobe -v error -select_streams a:0 -show_frames -show_entries frame=nb_samples -of csv=p=0 " +
      shell_quote(out);
  FILE* audio_frames = popen(command.c_str(), "r");
  if (!audio_frames) {
    return 8;
  }
  guint64 samples = 0;
  unsigned packet_samples = 0;
  while (fscanf(audio_frames, "%u", &packet_samples) == 1) {
    samples += packet_samples;
  }
  const int probe_status = pclose(audio_frames);
  const guint64 expected_samples = expected_frames * 48000 / 15;
  constexpr guint64 kAacSampleTolerance = 4096;
  if (probe_status != 0 || samples + kAacSampleTolerance < expected_samples ||
      samples > expected_samples + kAacSampleTolerance) {
    std::cerr << "Encoded chapter audio contains " << samples << " samples; expected " << expected_samples << '\n';
    return 8;
  }
  return 0;
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
    bool pause_before_playing = false,
    guint64 expected_total_frames = 0,
    guint extra_surfaces = 0,
    gboolean low_latency = FALSE,
    bool verify_decoder_settings = false) {
  NvDsSourceConfig configs[2]{};
  configure_uri_multiple_source(configs[0], left_uris, /*source_id=*/0);
  configure_uri_multiple_source(configs[1], right_uris, /*source_id=*/1, include_right_uri_list);
  for (auto& config : configs) {
    config.num_extra_surfaces = extra_surfaces;
    config.low_latency_mode = low_latency;
  }

  NvDsSrcParentBin src_parent{};
  if (!create_multi_source_bin(2, configs, &src_parent)) {
    std::cerr << "Failed to create lossless two-camera source bin\n";
    return 2;
  }
  DecoderSettingsStats decoder_stats[2];
  for (guint source_id = 0; source_id < 2; ++source_id) {
    decoder_stats[source_id].extra_surfaces = extra_surfaces;
    decoder_stats[source_id].low_latency = low_latency;
    if (verify_decoder_settings) {
      g_signal_connect(
          src_parent.sub_bins[source_id].src_elem,
          "deep-element-added",
          G_CALLBACK(inspect_hardware_decoder),
          &decoder_stats[source_id]);
    }
  }
  if ((left_initial_offset || right_initial_offset || start_time) &&
      !configure_uri_playlist_initial_offsets(
          &src_parent, left_initial_offset, right_initial_offset, audio_source_id, start_time)) {
    std::cerr << "Failed to configure initial camera positions before preroll\n";
    return 2;
  }
  if (start_time >= GST_SECOND &&
      (src_parent.sub_bins[0].uri_playlist_initial_uri_index == 0 ||
       src_parent.sub_bins[1].uri_playlist_initial_uri_index == 0 ||
       src_parent.sub_bins[0].uri_playlist_initial_skipped_base_ns == 0 ||
       src_parent.sub_bins[1].uri_playlist_initial_skipped_base_ns == 0 ||
       src_parent.sub_bins[0].uri_list_decoded_frame_count != 0 ||
       src_parent.sub_bins[1].uri_list_decoded_frame_count != 0)) {
    std::cerr << "Large initial positioning did not preselect complete chapters before decoding\n";
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
  // This observer consumes batches. Retaining the last borrowed decoder frame
  // until the next batch would create a cycle with chapter retirement.
  g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);
  g_object_set(G_OBJECT(audio_sink), "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);
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
  if (verify_decoder_settings) {
    for (guint source_id = 0; source_id < 2; ++source_id) {
      const guint expected = source_id == 0 ? left_uris.size() : right_uris.size();
      if (decoder_stats[source_id].verified != expected || decoder_stats[source_id].incorrect != 0) {
        std::cerr << "Decoder settings not exercised for source " << source_id
                  << ": verified=" << decoder_stats[source_id].verified
                  << " incorrect=" << decoder_stats[source_id].incorrect << " expected=" << expected << '\n';
        return 11;
      }
    }
  }
  const size_t expected_complete_chapters = std::min(left_uris.size(), right_uris.size());
  if (expected_total_frames > 0) {
    const guint expected_left_switches = static_cast<guint>(left_uris.size() - 1);
    const guint expected_right_switches = static_cast<guint>(right_uris.size() - 1);
    if (src_parent.sub_bins[0].uri_switch_count != expected_left_switches ||
        src_parent.sub_bins[1].uri_switch_count != expected_right_switches) {
      std::cerr << "Uneven physical chapters did not all switch before terminal pairing: switches="
                << src_parent.sub_bins[0].uri_switch_count << "," << src_parent.sub_bins[1].uri_switch_count
                << " expected=" << expected_left_switches << "," << expected_right_switches << "\n";
      return 5;
    }
  } else {
    const size_t expected_decoded_chapters = std::min(
        left_uris.size() - src_parent.sub_bins[0].uri_playlist_initial_uri_index,
        right_uris.size() - src_parent.sub_bins[1].uri_playlist_initial_uri_index);
    const guint minimum_switches = static_cast<guint>(expected_decoded_chapters - 1);
    const guint maximum_left_switches = std::min<guint>(
        left_uris.size() - src_parent.sub_bins[0].uri_playlist_initial_uri_index - 1,
        static_cast<guint>(expected_decoded_chapters));
    const guint maximum_right_switches = std::min<guint>(
        right_uris.size() - src_parent.sub_bins[1].uri_playlist_initial_uri_index - 1,
        static_cast<guint>(expected_decoded_chapters));
    if (src_parent.sub_bins[0].uri_switch_count < minimum_switches ||
        src_parent.sub_bins[0].uri_switch_count > maximum_left_switches ||
        src_parent.sub_bins[1].uri_switch_count < minimum_switches ||
        src_parent.sub_bins[1].uri_switch_count > maximum_right_switches) {
      std::cerr << "Unexpected URI switch counts before terminal pairing: " << src_parent.sub_bins[0].uri_switch_count
                << " and " << src_parent.sub_bins[1].uri_switch_count << "\n";
      return 5;
    }
  }

  constexpr guint64 kFramesPerChapter = 15;
  const guint64 preselected_left_frames = src_parent.sub_bins[0].uri_playlist_initial_skipped_base_ns * 15 / GST_SECOND;
  const guint64 preselected_right_frames =
      src_parent.sub_bins[1].uri_playlist_initial_skipped_base_ns * 15 / GST_SECOND;
  const guint64 positioned_frames = std::max(
      preselected_left_frames + src_parent.sub_bins[0].uri_list_initial_positioned_frame_count,
      preselected_right_frames + src_parent.sub_bins[1].uri_list_initial_positioned_frame_count);
  const guint64 total_frames =
      expected_total_frames > 0 ? expected_total_frames : kFramesPerChapter * expected_complete_chapters;
  if (positioned_frames >= total_frames) {
    std::cerr << "Initial positioning consumed the entire expected paired timeline\n";
    return 6;
  }
  const guint64 expected_frames_per_source = total_frames - positioned_frames;
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
  const guint64 expected_left_positioned_frames =
      ((start_time + left_initial_offset) * 15) / GST_SECOND - preselected_left_frames;
  const guint64 expected_right_positioned_frames =
      ((start_time + right_initial_offset) * 15) / GST_SECOND - preselected_right_frames;
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
    if (verify_decoder_settings &&
        (video_timeline_stats[source_id].invalid_timestamps != 0 ||
         video_timeline_stats[source_id].discontinuities != 0)) {
      std::cerr << "Decoder matrix video timeline failed for source " << source_id
                << ": invalid=" << video_timeline_stats[source_id].invalid_timestamps
                << ", discontinuities=" << video_timeline_stats[source_id].discontinuities << '\n';
      return 7;
    }
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
      audio_stats.eos_events != 1 || audio_stats.invalid_timestamps != 0 || audio_stats.dts_before_pts != 0 ||
      audio_stats.discontinuities != 0 ||
      ((left_initial_offset || right_initial_offset || start_time) && audio_stats.segment_mismatches != 0) ||
      audio_stats.first_pts > GST_SECOND / 10 || audio_stats.final_end < minimum_audio_duration ||
      audio_stats.final_end > maximum_audio_duration || audio_stats.final_end + GST_SECOND / 50 < paired_video_end) {
    std::cerr << "Source audio was not continuous through the coordinated camera playlist: buffers="
              << audio_stats.buffers << ", eos_events=" << audio_stats.eos_events
              << ", invalid_timestamps=" << audio_stats.invalid_timestamps
              << ", dts_before_pts=" << audio_stats.dts_before_pts
              << ", discontinuities=" << audio_stats.discontinuities
              << ", segment_mismatches=" << audio_stats.segment_mismatches
              << ", first_pts=" << GST_TIME_AS_SECONDS(audio_stats.first_pts)
              << "s, final_end=" << GST_TIME_AS_SECONDS(audio_stats.final_end)
              << "s, paired_video_end=" << GST_TIME_AS_SECONDS(paired_video_end)
              << "s, expected=" << static_cast<double>(expected_frames_per_source) / 15.0 << "s\n";
    return 9;
  }
  return 0;
}

int run_realtime_render_tests() {
  if (const int rc = run_realtime_render_pacing(false, true, true))
    return rc;
  if (const int rc = run_render_audio_initial_mute("/tmp/unused-render-audio-test.wav", false, true, true))
    return rc;
  for (bool realtime : {false, true}) {
    for (bool slow : {false, true}) {
      const int rc = run_realtime_render_pacing(slow, realtime);
      if (rc)
        return rc;
    }
    const int rc = run_render_audio_initial_mute("/tmp/unused-render-audio-test.wav", false, realtime);
    if (rc)
      return rc;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  g_setenv("USE_NEW_NVSTREAMMUX", "yes", TRUE);
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  if (argc == 2 && std::string(argv[1]) == "--webrtc-audio-pressure-only")
    return run_webrtc_audio_queue(true, false, true, true);
  if (argc == 2 && std::string(argv[1]) == "--webrtc-audio-queue-only") {
    for (bool realtime : {false, true})
      for (bool calibration : {false, true})
        for (bool private_preview : {false, true})
          if (const int rc = run_webrtc_audio_queue(realtime, calibration, private_preview))
            return rc;
    return run_webrtc_audio_queue(true, false, true, true);
  }
  if (argc == 2 && std::string(argv[1]) == "--render-video-sink-only")
    return run_headless_render_video_sink();
  const bool decoder_matrix_only = argc == 2 && std::string(argv[1]) == "--decoder-matrix-only";
  const bool startup_seek_only = argc == 2 && std::string(argv[1]) == "--startup-seek-only";
  if (!decoder_matrix_only && !startup_seek_only) {
    if (const int rc = run_realtime_render_tests())
      return rc;
  }
  if (argc == 2 && std::string(argv[1]) == "--render-realtime-only")
    return 0;

  const fs::path tmpdir = fs::temp_directory_path() / ("uri_multiple_pipeline_test_" + std::to_string(::getpid()));
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);

  const fs::path a0 = tmpdir / "left_0.mp4";
  const fs::path a1 = tmpdir / "left_1.mp4";
  const fs::path a2 = tmpdir / "left_2.mp4";
  const fs::path a_long = tmpdir / "left_two_seconds.mp4";
  const fs::path seek_chapter = tmpdir / "seek_six_seconds.mp4";
  const fs::path b0 = tmpdir / "right_0.mp4";
  const fs::path b1 = tmpdir / "right_1.mp4";
  const fs::path b2 = tmpdir / "right_2.mp4";
  const fs::path shifted_b0 = tmpdir / "right_shifted_0.mp4";
  const fs::path shifted_b1 = tmpdir / "right_shifted_1.mp4";
  const fs::path shifted_b2 = tmpdir / "right_shifted_2.mp4";
  const fs::path audio = tmpdir / "continuous_audio.m4a";
  if (!make_synthetic_mp4(a0, 1, 0, 440) || !make_synthetic_mp4(a1, 1, 30, 494) ||
      !make_synthetic_mp4(a2, 1, 60, 523) || !make_synthetic_mp4(a_long, 2, 75, 554) ||
      !make_synthetic_mp4(b0, 1, 90, 587) || !make_synthetic_mp4(b1, 1, 120, 659) ||
      !make_synthetic_mp4(b2, 1, 150, 698) || !make_synthetic_mp4(shifted_b0, 0.8, 180, 740) ||
      !make_synthetic_mp4(shifted_b1, 1.2, 210, 784) || !make_synthetic_mp4(shifted_b2, 1.0, 240, 831) ||
      !make_synthetic_audio(audio, 3) || !make_synthetic_mp4(seek_chapter, 6, 45, 440)) {
    std::cerr << "Failed to generate synthetic mp4 chapters with ffmpeg\n";
    fs::remove_all(tmpdir);
    return 2;
  }

  const std::vector<std::string> left_uris{to_file_uri(a0), to_file_uri(a1), to_file_uri(a2)};
  const std::vector<std::string> right_uris{to_file_uri(b0), to_file_uri(b1), to_file_uri(b2)};
  const std::vector<std::string> shifted_right_uris{
      to_file_uri(shifted_b0), to_file_uri(shifted_b1), to_file_uri(shifted_b2)};

  std::vector<std::string> main10, shifted_main10;
  for (unsigned chapter = 0; chapter < 3; ++chapter) {
    const fs::path path = tmpdir / ("main10_" + std::to_string(chapter) + ".mp4");
    if (!make_synthetic_mp4(path, 1, chapter * 60, 440, true)) {
      fs::remove_all(tmpdir);
      return 2;
    }
    main10.push_back(to_file_uri(path));
    const fs::path shifted_path = tmpdir / ("main10_shifted_" + std::to_string(chapter) + ".mp4");
    const double duration = chapter == 0 ? 0.8 : (chapter == 1 ? 1.2 : 1.0);
    if (!make_synthetic_mp4(shifted_path, duration, chapter * 60 + 30, 494, true)) {
      fs::remove_all(tmpdir);
      return 2;
    }
    shifted_main10.push_back(to_file_uri(shifted_path));
  }
  // Decoder allocation/retention changes must preserve complete, exact pairs
  // across both aligned and staggered chapter boundaries. Small synthetic
  // inputs qualify ownership/counts; the full-canvas memory regression is in
  // scripts/test_uri_playlist_memory.py.
  if (!startup_seek_only) {
    for (const bool main10_codec : {false, true}) {
      for (const guint extra_surfaces : {0U, 1U, 2U}) {
        for (const bool low_latency : {false, true}) {
          std::cout << "Decoder matrix: main10=" << main10_codec << " extra=" << extra_surfaces
                    << " low-latency=" << low_latency << std::endl;
          const int matrix_rc = run_lossless_two_camera_mux(
              main10_codec ? main10 : left_uris,
              main10_codec ? shifted_main10 : shifted_right_uris,
              true,
              0,
              0,
              ExpectedPipelineFailure::kNone,
              0,
              -1,
              0,
              0,
              0,
              0,
              false,
              45,
              extra_surfaces,
              low_latency,
              true);
          if (matrix_rc != 0) {
            fs::remove_all(tmpdir);
            return matrix_rc;
          }
        }
      }
    }
  }
  if (decoder_matrix_only) {
    fs::remove_all(tmpdir);
    return 0;
  }

  // The startup seek leaves three seconds in chapter one. The next chapter
  // must not introduce a three-second audio hole that blocks the archive mux.
  const std::vector<std::string> seek_uris{to_file_uri(seek_chapter), to_file_uri(seek_chapter)};
  int rc = run_decode_compose_encode(tmpdir, seek_uris, seek_uris, 3 * GST_SECOND, 135);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }
  rc = run_decode_compose_encode(tmpdir, left_uris, right_uris);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  // Exercise the real raw-audio conversion/encoding branch after trimming the
  // first chapter. Counting buffers before a fakesink alone misses segment
  // clipping that can discard all subsequent chapter audio downstream.
  rc = run_decode_compose_encode(tmpdir, left_uris, right_uris, 400 * GST_MSECOND, 39);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  if (startup_seek_only) {
    fs::remove_all(tmpdir);
    return 0;
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

  // Physical chapter files are independent container boundaries, not frame-pair boundaries. Exercise the same
  // three-second, 45-frame camera timeline partitioned as two files on the left and three on the right. Every file
  // must switch, while every decoded sequence still forms exactly one complete two-camera mux batch.
  rc = run_lossless_two_camera_mux(
      {to_file_uri(a_long), to_file_uri(a2)},
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
      /*start_time=*/0,
      /*pause_before_playing=*/false,
      /*expected_total_frames=*/45);
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

  rc = run_file_audio_fanout_to_two_file_sinks(tmpdir, audio, true);
  if (rc != 0)
    return rc;
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

  rc = run_headless_render_video_sink();
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_scaled_render_sink_caps();
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  rc = run_render_audio_initial_mute(audio, true);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }
  rc = run_render_audio_initial_mute(audio, false);
  if (rc != 0) {
    fs::remove_all(tmpdir);
    return rc;
  }

  fs::remove_all(tmpdir);
  return 0;
}
