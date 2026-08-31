/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef __NVGSTDS_APP_H__
#define __NVGSTDS_APP_H__

#include <gst/gst.h>

#include "hstream/src/apps/apps-common/deepstream_c2d_msg.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_dsanalytics.h"
#include "hstream/src/apps/apps-common/deepstream_dsexample.h"
#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "hstream/src/apps/apps-common/deepstream_image_save.h"
#include "hstream/src/apps/apps-common/deepstream_osd.h"
#include "hstream/src/apps/apps-common/deepstream_perf.h"
#include "hstream/src/apps/apps-common/deepstream_preprocess.h"
#include "hstream/src/apps/apps-common/deepstream_primary_gie.h"
#include "hstream/src/apps/apps-common/deepstream_secondary_gie.h"
#include "hstream/src/apps/apps-common/deepstream_secondary_preprocess.h"
#include "hstream/src/apps/apps-common/deepstream_segvisual.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/apps/apps-common/deepstream_streammux.h"
#include "hstream/src/apps/apps-common/deepstream_tiled_display.h"
#include "hstream/src/apps/apps-common/deepstream_tracker.h"
#include "hstream/src/libs/common/BaselineConfig.h"

// #include "gst-nvdscommonconfig.h"

#include "absl/status/statusor.h"

#include "configurator.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// #ifdef __cplusplus
// extern "C" {
// #endif

typedef struct _AppCtx AppCtx;

typedef void (*bbox_generated_callback)(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
typedef gboolean (*overlay_graphics_callback)(AppCtx* appCtx, GstBuffer* buf, NvDsBatchMeta* batch_meta, guint index);
typedef gboolean (*element_message_callback)(AppCtx* appCtx, GstMessage* message);
typedef void (*bus_message_callback)(AppCtx* appCtx, GstMessage* message);
typedef gboolean (*defer_eos_callback)(AppCtx* appCtx);
typedef void (*fatal_pipeline_error_callback)(AppCtx* appCtx);

typedef struct {
  guint index;
  gulong all_bbox_buffer_probe_id;
  gulong primary_bbox_buffer_probe_id;
  gulong fps_buffer_probe_id;
  GstElement* bin;
  GstElement* tee;
  GstElement* msg_conv;

  // BEGIN tee off downsampled streammux
  NvDsHmVideoPrepBin hmplaycropper_bin;
  // END tee off downsampled streammux

  NvDsPreProcessBin preprocess_bin;
  NvDsPrimaryGieBin primary_gie_bin;
  NvDsOSDBin osd_bin;
  NvDsSegVisualBin segvisual_bin;
  NvDsHmAudioBin hmaudio_bin;
  NvDsSecondaryGieBin secondary_gie_bin;
  NvDsSecondaryPreProcessBin secondary_preprocess_bin;
  NvDsTrackerBin tracker_bin;
  NvDsHmImageMetaMergerBin hmimagemetamerger_bin;
  NvDsSinkBin sink_bin;
  NvDsSinkBin demux_sink_bin;
  NvDsDsAnalyticsBin dsanalytics_bin;
  NvDsDsExampleBin dsexample_bin;
  AppCtx* appCtx;
} NvDsInstanceBin;

struct NvDsPipeline {
  gulong primary_bbox_buffer_probe_id;
  gulong detection_snapshot_buffer_probe_id;
  guint bus_id;
  GstElement* pipeline;
  NvDsSrcParentBin multi_src_bin;
  NvDsInstanceBin instance_bins[MAX_SOURCE_BINS];
  NvDsInstanceBin demux_instance_bins[MAX_SOURCE_BINS];
  NvDsInstanceBin common_elements;
  GstElement* tiler_tee;
  NvDsTiledDisplayBin tiled_display_bin;
  GstElement* demuxer;
  NvDsDsExampleBin dsexample_bin;
  NvDsDsFieldMaskBin dsfieldmask_bin;
  HmStitcherBin hmstitcher_bin;
  NvDsDsPlayTrackerBin dsplaytracker_bin;
  AppCtx* appCtx;
};

struct NvDsConfig {
  // The stage of this application
  gint stage;
  gboolean enable_perf_measurement;
  gint file_loop;
  gint pipeline_recreate_sec;
  gboolean source_list_enabled;
  guint total_num_sources;
  guint num_source_sub_bins;
  guint num_secondary_gie_sub_bins;
  guint num_secondary_preprocess_sub_bins;
  guint num_sink_sub_bins;
  guint num_hmaudio_sub_bins;
  guint num_message_consumers;
  guint perf_measurement_interval_sec;
  guint sgie_batch_size;
  gboolean extract_sei_type5_data;
  gchar* sei_uuid;
  gboolean low_latency_mode;
  gchar* bbox_dir_path;
  gchar* kitti_track_dir_path;
  gchar* reid_track_dir_path;
  gchar* terminated_track_output_path;
  gchar* shadow_track_output_path;

  gchar** uri_list;
  gchar** sensor_id_list;
  gchar** sensor_name_list;
  NvDsSourceConfig multi_source_config[MAX_SOURCE_BINS];
  NvDsStreammuxConfig streammux_config;
  NvDsStreammuxConfig streammux2_config;
  NvDsOSDConfig osd_config;
  NvDsSegVisualConfig segvisual_config;
  NvDsPreProcessConfig preprocess_config;
  NvDsPreProcessConfig secondary_preprocess_sub_bin_config[MAX_SECONDARY_PREPROCESS_BINS];
  NvDsGieConfig primary_gie_config;
  NvDsTrackerConfig tracker_config;
  NvDsGieConfig secondary_gie_sub_bin_config[MAX_SECONDARY_GIE_BINS];
  NvDsSinkSubBinConfig sink_bin_sub_bin_config[MAX_SINK_BINS];
  NvDsMsgConsumerConfig message_consumer_config[MAX_MESSAGE_CONSUMERS];
  NvDsTiledDisplayConfig tiled_display_config;
  NvDsDsAnalyticsConfig dsanalytics_config;
  NvDsDsExampleConfig dsexample_config;
  NvDsHmImageMetaMergerConfig hmimagemetamerger_config;
  HmPlayCropperConfig hmplaycropper_config;
  NvDsDsFieldMaskConfig dsfieldmask_config;
  HmStitcherConfig hmsticher_config;
  NvDsDsPlayTrackerConfig dsplaytracker_config;
  NvDsSinkMsgConvBrokerConfig msg_conv_config;
  NvDsImageSave image_save_config;
  NvDsHmAudioConfig hmaudio_config[MAX_SOURCE_BINS];

  /** To support nvmultiurisrcbin */
  gboolean use_nvmultiurisrcbin;
  gboolean stream_name_display;
  guint max_batch_size;
  gchar* http_ip;
  gchar* http_port;
  gboolean source_attr_all_parsed;
  NvDsSourceConfig source_attr_all_config;

  /** To set Global GPU ID for all the componenents at once if needed
   * This will be used in case gpu_id prop is not set for a component
   * if gpu_id prop is set for a component, global_gpu_id will be overridden by it */
  gint global_gpu_id;
  gchar video_converter[64];
};

typedef struct {
  gulong frame_num;
} NvDsInstanceData;

struct _AppCtx {
  gboolean version{false};
  gboolean cintr{false};
  gboolean show_bbox_text{false};
  gboolean seeking{false};
  gboolean quit{false};
  gboolean capture_playtracker_detections{false};
  gint person_class_id{0};
  gint car_class_id{0};
  gint return_value{0};
  guint index{0};
  gint active_source_index{0};
  GstState observed_pipeline_state{GST_STATE_NULL};

  GMutex app_lock{
      0,
  };
  GCond app_cond{
      0,
  };

  NvDsPipeline pipeline{
      0,
  };
  NvDsConfig config{
      0,
  };
  // NvDsConfig override_config{
  //     0,
  // };
  NvDsInstanceData instance_data[MAX_SOURCE_BINS] = {
      0,
  };
  NvDsC2DContext* c2d_ctx[MAX_MESSAGE_CONSUMERS] = {
      0,
  };
  NvDsAppPerfStructInt perf_struct{
      0,
  };
  bbox_generated_callback bbox_generated_post_analytics_cb{
      0,
  };
  bbox_generated_callback all_bbox_generated_cb{
      0,
  };
  overlay_graphics_callback overlay_graphics_cb{
      0,
  };
  element_message_callback element_message_cb{nullptr};
  bus_message_callback bus_message_cb{nullptr};
  defer_eos_callback defer_eos_cb{nullptr};
  fatal_pipeline_error_callback fatal_pipeline_error_cb{nullptr};
  gboolean defer_bus_watch{false};
  guint pipeline_recreate_source_id{0};
  gboolean pipeline_cleanup_complete{true};
  guint pipeline_create_attempt_count{0};
  NvDsFrameLatencyInfo* latency_info{nullptr};
  GMutex latency_lock{
      0,
  };
  gboolean latency_lock_initialized{false};
  GThread* ota_handler_thread{nullptr};
  guint ota_inotify_fd{0};
  guint ota_watch_desc{0};

  /** Hash table to save NvDsSensorInfo
   * obtained with REST API stream/add, remove operations
   * The key is souce_id */
  GHashTable* sensorInfoHash{nullptr};
  gboolean eos_received{false};
};

inline gboolean terminal_source_error_requires_failure(const AppCtx* app_ctx) {
  return !app_ctx || !app_ctx->eos_received;
}

inline gboolean mark_terminal_source_error_failure(AppCtx* app_ctx) {
  if (!terminal_source_error_requires_failure(app_ctx)) {
    return FALSE;
  }
  if (app_ctx) {
    app_ctx->return_value = -1;
  }
  return TRUE;
}

class HmApp : public _AppCtx {
 public:
  HmApp(std::string game_id, std::string app_config_file, int override_gpu_id)
      : game_id_(std::move(game_id)), app_config_file_(std::move(app_config_file)), override_gpu_id_(override_gpu_id) {
    g_mutex_init(&app_lock);
    g_cond_init(&app_cond);
  }

  ~HmApp() {
    g_cond_clear(&app_cond);
    g_mutex_clear(&app_lock);
  }

  const std::string& app_config_file() const {
    return app_config_file_;
  }

  absl::Status load_config() {
    const auto config_root = hm::baseline_config::resolve_root();
    if (!config_root.ok())
      return config_root.status();
    configurator_ = std::make_unique<hm::Configurator>(game_id_, config_root->string(), override_gpu_id_);
    return configurator_->configure();
  }

  bool underlay_config(const std::string& node, const std::string& file) {
    return configurator_->underlay_config(node, file);
  }
  const hm::Configurator& configurator() const {
    return *configurator_;
  }
  hm::Configurator& configurator() {
    return *configurator_;
  }
  absl::Status complete_configuration(
      bool force,
      bool clean_stitching_artifacts = false,
      bool clean_stitching_from_control_points = false,
      const std::string& clean_expected_invalidation_id = {},
      bool show_render_sink = false,
      double show_render_scale = -1.0,
      bool stitching_calibration_only = false) {
    return configurator_->complete_configuration(
        force,
        clean_stitching_artifacts,
        clean_stitching_from_control_points,
        clean_expected_invalidation_id,
        show_render_sink,
        show_render_scale,
        std::filesystem::path(app_config_file_).parent_path(),
        stitching_calibration_only);
  }

  bool pause();

 private:
  std::unique_ptr<hm::Configurator> configurator_;
  std::string game_id_;
  std::string app_config_file_;
  int override_gpu_id_;
};

/**
 * @brief  Create DS Anyalytics Pipeline per the appCtx
 *         configurations
 * @param  appCtx [IN/OUT] The application context
 *         providing the config info and where the
 *         pipeline resources are maintained
 * @param  bbox_generated_post_analytics_cb [IN] This callback
 *         shall be triggered after analytics
 *         (PGIE, Tracker or the last SGIE appearing
 *         in the pipeline)
 *         More info: create_common_elements()
 * @param  all_bbox_generated_cb [IN]
 * @param  perf_cb [IN]
 * @param  overlay_graphics_cb [IN]
 */
gboolean create_pipeline(
    AppCtx* appCtx,
    bbox_generated_callback bbox_generated_post_analytics_cb,
    bbox_generated_callback all_bbox_generated_cb,
    perf_callback perf_cb,
    overlay_graphics_callback overlay_graphics_cb);

gboolean pause_pipeline(AppCtx* appCtx);
gboolean resume_pipeline(AppCtx* appCtx);
gboolean seek_pipeline(AppCtx* appCtx, glong milliseconds, gboolean seek_is_relative);

void toggle_show_bbox_text(AppCtx* appCtx);

void destroy_pipeline(AppCtx* appCtx);
void destroy_pipeline_for_recreate(AppCtx* appCtx);
gboolean attach_pipeline_bus_watch(AppCtx* appCtx);
void detach_pipeline_bus_watch(AppCtx* appCtx);
gboolean dispatch_pending_pipeline_bus_messages(AppCtx* appCtx);
gboolean stop_pipeline_gracefully(AppCtx* appCtx, GstClockTime timeout);
void restart_pipeline(AppCtx* appCtx);

// std::optional<YAML::Node> maybe_get_config_file(
//     const YAML::Node& yaml_node,
//     const std::string& config_dir);

absl::StatusOr<YAML::Node> get_app_config(const gchar* cfg_file_path);

/**
 * Function to read properties from configuration file.
 *
 * @param[in] config pointer to @ref NvDsConfig
 * @param[in] cfg_file_path path of configuration file.
 *
 * @return true if parsed successfully.
 */
gboolean parse_config_file(NvDsConfig* config, const gchar* cfg_file_path);

gboolean parse_config_yaml(const YAML::Node& configyml, NvDsConfig* config, const std::string& config_dir);

/**
 * Function to procure the NvDsSensorInfo for the source_id
 * that was added using the nvmultiurisrcbin REST API
 *
 * @param[in] appCtx [IN/OUT] The application context
 *            providing the config info and where the
 *            pipeline resources are maintained
 * @param[in] source_id [IN] The unique source_id found in NvDsFrameMeta
 *
 * @return [transfer-floating] The NvDsSensorInfo for the source_id
 * that was added using the nvmultiurisrcbin REST API.
 * Please note that the returned pointer
 * will be valid only until the stream is removed.
 */
// NvDsSensorInfo* get_sensor_info(AppCtx* appCtx, guint source_id);

// #ifdef __cplusplus
// }
// #endif

#ifdef __cplusplus

template <typename T, typename... Ts>
T or_flags(const T& flag0, const Ts&... flags) {
  return static_cast<T>((static_cast<int>(flag0) | ... | static_cast<int>(flags)));
}

#endif // __cplusplus
#endif
