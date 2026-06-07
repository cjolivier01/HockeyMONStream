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

#ifndef _NVGSTDS_DSFIELDMASK_H_
#define _NVGSTDS_DSFIELDMASK_H_

#include <glib-2.0/glib.h>
#include <gst/gst.h>

#include <stdlib.h>

#include "deepstream_config.h"
#include "deepstream_sinks.h"
#include "deepstream_sources.h"
#include "gst_plugin_properties.h"

/**
 *  ______  _       _     _ __  __             _
 * |  ____|(_)     | |   | |  \/  |           | |
 * | |__    _  ___ | | __| | \  / | __ _  ___ | | __
 * |  __|  | |/ _ \| |/ _` | |\/| |/ _` |/ __|| |/ /
 * | |     | |  __/| | (_| | |  | | (_| |\__ \|   <
 * |_|     |_|\___||_|\__,_|_|  |_|\__,_||___/|_|\_\
 *
 */
struct NvDsDsFieldMaskConfig {
  // Create a bin for the element only if enabled
  gboolean enable;
  // Struct members to store config / properties for the element
  guint unique_id;
  guint gpu_id;
  gchar detection_mask_file[PATH_MAX * 4];
  hm::gst::PluginProperties plugin_properties;
  // For nvvidconv
  guint nvbuf_memory_type;
};

// Struct to store references to the bin and elements
struct NvDsDsFieldMaskBin {
  GstElement* bin{nullptr};
  GstElement* queue{nullptr};
  GstElement* pre_conv{nullptr};
  GstElement* cap_filter{nullptr};
  GstElement* elem_dsfieldmask{nullptr};
};

// Function to create the bin and set properties
gboolean create_dsfieldmask_bin(const NvDsDsFieldMaskConfig* config, NvDsDsFieldMaskBin* bin);

constexpr size_t kMyMaxPath = PATH_MAX * 4;

/**
 *  _____  _          _______              _
 * |  __ \| |        |__   __|            | |
 * | |__) | | __ _ _   _| |_ __  __ _  ___| | __ ___  _ __
 * |  ___/| |/ _` | | | | | '__|/ _` |/ __| |/ // _ \| '__|
 * | |    | | (_| | |_| | | |  | (_| | (__|   <|  __/| |
 * |_|    |_|\__,_|\__, |_|_|   \__,_|\___|_|\_\\___||_|
 *                  __/ |
 *                 |___/
 */
struct NvDsDsPlayTrackerConfig {
  // Create a bin for the element only if enabled
  gboolean enable;
  // Struct members to store config / properties for the element
  guint unique_id;
  guint gpu_id;
  gboolean draw;
  gboolean show;
  gfloat fixed_edge_rotation_angle;
  gfloat dynamic_acceleration_scaling;
  gchar config_file[kMyMaxPath];
  hm::gst::PluginProperties plugin_properties;
  hm::gst::PluginProperties private_properties;
};

// Struct to store references to the bin and elements
struct NvDsDsPlayTrackerBin {
  GstElement* bin{nullptr};
  GstElement* queue{nullptr};
  GstElement* pre_conv{nullptr};
  GstElement* cap_filter{nullptr};
  GstElement* elem_dsplaytracker{nullptr};
};

// Function to create the bin and set properties
gboolean create_dsplaytracker_bin(NvDsDsPlayTrackerConfig* config, NvDsDsPlayTrackerBin* bin);

/**
 * __      __ _     _             _____
 * \ \    / /(_)   | |           |  __ \
 *  \ \  / /  _  __| | ___   ___ | |__) |_ __  ___  _ __
 *   \ \/ /  | |/ _` |/ _ \ / _ \|  ___/| '__|/ _ \| '_ \
 *    \  /   | | (_| |  __/| (_) | |    | |  |  __/| |_) |
 *     \/    |_|\__,_|\___| \___/|_|    |_|   \___|| .__/
 *                                                 | |
 *                                                 |_|
 */
struct NvDsHmVideoPrepConfig {
  // This config was found and parsed (so create this bin)
  gboolean enable;
  guint unique_id;
  guint gpu_id;
  gboolean show;
  bool has_queue;
  bool has_videoconvert;
  bool has_videorate;
  guint fps_n;
  guint fps_d;
  guint num_output_buffers;
  guint dewarper_dump_frames;
  guint source_id;
  guint num_surfaces_per_frame;
  guint num_batch_buffers;
  guint output_width;
  guint output_height;
  gchar config_file[kMyMaxPath];
  gchar plugin_type[kMyMaxPath];
  gchar plugin_private_config[1024 * 1024];
  hm::gst::PluginProperties plugin_properties;
  hm::gst::PluginProperties private_properties;

  // For nvvidconv
  guint nvbuf_memory_type;
};

struct NvDsHmVideoPrepBin {
  GstElement* bin;
  GstElement* queue;
  GstElement* src_queue;
  GstElement* videorate;
  GstElement* conv_queue;
  GstElement* nvvidconv;
  GstElement* cap_filter;
  GstElement* playcropper_caps_filter;
  GstElement* playcropper;
};

struct HmPlayCropperConfig : public NvDsHmVideoPrepConfig {
  // Four (x,y) coordinates
  gboolean no_crop;
  gboolean show_scoreboard;
  gchar scoreboard_projected_width[32];
  gchar scoreboard_projected_height[32];
  gfloat scoreboard_scale;
  gboolean plot_play_tracking;
  gboolean plot_player_tracking;
  gboolean transform_object_meta;
  gfloat fixed_edge_rotation_angle;
  int scoreboard_perspective_polygon[8];
  guint runtime_output_max_width;
  guint runtime_output_max_height;
};

gboolean create_hmplaycropper_bin(HmPlayCropperConfig* config, NvDsHmVideoPrepBin* bin);

/**
 *   _____ _   _  _        _
 *  / ____| | (_)| |      | |
 * | (___ | |_ _ | |_  ___| |__   ___  _ __
 *  \___ \| __| || __|/ __| '_ \ / _ \| '__|
 *  ____) | |_| || |_| (__| | | |  __/| |
 * |_____/ \__|_| \__|\___|_| |_|\___||_|
 *
 */
struct HmStitcherConfig : public NvDsHmVideoPrepConfig {
  gboolean configure_only;
  gboolean one_pass_mode;
  gulong left_frame_offset_ns;
  gulong right_frame_offset_ns;
  gboolean show;
  gboolean force_scoreboard_config;
  gfloat post_stitch_rotate_degrees;
};

// Struct to store references to the bin and elements
struct HmStitcherBin {
  GstElement* bin{nullptr};
  GstElement* queue{nullptr};
  GstElement* pre_conv{nullptr};
  GstElement* cap_filter{nullptr};
  GstElement* elem_hmstitcher{nullptr};
};

gboolean create_hmstitcher_bin(HmStitcherConfig* config, HmStitcherBin* bin);

/* clang-format off */
/**
 *  _    _           _____                             __  __       _         __  __
 * | |  | |         |_   _|                           |  \/  |     | |       |  \/  |
 * | |__| |_ __ ___   | |  _ __ ___   __ _  __ _  ___ | \  / | ___ | |_  __ _| \  / | ___  _ __  __ _  ___  _ __
 * |  __  | '_ ` _ \  | | | '_ ` _ \ / _` |/ _` |/ _ \| |\/| |/ _ \| __|/ _` | |\/| |/ _ \| '__|/ _` |/ _ \| '__|
 * | |  | | | | | | |_| |_| | | | | | (_| | (_| |  __/| |  | |  __/| |_| (_| | |  | |  __/| |  | (_| |  __/| |
 * |_|  |_|_| |_| |_|_____|_| |_| |_|\__,_|\__, |\___||_|  |_|\___| \__|\__,_|_|  |_|\___||_|   \__, |\___||_|
 *                                          __/ |                                                __/ |
 *                                         |___/                                                |___/
 */
/* clang-format on */
struct NvDsHmImageMetaMergerConfig {
  // This config was found and parsed (so create this bin)
  gboolean enable;
  guint unique_id;
  guint gpu_id;
  // For nvvidconv
  guint nvbuf_memory_type;
};

struct NvDsHmImageMetaMergerBin {
  GstElement* bin;
  // GstElement* queue;
  GstElement* meta_identity_in;
  GstElement* image_identity_in;
};

gboolean create_hmimagemetamerger_bin(NvDsHmImageMetaMergerConfig* config, NvDsHmImageMetaMergerBin* bin);

/**
 *                     _  _
 *     /\             | |(_)
 *    /  \   _   _  __| | _  ___
 *   / /\ \ | | | |/ _` || |/ _ \
 *  / ____ \| |_| | (_| || | (_) |
 * /_/    \_\\__,_|\__,_||_|\___/
 *
 *
 */
enum EHmAudioSrc {
  SRC_DEFAULT = 0,
  SRC_FILE = 1,
  SRC_SOURCE_BIN = 2,
};

enum EHmAudioDest {
  DEST_INDEPENDENT = 0,
  DEST_SINK = 1, // uses sink-id
  DEST_MULTI_SINK = 2, // uses multi-sink-ids
};

struct NvDsHmAudioConfig {
  gboolean enable;
  guint src;
  guint source_id;
  guint dest;
  gint sink_id;
  // For multiple sink destinations
  gint multi_sink_ids[MAX_SINK_BINS];
  gchar audio_location[kMyMaxPath];
  gchar alsa_src_device[kMyMaxPath];
  gchar alsa_dest_device[kMyMaxPath];
};

struct NvDsHmAudioBin {
  GstElement* bin{nullptr};
  GstElement* audiosrc{nullptr};

  // File only
  GstElement* qtdemux;
  GstElement* demux_queue;
  GstElement* decodebin;

  GstElement* tee;

  // All
  GstElement* audioconvert{nullptr};
  GstElement* audioresample{nullptr};
  GstElement* queue;
  GstElement* audioparse;

  // RTSP/RTMP
  GstElement* encoder;

  GstElement* post_queue;

  // GstElement* postparse_presink_tee;
  GstElement* audiosink{nullptr};

  // If the referenced sink is fakesink, make one similar to the video faksesink
  NvDsSinkBinSubBin fakesink_bin;
};

// struct NvDsAudioVideoMerger {
//   GstElement* premux_queue;
//   GstElement* muxer;
// };

gboolean create_hmaudio_bin(
    GstBin* parent_bin,
    const NvDsHmAudioConfig* config,
    NvDsHmAudioBin* bin,
    NvDsSrcBin* src_sub_bins,
    const NvDsSinkSubBinConfig* config_array,
    NvDsSinkBin* sink_bin);

#endif /* _NVGSTDS_DSFIELDMASK_H_ */
