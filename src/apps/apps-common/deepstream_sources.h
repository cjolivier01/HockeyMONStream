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
  /** Optional playlist state (for file sources). */
  gchar** uri_list;
  guint num_uri_list;
  guint uri_list_index;
  guint uri_switch_count;
  gboolean uri_switch_pending;
  /** URI playlist timestamp continuity state (nanoseconds). */
  guint64 uri_list_segment_stop;
  guint64 uri_list_last_pts;
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
  gulong nvstreammux_eosmonitor_probe;
};

gboolean create_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin);
gboolean create_audio_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin);

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
