#pragma once

#include <gst/gst.h>
#include <sys/time.h>

#include "deepstream_config.h"
#include "deepstream_dewarper.h"

#include <optional>
#include <string>

typedef enum {
  NV_DS_SOURCE_CAMERA_V4L2 = 1,
  NV_DS_SOURCE_URI,
  NV_DS_SOURCE_URI_MULTIPLE,
  NV_DS_SOURCE_RTSP,
  NV_DS_SOURCE_CAMERA_CSI,
  NV_DS_SOURCE_AUDIO_WAV,
  NV_DS_SOURCE_AUDIO_URI,
  NV_DS_SOURCE_ALSA_SRC,
  NV_DS_SOURCE_IPC,
} NvDsSourceType;

typedef struct {
  NvDsSourceType type;
  gboolean enable;
  gboolean loop;
  /** When set, the source will play each URI in sequence, switching on EOS.
   * Format: semicolon-separated URI list (e.g. "file:///a.mp4;file:///b.mp4").
   * Prefer configuring via YAML `uri-list:` (sequence), which is normalized into this string. */
  gchar* uri_list;
  /** If true, wrap to the first entry after the last URI completes (no pipeline EOS). */
  gboolean uri_list_loop;
  gboolean live_source;
  gboolean intra_decode_enable;
  gboolean low_latency_mode;
  guint smart_record;
  gint camera_width;
  gint camera_height;
  gint camera_fps_n;
  gint camera_fps_d;
  gint camera_csi_sensor_id;
  gint camera_i2c_bus;
  gint camera_wbmode;
  gint camera_num_buffers;
  gfloat camera_saturation;
  gfloat camera_exposure_compensation;
  gint flip_method;
  gchar* exposure_time_range;
  gchar* gain_range;
  gboolean camera_auto_focus;
  gint camera_v4l2_dev_node;
  gchar* config_file;
  gchar* uri;
  gchar* start_rec_dir_path;
  gchar* start_rec_file_prefix;
  gint latency;
  guint smart_rec_cache_size;
  guint smart_rec_container;
  guint smart_rec_def_duration;
  guint smart_rec_duration;
  guint smart_rec_start_time;
  guint smart_rec_interval;
  guint num_sources;
  guint gpu_id;
  guint camera_id;
  guint source_id;
  guint select_rtp_protocol;
  guint num_decode_surfaces;
  guint num_extra_surfaces;
  guint nvbuf_memory_type;
  guint cuda_memory_type;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  /* copy-hw as VIC applicable only for Jetson */
  guint nvvideoconvert_copy_hw;
#endif
  NvDsDewarperConfig dewarper_config;
  guint drop_frame_interval;
  gboolean extract_sei_type5_data;
  gint rtsp_reconnect_interval_sec;
  guint rtsp_reconnect_attempts;
  gboolean rtsp_reconnect_attempt_exceeded;
  guint udp_buffer_size;
  /** Desired input audio rate to nvinferaudio from PGIE config;
   * This config shall be copied over from NvDsGieConfig
   * at create_multi_source_bin()*/
  guint input_audio_rate;
  /** ALSA device, as defined in an asound configuration file */
  gchar* alsa_device;
  /** Video format to be applied at nvvideoconvert source pad. */
  gchar* video_format;
  gchar* media_type;
} NvDsSourceConfig;

typedef struct NvDsSrcParentBin NvDsSrcParentBin;

typedef struct {
  GstElement* bin;
  GstElement* src_elem;
  GstElement* src_parser;
  GstElement* src_decoder;
  GstElement* cap_filter;
  GstElement* cap_filter1;
  GstElement* depay;
  GstElement* parser;
  GstElement* enc_que;
  GstElement* dec_que;
  GstElement* decodebin;
  GstElement* enc_filter;
  GstElement* encbin_que;
  GstElement* tee;
  GstElement* tee_rtsp_pre_decode;
  GstElement* tee_rtsp_post_decode;
  GstElement* fakesink_queue;
  GstElement* fakesink;
  GstElement* nvvidconv;
  GstElement* audio_converter;
  GstElement* audio_resample;

  gboolean do_record;
  guint64 pre_event_rec;
  GMutex bin_lock;
  guint bin_id;
  gint rtsp_reconnect_interval_sec;
  gint rtsp_reconnect_attempts;
  gint num_rtsp_reconnects;
  gboolean have_eos;
  struct timeval last_buffer_time;
  struct timeval last_reconnect_time;
  gulong src_buffer_probe;
  gulong rtspsrc_monitor_probe;
  gpointer bbox_meta;
  GstBuffer* inbuf;
  gchar* location;
  gchar* file;
  gchar* direction;
  gint latency;
  guint udp_buffer_size;
  gboolean got_key_frame;
  gboolean eos_done;
  gboolean reset_done;
  gboolean live_source;
  gboolean reconfiguring;
  gboolean async_state_watch_running;
  NvDsDewarperBin dewarper_bin;
  gulong probe_id;
  guint64 accumulated_base;
  guint64 prev_accumulated_base;
  guint source_id;
  NvDsSourceConfig* config;
  NvDsSrcParentBin* parent_bin;
  gpointer recordCtx;
  GstElement* uri_audio_tee;
  gulong uri_audio_probe;
  guint uri_audio_link_count;
  gboolean uri_audio_has_pad;
  gboolean uri_audio_eos_seen;
  /** Keep a selected peer audio branch alive after camera exhaustion until its current chapter has drained. */
  gboolean uri_terminal_audio_drain_pending;
  /** Logical video end at permanent camera exhaustion; later peer audio belongs to unpairable video and is dropped. */
  guint64 uri_terminal_audio_cutoff;
  gboolean uri_list_video_eos_seen;
  gboolean uri_list_pads_complete;
  gboolean uri_list_boundary_handled;
  /** Guards URI playlist lifecycle fields when this source is not owned by a multi-source parent. */
  GMutex uri_playlist_mutex;
  gboolean uri_playlist_mutex_initialized;
  /** Optional playlist state (for file sources). */
  gchar** uri_list;
  guint num_uri_list;
  guint uri_list_index;
  /** First physical chapter selected during pre-preroll positioning. */
  guint uri_playlist_initial_uri_index;
  /** Exact duration of complete physical chapters skipped before decoding begins. */
  guint64 uri_playlist_initial_skipped_base_ns;
  guint uri_switch_count;
  gboolean uri_switch_pending;
  /**
   * Main-context work scheduled by decoder streaming threads must not outlive
   * the GstPipeline generation that scheduled it. These fields are accessed
   * with GLib atomic operations, not the playlist mutex: runtime seek
   * recreation deliberately clears that mutex after invalidating callbacks.
   */
  gint uri_playlist_callbacks_enabled;
  gint uri_playlist_callback_generation;
  gint uri_loop_seek_source_id;
  gint uri_switch_source_id;
  gint uri_terminal_audio_drain_source_id;
  /** Video frames admitted after initial positioning across every URI. Never resets at chapter boundaries. */
  guint64 uri_list_decoded_frame_count;
  /** Logical end timestamp of the latest decoded video buffer released by the exact-pair barrier. */
  guint64 uri_list_released_video_end;
  /** Latest committed sequence that reached this source's nvstreammux sink pad. G_MAXUINT64 means none. */
  guint64 uri_list_mux_delivered_sequence;
  /** Frames decoded only after a peer camera permanently ended; they are stopped before nvstreammux. */
  guint64 uri_list_terminal_dropped_frame_count;
  /** Frames consumed solely to reach the configured initial synchronization frontier, before sequence zero. */
  guint64 uri_list_initial_positioned_frame_count;
  /** Decode-time sequence currently waiting for the peer camera before either buffer can reach nvstreammux. */
  guint64 uri_list_frame_ready_sequence;
  gboolean uri_list_permanently_ended;
  /** Initial decoded-video trim, applied before sequence metadata and exact-pair admission begin. */
  guint64 uri_playlist_initial_video_offset_ns;
  /** Initial selected-audio trim. Kept on the zero-video-offset source so muxed audio starts with pair zero. */
  guint64 uri_playlist_initial_audio_offset_ns;
  /** Raw logical PTS of the first retained video frame; retained video timestamps are rebased from this epoch. */
  guint64 uri_playlist_video_origin_ns;
  /** Raw logical PTS requested for selected audio; retained audio timestamps are rebased from this epoch. */
  guint64 uri_playlist_audio_origin_ns;
  /** URI playlist timestamp continuity state (nanoseconds). */
  guint64 uri_list_segment_stop;
  guint64 uri_list_last_pts;
  guint64 uri_list_last_duration;
} NvDsSrcBin;

struct NvDsSrcParentBin {
  GstElement* bin;
  GstElement* pre_mux_tee;
  GstElement* streammux;
  GstElement* nvmultiurisrcbin;
  GThread* reset_thread;
  GstElement* sub_pre_mux_tee[MAX_SOURCE_BINS];
  NvDsSrcBin sub_bins[MAX_SOURCE_BINS];
  guint num_bins;
  guint num_fr_on;
  gboolean live_source;
  /** Coordinates exact decoded-frame pairs across multi-camera URI playlist switches. */
  GMutex uri_playlist_barrier_mutex;
  GCond uri_playlist_barrier_cond;
  gboolean uri_playlist_barrier_initialized;
  guint64 uri_playlist_next_frame_sequence;
  guint64 uri_playlist_paired_video_end;
  gboolean uri_playlist_exact_pairing_enabled;
  gboolean uri_playlist_terminal;
  gboolean uri_playlist_barrier_failed;
  /** Cancels waits for committed frames to reach nvstreammux during failure/application teardown. */
  gboolean uri_playlist_delivery_aborted;
  gboolean uri_playlist_initial_offsets_configured;
  gulong nvstreammux_eosmonitor_probe;
};

/**
 * Configure coherent initial source positioning before the pipeline leaves NULL/READY. Unlike a flushing seek after
 * PAUSED preroll, this trim happens at decoded-pad admission, before sequence zero can be committed or audio exposed.
 */
gboolean configure_uri_playlist_initial_offsets(
    NvDsSrcParentBin* bin,
    guint64 left_video_offset_ns,
    guint64 right_video_offset_ns,
    guint audio_source_id,
    guint64 start_time_ns);

/** Configure a non-paired URI playlist to begin at a logical timestamp before preroll. */
gboolean configure_uri_playlist_initial_position(NvDsSrcParentBin* bin, guint64 start_time_ns);
/** Cancel exact-pair waits and close every logical URI-playlist branch with synthetic EOS. */
void stop_uri_playlist_sources_gracefully(NvDsSrcParentBin* bin);

gboolean create_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin);
gboolean create_audio_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin);
gboolean link_uri_source_audio_src(NvDsSrcBin* bin, GstElement* sinkelem);

/**
 * Initialize @ref NvDsSrcParentBin. It creates and adds source and
 * other elements needed for processing to the bin.
 * It also sets properties mentioned in the configuration file under
 * group @ref CONFIG_GROUP_SOURCE
 *
 * @param[in] num_sub_bins number of source elements.
 * @param[in] configs array of pointers of type @ref NvDsSourceConfig
 *            parsed from configuration file.
 * @param[in] bin pointer to @ref NvDsSrcParentBin to be filled.
 *
 * @return true if bin created successfully.
 */
gboolean create_multi_source_bin(guint num_sub_bins, NvDsSourceConfig* configs, NvDsSrcParentBin* bin);

/** Wake every URI-playlist frame waiter before an error/stop transitions the pipeline to NULL. */
void cancel_uri_playlist_frame_barrier(NvDsSrcParentBin* bin);

/**
 * Fence and remove URI-playlist callbacks owned by the current pipeline
 * generation. Must run on the owning GLib main context before an asynchronous
 * recreation worker is allowed to mutate the reused source-bin storage.
 */
void suspend_uri_playlist_main_context_callbacks(NvDsSrcParentBin* bin);

/** Queue a delayed physical-boundary switch for the runtime recreation regression test. */
gboolean queue_uri_playlist_switch_callback_for_test(NvDsSrcParentBin* bin, guint source_id, guint delay_ms);

/** Exercise scheduler publication racing callback suspension; test-only. */
gboolean exercise_uri_playlist_schedule_suspend_race_for_test(NvDsSrcParentBin* bin, guint source_id, guint delay_ms);

/**
 * Commit one decoded URI-playlist frame only after the other camera reaches
 * the identical sequence. This wait has no frame-dropping wall-clock timeout;
 * permanent playlist exhaustion or explicit cancellation releases an
 * unpairable waiter with FALSE.
 *
 * Exposed for the lossless synchronization regression test. Production
 * callers enter through the decoded-video pad probe.
 */
gboolean wait_at_uri_playlist_frame_barrier(NvDsSrcBin* bin, guint64 sequence, GstClockTime logical_video_end);

/**
 * Initialize @ref NvDsSrcParentBin. It creates and adds nvmultiurisrcbin
 * needed for processing to the bin.
 * It also sets properties mentioned in the configuration file under
 * group @ref CONFIG_GROUP_SOURCE_LIST, @ref CONFIG_GROUP_SOURCE_ALL
 *
 * @param[in] num_sub_bins number of source elements.
 * @param[in] configs array of pointers of type @ref NvDsSourceConfig
 *            parsed from configuration file.
 * @param[in] bin pointer to @ref NvDsSrcParentBin to be filled.
 *
 * @return true if bin created successfully.
 */
gboolean create_nvmultiurisrcbin_bin(guint num_sub_bins, NvDsSourceConfig* configs, NvDsSrcParentBin* bin);

gboolean reset_source_pipeline(gpointer data);
gboolean set_source_to_playing(gpointer data);
gpointer reset_encodebin(gpointer data);
void destroy_smart_record_bin(gpointer data);

namespace hm {
std::optional<NvDsSourceType> source_type_from_string(const std::string& str);
std::string to_string(const NvDsSourceType& type);

} // namespace hm
