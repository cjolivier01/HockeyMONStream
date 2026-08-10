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

#include <glib-2.0/glib-object.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <cuda_runtime_api.h>
#include <gst/audio/audio.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtsp/gstrtsptransport.h>
#include "deepstream_common.h"
#include "deepstream_sources.h"
#include "hstream/src/apps/apps-common/HStreamLosslessMux.h"

#include "hstream/src/libs/camera/AutoFocus.h"
#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"
#include "hstream/src/libs/common/pipeline_utils.h"

#include "gst-nvdssr.h"
#include "gst-nvevent.h"
#include "nvdsgstutils.h"

#define SRC_CONFIG_KEY "src_config"
#define SOURCE_RESET_INTERVAL_SEC 60

GST_DEBUG_CATEGORY_EXTERN(NVDS_APP);
GST_DEBUG_CATEGORY_EXTERN(APP_CFG_PARSER_CAT);

static gboolean install_mux_eosmonitor_probe = FALSE;

struct UriListPadProbeData {
  NvDsSrcBin* bin;
  guint uri_index;
  GstClockTime base;
  gboolean is_video;
};

static gboolean send_uri_audio_eos(NvDsSrcBin* bin, gboolean log_failure);
static gboolean switch_to_next_uri(gpointer data);
static gboolean finish_uri_terminal_audio_drain(gpointer data);
static void cancel_uri_playlist_source(NvDsSrcBin* bin, gboolean barrier_failed);

constexpr gint64 kUriPlaylistBarrierWaitUs = 30 * G_TIME_SPAN_SECOND;

static GMutex* uri_playlist_mutex(NvDsSrcBin* bin) {
  return bin && bin->parent_bin ? &bin->parent_bin->uri_playlist_barrier_mutex : &bin->uri_playlist_mutex;
}

static gboolean uri_playlist_terminal_locked(const NvDsSrcBin* bin) {
  return !bin || (bin->parent_bin ? bin->parent_bin->uri_playlist_terminal : bin->uri_list_permanently_ended);
}

static void advance_uri_playlist_base_locked(NvDsSrcBin* bin) {
  GstClockTime stop = bin->uri_list_segment_stop;
  if (stop == GST_CLOCK_TIME_NONE && GST_CLOCK_TIME_IS_VALID(bin->uri_list_last_pts)) {
    stop = bin->uri_list_last_pts;
    if (GST_CLOCK_TIME_IS_VALID(bin->uri_list_last_duration)) {
      stop += bin->uri_list_last_duration;
    }
  }
  if (stop != GST_CLOCK_TIME_NONE) {
    bin->accumulated_base = bin->prev_accumulated_base + stop;
    bin->prev_accumulated_base = bin->accumulated_base;
  }
}

static GstClockTime uri_playlist_logical_video_end_locked(const NvDsSrcBin* bin) {
  if (!bin) {
    return GST_CLOCK_TIME_NONE;
  }
  GstClockTime local_end = GST_CLOCK_TIME_NONE;
  if (GST_CLOCK_TIME_IS_VALID(bin->uri_list_last_pts)) {
    local_end = bin->uri_list_last_pts;
    if (GST_CLOCK_TIME_IS_VALID(bin->uri_list_last_duration)) {
      local_end += bin->uri_list_last_duration;
    }
  } else if (bin->uri_list_segment_stop != GST_CLOCK_TIME_NONE) {
    local_end = bin->uri_list_segment_stop;
  }
  return local_end == GST_CLOCK_TIME_NONE ? GST_CLOCK_TIME_NONE : bin->prev_accumulated_base + local_end;
}

static std::vector<NvDsSrcBin*> uri_playlist_sources(NvDsSrcParentBin* parent) {
  std::vector<NvDsSrcBin*> sources;
  if (!parent) {
    return sources;
  }
  for (guint source_index = 0; source_index < parent->num_bins; ++source_index) {
    NvDsSrcBin* source = &parent->sub_bins[source_index];
    if (source->bin && source->uri_list && source->num_uri_list >= 1) {
      sources.push_back(source);
    }
  }
  return sources;
}

static GstPadProbeReturn fail_uri_playlist_audio_gate(NvDsSrcBin* bin, const char* reason) {
  cancel_uri_playlist_source(bin, TRUE);
  GST_ELEMENT_ERROR(
      bin->src_elem,
      STREAM,
      FAILED,
      ("Could not preserve lossless source audio alignment"),
      ("source=%u: %s", bin->source_id, reason));
  return GST_PAD_PROBE_DROP;
}

static GstPadProbeReturn gate_uri_playlist_audio_buffer(
    GstPad* pad,
    GstPadProbeInfo* info,
    UriListPadProbeData* probe_data) {
  NvDsSrcBin* bin = probe_data ? probe_data->bin : nullptr;
  NvDsSrcParentBin* parent = bin ? bin->parent_bin : nullptr;
  if (!bin || !parent || !parent->uri_playlist_exact_pairing_enabled || bin->uri_audio_link_count == 0) {
    return GST_PAD_PROBE_OK;
  }

  GstBuffer* buffer = GST_BUFFER(info->data);
  if (!buffer || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
    return fail_uri_playlist_audio_gate(bin, "audio buffer has no valid PTS");
  }
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstAudioInfo audio_info;
  gst_audio_info_init(&audio_info);
  const gboolean valid_audio_info = caps && gst_audio_info_from_caps(&audio_info, caps);
  if (caps) {
    gst_caps_unref(caps);
  }
  const guint bytes_per_frame = valid_audio_info ? GST_AUDIO_INFO_BPF(&audio_info) : 0;
  const guint rate = valid_audio_info ? GST_AUDIO_INFO_RATE(&audio_info) : 0;
  if (bytes_per_frame == 0 || rate == 0) {
    return fail_uri_playlist_audio_gate(bin, "audio caps have no valid sample layout");
  }

  const gsize buffer_size = gst_buffer_get_size(buffer);
  const GstAudioMeta* audio_meta = gst_buffer_get_audio_meta(buffer);
  const guint64 buffer_frames = audio_meta ? audio_meta->samples : buffer_size / bytes_per_frame;
  GstClockTime duration = GST_BUFFER_DURATION(buffer);
  if (!GST_CLOCK_TIME_IS_VALID(duration)) {
    duration = gst_util_uint64_scale(buffer_frames, GST_SECOND, rate);
  }
  if (!GST_CLOCK_TIME_IS_VALID(duration)) {
    return fail_uri_playlist_audio_gate(bin, "audio buffer has no valid duration");
  }
  const GstClockTime logical_start = GST_BUFFER_PTS(buffer) + probe_data->base;
  const GstClockTime logical_end = logical_start + duration;

  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  while (!uri_playlist_terminal_locked(bin) &&
         (!GST_CLOCK_TIME_IS_VALID(parent->uri_playlist_paired_video_end) ||
          logical_end > parent->uri_playlist_paired_video_end) &&
         !bin->uri_list_video_eos_seen) {
    g_cond_wait(&parent->uri_playlist_barrier_cond, mutex);
  }
  const gboolean terminal = uri_playlist_terminal_locked(bin);
  const gboolean drain_terminal_audio = terminal && bin->uri_terminal_audio_drain_pending;
  GstClockTime audio_limit = GST_CLOCK_TIME_NONE;
  if (drain_terminal_audio) {
    audio_limit = bin->uri_terminal_audio_cutoff;
  } else if (
      !terminal &&
      (!GST_CLOCK_TIME_IS_VALID(parent->uri_playlist_paired_video_end) ||
       logical_end > parent->uri_playlist_paired_video_end)) {
    // Video EOS for this chapter proves its current logical endpoint. Clip only decoder padding beyond the fully
    // paired frontier; a nonterminal next chapter will continue from that same logical timestamp.
    audio_limit =
        GST_CLOCK_TIME_IS_VALID(parent->uri_playlist_paired_video_end) ? parent->uri_playlist_paired_video_end : 0;
  }
  g_mutex_unlock(mutex);

  if (terminal && !drain_terminal_audio) {
    return GST_PAD_PROBE_DROP;
  }
  if (audio_limit == GST_CLOCK_TIME_NONE || logical_end <= audio_limit) {
    return GST_PAD_PROBE_OK;
  }
  if (logical_start >= audio_limit) {
    return GST_PAD_PROBE_DROP;
  }

  const GstClockTime allowed_duration = audio_limit - logical_start;
  const guint64 allowed_frames = gst_util_uint64_scale(allowed_duration, rate, GST_SECOND);
  const guint64 retained_frames = std::min(allowed_frames, buffer_frames);
  if (retained_frames == 0) {
    return GST_PAD_PROBE_DROP;
  }
  // GstAudioMeta is authoritative for non-interleaved decoder output. The audio helper updates plane offsets/sample
  // counts for planar buffers and creates a bounded region for interleaved buffers, avoiding stale metadata or an
  // invalid in-place resize. Give it a temporary reference so failure leaves the probe's original buffer valid.
  GstBuffer* clipped = gst_audio_buffer_truncate(gst_buffer_ref(buffer), bytes_per_frame, 0, retained_frames);
  if (!clipped) {
    return fail_uri_playlist_audio_gate(bin, "could not clip audio at the paired-video frontier");
  }
  GST_BUFFER_DURATION(clipped) = gst_util_uint64_scale(retained_frames, GST_SECOND, rate);
  gst_buffer_unref(buffer);
  GST_PAD_PROBE_INFO_DATA(info) = clipped;
  return GST_PAD_PROBE_OK;
}

static void cancel_uri_playlist_source(NvDsSrcBin* bin, gboolean barrier_failed) {
  if (!bin || !bin->uri_list || bin->num_uri_list < 1) {
    return;
  }
  NvDsSrcParentBin* parent = bin->parent_bin;
  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  if (parent) {
    parent->uri_playlist_terminal = TRUE;
    parent->uri_playlist_barrier_failed = parent->uri_playlist_barrier_failed || barrier_failed;
    for (NvDsSrcBin* source : uri_playlist_sources(parent)) {
      source->uri_list_permanently_ended = TRUE;
      source->uri_switch_pending = FALSE;
    }
    g_cond_broadcast(&parent->uri_playlist_barrier_cond);
  } else {
    bin->uri_list_permanently_ended = TRUE;
    bin->uri_switch_pending = FALSE;
  }
  g_mutex_unlock(mutex);
}

void cancel_uri_playlist_frame_barrier(NvDsSrcParentBin* parent) {
  if (!parent) {
    return;
  }
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  parent->uri_playlist_terminal = TRUE;
  for (NvDsSrcBin* source : uri_playlist_sources(parent)) {
    source->uri_list_permanently_ended = TRUE;
    source->uri_switch_pending = FALSE;
  }
  g_cond_broadcast(&parent->uri_playlist_barrier_cond);
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
}

static gboolean configure_lossless_uri_playlist_mux(NvDsSrcParentBin* parent) {
  if (!parent || !parent->streammux || uri_playlist_sources(parent).size() != 2) {
    return TRUE;
  }
  if (g_strcmp0(g_getenv("USE_NEW_NVSTREAMMUX"), "yes") != 0) {
    g_object_set(G_OBJECT(parent->streammux), "batched-push-timeout", -1, NULL);
  }
  return TRUE;
}

static gboolean send_uri_video_eos(NvDsSrcBin* bin) {
  if (!bin || !bin->tee) {
    return FALSE;
  }
  gboolean sent = FALSE;
  GstIterator* iterator = gst_element_iterate_src_pads(bin->tee);
  GValue item = G_VALUE_INIT;
  gboolean done = FALSE;
  while (!done) {
    switch (gst_iterator_next(iterator, &item)) {
      case GST_ITERATOR_OK: {
        GstPad* src_pad = GST_PAD(g_value_get_object(&item));
        if (src_pad && gst_pad_is_linked(src_pad)) {
          sent = gst_pad_push_event(src_pad, gst_event_new_eos()) || sent;
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(iterator);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  if (G_VALUE_TYPE(&item) != 0) {
    g_value_unset(&item);
  }
  gst_iterator_free(iterator);
  return sent;
}

static void mark_uri_playlist_terminal(NvDsSrcBin* bin) {
  if (!bin) {
    return;
  }
  NvDsSrcParentBin* parent = bin->parent_bin;
  std::vector<NvDsSrcBin*> sources = uri_playlist_sources(parent);
  if (!parent) {
    sources.push_back(bin);
  }
  std::vector<gboolean> drain_peer_audio(sources.size(), FALSE);

  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  const gboolean already_terminal = parent ? parent->uri_playlist_terminal : bin->uri_list_permanently_ended;
  const GstClockTime local_video_end = uri_playlist_logical_video_end_locked(bin);
  const GstClockTime audio_cutoff = parent && parent->uri_playlist_exact_pairing_enabled
      ? (GST_CLOCK_TIME_IS_VALID(parent->uri_playlist_paired_video_end) ? parent->uri_playlist_paired_video_end : 0)
      : local_video_end;
  if (!already_terminal) {
    if (parent) {
      parent->uri_playlist_terminal = TRUE;
    }
    for (size_t source_index = 0; source_index < sources.size(); ++source_index) {
      NvDsSrcBin* source = sources[source_index];
      source->uri_list_permanently_ended = TRUE;
      source->uri_switch_pending = FALSE;
      source->uri_terminal_audio_cutoff = audio_cutoff;
      source->uri_terminal_audio_drain_pending = source != bin && source->uri_audio_link_count > 0 &&
          ((!source->uri_list_pads_complete && !source->uri_audio_has_pad) ||
           (source->uri_audio_has_pad && !source->uri_audio_eos_seen));
      drain_peer_audio[source_index] = source->uri_terminal_audio_drain_pending;
    }
    if (parent) {
      g_cond_broadcast(&parent->uri_playlist_barrier_cond);
    }
  }
  g_mutex_unlock(mutex);
  if (already_terminal) {
    return;
  }

  // The ending camera has drained both streams before reaching this point. Peers end here because no later exact
  // camera pair can exist. A linked peer audio branch is the one exception: keep its decoder alive until serialized
  // audio EOS proves that all audio through the last retained video interval has cleared downstream backpressure.
  // Audio after the cutoff belongs only to unpairable peer video and is dropped by the pad probe below.
  for (size_t source_index = 0; source_index < sources.size(); ++source_index) {
    NvDsSrcBin* source = sources[source_index];
    if (source != bin && !drain_peer_audio[source_index] && source->src_elem) {
      gst_element_set_state(source->src_elem, GST_STATE_NULL);
    }
  }

  // Send serialized EOS on every branch except a peer still draining audio. That peer emits its synthetic EOS from
  // the main loop after raw chapter EOS, so setting its decoder to NULL cannot join its own streaming task.
  for (size_t source_index = 0; source_index < sources.size(); ++source_index) {
    NvDsSrcBin* source = sources[source_index];
    if (!drain_peer_audio[source_index]) {
      send_uri_audio_eos(source, FALSE);
      send_uri_video_eos(source);
    }
  }
}

static gboolean finish_uri_terminal_audio_drain(gpointer data) {
  auto* bin = static_cast<NvDsSrcBin*>(data);
  if (!bin) {
    return G_SOURCE_REMOVE;
  }
  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  const gboolean terminal = uri_playlist_terminal_locked(bin);
  const gboolean drain_complete = bin->uri_terminal_audio_drain_pending &&
      (bin->uri_audio_eos_seen || (bin->uri_list_pads_complete && !bin->uri_audio_has_pad));
  if (terminal && drain_complete) {
    bin->uri_terminal_audio_drain_pending = FALSE;
  }
  g_mutex_unlock(mutex);
  if (!terminal || !drain_complete) {
    return G_SOURCE_REMOVE;
  }

  if (bin->src_elem) {
    gst_element_set_state(bin->src_elem, GST_STATE_NULL);
  }
  send_uri_audio_eos(bin, FALSE);
  send_uri_video_eos(bin);
  return G_SOURCE_REMOVE;
}

static gboolean wait_at_uri_playlist_frame_barrier(NvDsSrcBin* bin, guint64 sequence, GstClockTime logical_video_end) {
  NvDsSrcParentBin* parent = bin ? bin->parent_bin : nullptr;
  if (!parent) {
    if (!bin) {
      return FALSE;
    }
    g_mutex_lock(&bin->uri_playlist_mutex);
    const gboolean released = !bin->uri_list_permanently_ended;
    g_mutex_unlock(&bin->uri_playlist_mutex);
    return released;
  }

  gboolean report_failure = FALSE;
  g_mutex_lock(&parent->uri_playlist_barrier_mutex);
  if (parent->uri_playlist_terminal) {
    g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
    return FALSE;
  }
  if (!parent->uri_playlist_exact_pairing_enabled) {
    g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
    return TRUE;
  }
  bin->uri_list_frame_ready_sequence = sequence;
  bin->uri_list_released_video_end = logical_video_end;
  const std::vector<NvDsSrcBin*> sources = uri_playlist_sources(parent);
  if (sources.size() != 2 || !GST_CLOCK_TIME_IS_VALID(logical_video_end)) {
    parent->uri_playlist_barrier_failed = TRUE;
    parent->uri_playlist_terminal = TRUE;
    for (NvDsSrcBin* source : sources) {
      source->uri_list_permanently_ended = TRUE;
      source->uri_switch_pending = FALSE;
    }
    g_cond_broadcast(&parent->uri_playlist_barrier_cond);
    report_failure = TRUE;
  }
  if (!report_failure) {
    const bool all_sources_ready = std::all_of(sources.begin(), sources.end(), [sequence](const NvDsSrcBin* source) {
      return source->uri_list_frame_ready_sequence == sequence;
    });
    if (all_sources_ready) {
      const gboolean valid_pair_ends = std::all_of(sources.begin(), sources.end(), [](const NvDsSrcBin* source) {
        return GST_CLOCK_TIME_IS_VALID(source->uri_list_released_video_end);
      });
      const GstClockTime paired_end = valid_pair_ends
          ? std::min(sources[0]->uri_list_released_video_end, sources[1]->uri_list_released_video_end)
          : GST_CLOCK_TIME_NONE;
      const gboolean advancing_frontier = GST_CLOCK_TIME_IS_VALID(paired_end) &&
          (!GST_CLOCK_TIME_IS_VALID(parent->uri_playlist_paired_video_end) ||
           paired_end > parent->uri_playlist_paired_video_end);
      if (parent->uri_playlist_next_frame_sequence == sequence && advancing_frontier) {
        // Publish sequence N's fully paired video endpoint before releasing either N buffer. Terminal EOS therefore
        // cannot snapshot an N-1 audio cutoff after N has already become a stitchable output pair.
        parent->uri_playlist_paired_video_end = paired_end;
        ++parent->uri_playlist_next_frame_sequence;
      } else {
        parent->uri_playlist_barrier_failed = TRUE;
        parent->uri_playlist_terminal = TRUE;
        for (NvDsSrcBin* source : sources) {
          source->uri_list_permanently_ended = TRUE;
          source->uri_switch_pending = FALSE;
        }
        report_failure = TRUE;
      }
      g_cond_broadcast(&parent->uri_playlist_barrier_cond);
    } else {
      const gint64 deadline = g_get_monotonic_time() + kUriPlaylistBarrierWaitUs;
      while (!parent->uri_playlist_terminal && parent->uri_playlist_next_frame_sequence <= sequence) {
        if (!g_cond_wait_until(&parent->uri_playlist_barrier_cond, &parent->uri_playlist_barrier_mutex, deadline)) {
          parent->uri_playlist_barrier_failed = TRUE;
          parent->uri_playlist_terminal = TRUE;
          for (NvDsSrcBin* source : sources) {
            source->uri_list_permanently_ended = TRUE;
            source->uri_switch_pending = FALSE;
          }
          g_cond_broadcast(&parent->uri_playlist_barrier_cond);
          report_failure = TRUE;
          break;
        }
      }
    }
  }
  const gboolean released = !parent->uri_playlist_terminal && parent->uri_playlist_next_frame_sequence > sequence;
  g_mutex_unlock(&parent->uri_playlist_barrier_mutex);
  if (report_failure) {
    GST_ELEMENT_ERROR(
        bin->src_elem,
        STREAM,
        FAILED,
        ("Lossless camera frame barrier failed"),
        ("source=%u sequence=%" G_GUINT64_FORMAT " participants=%zu", bin->source_id, sequence, sources.size()));
  }
  return released;
}

static void maybe_complete_uri_playlist_boundary(NvDsSrcBin* bin, guint uri_index) {
  if (!bin) {
    return;
  }
  gboolean schedule_switch = FALSE;
  gboolean end_playlist = FALSE;
  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  const gboolean terminal = bin->parent_bin ? bin->parent_bin->uri_playlist_terminal : bin->uri_list_permanently_ended;
  const gboolean boundary_complete = bin->uri_list_video_eos_seen && bin->uri_list_pads_complete &&
      (!bin->uri_audio_has_pad || bin->uri_audio_eos_seen);
  if (!terminal && uri_index == bin->uri_list_index && boundary_complete && !bin->uri_list_boundary_handled) {
    bin->uri_list_boundary_handled = TRUE;
    const gboolean has_next = bin->uri_list_index + 1 < bin->num_uri_list;
    const gboolean will_loop = bin->config && bin->config->uri_list_loop;
    if (has_next || will_loop) {
      advance_uri_playlist_base_locked(bin);
      bin->uri_switch_pending = TRUE;
      schedule_switch = TRUE;
    } else {
      end_playlist = TRUE;
    }
  }
  g_mutex_unlock(mutex);
  if (schedule_switch) {
    g_timeout_add(1, switch_to_next_uri, bin);
  } else if (end_playlist) {
    mark_uri_playlist_terminal(bin);
  }
}

namespace hm {

namespace {
std::string normalize_type_string(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c); };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  std::replace(s.begin(), s.end(), '-', '_');
  return s;
}
} // namespace

// Converts enum value to string
std::string to_string(const NvDsSourceType& type) {
  switch (type) {
    case NV_DS_SOURCE_CAMERA_V4L2:
      return "V4L2";
    case NV_DS_SOURCE_URI:
      return "URI";
    case NV_DS_SOURCE_URI_MULTIPLE:
      return "URI-MULTIPLE";
    case NV_DS_SOURCE_RTSP:
      return "RTSP";
    case NV_DS_SOURCE_CAMERA_CSI:
      return "CSI";
    case NV_DS_SOURCE_AUDIO_WAV:
      return "AUDIO-WAV";
    case NV_DS_SOURCE_AUDIO_URI:
      return "AUDIO-URI";
    case NV_DS_SOURCE_ALSA_SRC:
      return "ALSA";
    case NV_DS_SOURCE_IPC:
      return "IPC";
    default:
      return "INVALID";
  }
}

// Converts string to enum value and returns std::optional
std::optional<NvDsSourceType> source_type_from_string(const std::string& str) {
  const std::string s = normalize_type_string(str);
  if (s == "V4L2")
    return NV_DS_SOURCE_CAMERA_V4L2;
  if (s == "URI")
    return NV_DS_SOURCE_URI;
  if (s == "URI_MULTIPLE")
    return NV_DS_SOURCE_URI_MULTIPLE;
  if (s == "RTSP")
    return NV_DS_SOURCE_RTSP;
  if (s == "RTMP")
    return NV_DS_SOURCE_RTSP;
  if (s == "CSI")
    return NV_DS_SOURCE_CAMERA_CSI;
  if (s == "AUDIO_WAV")
    return NV_DS_SOURCE_AUDIO_WAV;
  if (s == "AUDIO_URI")
    return NV_DS_SOURCE_AUDIO_URI;
  if (s == "ALSA")
    return NV_DS_SOURCE_ALSA_SRC;
  if (s == "IPC")
    return NV_DS_SOURCE_IPC;

  // Return an empty optional if no match was found.
  return std::nullopt;
}
} // namespace hm

#if 1
/* Functions below print the Capabilities in a human-friendly format */
[[maybe_unused]] static gboolean print_field(GQuark field, const GValue* value, gpointer pfx) {
  gchar* str = gst_value_serialize(value);

  g_print("%s  %15s: %s\n", (gchar*)pfx, g_quark_to_string(field), str);
  g_free(str);
  return TRUE;
}

// static void print_caps(const GstCaps* caps, const gchar* pfx) {
//   guint i;

//   g_return_if_fail(caps != NULL);

//   if (gst_caps_is_any(caps)) {
//     g_print("%sANY\n", pfx);
//     return;
//   }
//   if (gst_caps_is_empty(caps)) {
//     g_print("%sEMPTY\n", pfx);
//     return;
//   }

//   for (i = 0; i < gst_caps_get_size(caps); i++) {
//     GstStructure* structure = gst_caps_get_structure(caps, i);

//     g_print("%s%s\n", pfx, gst_structure_get_name(structure));
//     gst_structure_foreach(structure, print_field, (gpointer)pfx);
//   }
// }
#endif

/**
 * Prints the current negotiated caps of a pad on a GstElement
 *
 * @param element The GstElement whose pad caps to print
 * @param padName The name of the pad (e.g., "src", "sink")
 * @return true if caps were successfully printed, false otherwise
 */
bool print_pad_caps(GstElement* element, const char* padName) {
  if (!element) {
    std::cerr << "Element is NULL" << std::endl;
    return false;
  }

  // Get the pad
  GstPad* pad = gst_element_get_static_pad(element, padName);
  if (!pad) {
    std::cerr << "Failed to get " << padName << " pad from element " << GST_ELEMENT_NAME(element) << std::endl;
    return false;
  }

  // Get the current caps
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) {
    std::cout << "No caps currently negotiated on " << padName << " pad of " << GST_ELEMENT_NAME(element) << std::endl;
    gst_object_unref(pad);
    return false;
  }

  // Convert caps to string and print
  gchar* capsStr = gst_caps_to_string(caps);
  std::cout << "Caps on " << GST_ELEMENT_NAME(element) << ":" << padName << " = " << capsStr << std::endl;

  // Also print in a more detailed format
  std::cout << "Detailed caps:" << std::endl;
  for (guint i = 0; i < gst_caps_get_size(caps); i++) {
    GstStructure* structure = gst_caps_get_structure(caps, i);
    gchar* structStr = gst_structure_to_string(structure);
    std::cout << "  Structure " << i << ": " << structStr << std::endl;
    g_free(structStr);
  }

  // Free resources
  g_free(capsStr);
  gst_caps_unref(caps);
  gst_object_unref(pad);

  return true;
}

void print_pads(GstElement* element) {
  GstIterator* iter;
  GstPad* pad;
  GValue item = G_VALUE_INIT;
  GEnumValue* pad_direction;

  if (!element) {
    return;
  }

  g_print("Pads for element: %s\n", GST_ELEMENT_NAME(element));

  iter = gst_element_iterate_pads(element);
  while (gst_iterator_next(iter, &item) == GST_ITERATOR_OK) {
    pad = GST_PAD(g_value_get_object(&item));
    pad_direction =
        g_enum_get_value((GEnumClass*)g_type_class_peek(GST_TYPE_PAD_DIRECTION), gst_pad_get_direction(pad));

    g_print("  Pad: %s (%s)\n", GST_PAD_NAME(pad), pad_direction->value_nick);
    g_value_unset(&item);
  }
  gst_iterator_free(iter);
}

static gboolean set_camera_csi_params(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  g_object_set(G_OBJECT(bin->src_elem), "sensor-id", config->camera_csi_sensor_id, NULL);
  if (config->camera_wbmode) {
    g_object_set(G_OBJECT(bin->src_elem), "wbmode", config->camera_wbmode, NULL);
  }
  if (config->exposure_time_range) {
    g_object_set(G_OBJECT(bin->src_elem), "exposuretimerange", config->exposure_time_range, NULL);
  }
  if (config->gain_range) {
    g_object_set(G_OBJECT(bin->src_elem), "gainrange", config->gain_range, NULL);
  }
  if (config->camera_saturation != 0.0) {
    g_object_set(G_OBJECT(bin->src_elem), "saturation", config->camera_saturation, NULL);
  }
  if (config->camera_exposure_compensation != 0.0) {
    g_object_set(G_OBJECT(bin->src_elem), "exposurecompensation", config->camera_exposure_compensation, NULL);
  }
  if (config->camera_num_buffers) {
    g_object_set(G_OBJECT(bin->src_elem), "num-buffers", config->camera_num_buffers, NULL);
  }
  g_object_set(G_OBJECT(bin->src_elem), "ee-mode", 2, NULL);
  GST_CAT_DEBUG(NVDS_APP, "Setting csi camera params successful");
  return TRUE;
}

static gboolean set_camera_v4l2_params(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  gchar device[64];

  g_snprintf(device, sizeof(device), "/dev/video%d", config->camera_v4l2_dev_node);
  g_object_set(G_OBJECT(bin->src_elem), "device", device, NULL);

  GST_CAT_DEBUG(NVDS_APP, "Setting v4l2 camera params successful");

  return TRUE;
}

static void set_videoconvert_params(const NvDsSourceConfig* config, NvDsSrcBin* bin) {
  if (!bin->nvvidconv) {
    return;
  }
  if (config->flip_method) {
    g_object_set(G_OBJECT(bin->nvvidconv), "flip-method", config->flip_method, NULL);
  }
}

static gboolean create_camera_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  GstCaps *caps = NULL, *caps1 = NULL, *convertCaps = NULL;
  gboolean ret = FALSE;

  bool is_jpeg = false;

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_CSI:
      bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_CAMERA_CSI, "csi_src_elem");
      break;
    case NV_DS_SOURCE_CAMERA_V4L2:
      bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_CAMERA_V4L2, "v4l2_src_elem");
      bin->cap_filter1 = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter1");
      if (!bin->cap_filter1) {
        NVGSTDS_ERR_MSG_V("Could not create 'src_cap_filter1'");
        goto done;
      }
      is_jpeg = config->media_type && !strncmp(config->media_type, "image/jpeg", 10);
      if (is_jpeg) {
        caps1 = gst_caps_new_simple(
            // config->media_type ? config->media_type : "video/x-raw",
            "video/x-raw",
            //"parsed",
            // G_TYPE_BOOLEAN,
            // TRUE,
            "width",
            G_TYPE_INT,
            config->camera_width,
            "height",
            G_TYPE_INT,
            config->camera_height,
            "framerate",
            GST_TYPE_FRACTION,
            config->camera_fps_n,
            config->camera_fps_d,
            NULL);
      } else {
        caps1 = gst_caps_new_simple(
            "video/x-raw",
            "width",
            G_TYPE_INT,
            config->camera_width,
            "height",
            G_TYPE_INT,
            config->camera_height,
            "framerate",
            GST_TYPE_FRACTION,
            config->camera_fps_n,
            config->camera_fps_d,
            NULL);
      }
      break;
    default:
      NVGSTDS_ERR_MSG_V("Unsupported source type");
      goto done;
  }

  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V("Could not create 'src_elem'");
    goto done;
  }

  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Could not create 'src_cap_filter'");
    goto done;
  }

  if (config->video_format) {
    caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        config->video_format,
        "width",
        G_TYPE_INT,
        config->camera_width,
        "height",
        G_TYPE_INT,
        config->camera_height,
        "framerate",
        GST_TYPE_FRACTION,
        config->camera_fps_n,
        config->camera_fps_d,
        NULL);
  } else {
    caps = gst_caps_new_simple(
        // config->media_type ? config->media_type : "video/x-raw",
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "NV12",
        "width",
        G_TYPE_INT,
        config->camera_width,
        "height",
        G_TYPE_INT,
        config->camera_height,
        "framerate",
        GST_TYPE_FRACTION,
        config->camera_fps_n,
        config->camera_fps_d,
        NULL);
  }

  if (is_jpeg) {
    bin->src_decoder = gst_element_factory_make("jpegdec", "src_jpegdec");
    if (!bin->src_decoder) {
      NVGSTDS_ERR_MSG_V("Failed to create 'src_jpegdec'");
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->src_decoder);
  } else if (config->media_type && strstr(config->media_type, "h264")) {
    // assert(!bin->src_parser);
    bin->src_parser = gst_element_factory_make("h264parse", "cam_h264parse");
    if (!bin->src_parser) {
      NVGSTDS_ERR_MSG_V("Failed to create 'cam_h264parse'");
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->src_parser);
    bin->src_decoder = gst_element_factory_make("avdec_h264", "cam_h264_decoder");
    if (!bin->src_decoder) {
      NVGSTDS_ERR_MSG_V("Failed to create 'cam_decodebin'");
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->src_decoder);
    // NVGSTDS_LINK_ELEMENT(bin->parser, bin->src_decoder);
  }

  if (config->type == NV_DS_SOURCE_CAMERA_CSI) {
    GstCapsFeatures* feature = NULL;
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(caps, 0, feature);
  }

  // if (caps1) {
  //   print_caps(caps1, "caps1");
  // }
  // print_caps(caps, "caps");

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, config->gpu_id);

  if (config->type == NV_DS_SOURCE_CAMERA_V4L2) {
    GstElement* nvvidconv2;
    GstCapsFeatures* feature = NULL;
    // Check based on igpu/dgpu instead of x86/aarch64
    GstElement* nvvidconv1 = NULL;
    if (!prop.integrated) {
      nvvidconv1 = gst_element_factory_make("videoconvert", "nvvidconv1");
      if (!nvvidconv1) {
        NVGSTDS_ERR_MSG_V("Failed to create 'nvvidconv1'");
        goto done;
      }
    }

    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(caps, 0, feature);
    g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);

    g_object_set(G_OBJECT(bin->cap_filter1), "caps", caps1, NULL);

    nvvidconv2 = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "nvvidconv2");
    if (!nvvidconv2) {
      NVGSTDS_ERR_MSG_V("Failed to create 'nvvidconv2'");
      goto done;
    }

    g_object_set(G_OBJECT(nvvidconv2), "gpu-id", config->gpu_id, "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

    if (!prop.integrated) {
      gst_bin_add_many(
          GST_BIN(bin->bin), bin->src_elem, bin->cap_filter1, nvvidconv1, nvvidconv2, bin->cap_filter, NULL);
    } else {
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
      /* For Jetson, with copy-hw=1 and memory-type=nvbuf-mem-surface-array,
         cudaMemcopy fail is observed. This is a WAR till root cause is fixed */
      if (config->type == NV_DS_SOURCE_CAMERA_V4L2) {
        g_object_set(G_OBJECT(nvvidconv2), "copy-hw", 2, NULL);
      } else {
        if (config->nvvideoconvert_copy_hw) {
          g_object_set(G_OBJECT(nvvidconv2), "copy-hw", config->nvvideoconvert_copy_hw, NULL);
        }
      }
#endif
      gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem, bin->cap_filter1, nvvidconv2, bin->cap_filter, NULL);
    }

    if (bin->src_decoder) {
      // print_pads(bin->src_elem);
      // print_pads(bin->src_decoder);
      if (bin->src_parser) {
        NVGSTDS_LINK_ELEMENT(bin->src_elem, bin->src_parser);
        NVGSTDS_LINK_ELEMENT(bin->src_parser, bin->src_decoder);
      } else {
        NVGSTDS_LINK_ELEMENT(bin->src_elem, bin->src_decoder);
      }
      NVGSTDS_LINK_ELEMENT(bin->src_decoder, bin->cap_filter1);
    } else {
      NVGSTDS_LINK_ELEMENT(bin->src_elem, bin->cap_filter1);
    }

    if (!prop.integrated) {
      NVGSTDS_LINK_ELEMENT(bin->cap_filter1, nvvidconv1);

      NVGSTDS_LINK_ELEMENT(nvvidconv1, nvvidconv2);
    } else {
      NVGSTDS_LINK_ELEMENT(bin->cap_filter1, nvvidconv2);
    }

    NVGSTDS_LINK_ELEMENT(nvvidconv2, bin->cap_filter);

    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter, "src");

  } else {
    bin->nvvidconv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "nvvidconv");

    g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);

    gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem, bin->nvvidconv, bin->cap_filter, NULL);

    NVGSTDS_LINK_ELEMENT(bin->src_elem, bin->nvvidconv);
    NVGSTDS_LINK_ELEMENT(bin->nvvidconv, bin->cap_filter);

    // NVGSTDS_LINK_ELEMENT(bin->src_elem, bin->cap_filter);

    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter, "src");
  }

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_CSI:
      if (!set_camera_csi_params(config, bin)) {
        NVGSTDS_ERR_MSG_V("Could not set CSI camera properties");
        goto done;
      }
      break;
    case NV_DS_SOURCE_CAMERA_V4L2:
      if (!set_camera_v4l2_params(config, bin)) {
        NVGSTDS_ERR_MSG_V("Could not set V4L2 camera properties");
        goto done;
      }
      break;
    default:
      NVGSTDS_ERR_MSG_V("Unsupported source type");
      goto done;
  }

  hm::save_dot_file(bin->bin, GstDebugGraphDetails::GST_DEBUG_GRAPH_SHOW_ALL, "camera_bin");

  ret = TRUE;

  GST_CAT_DEBUG(NVDS_APP, "Created camera source bin successfully");

done:
  if (caps)
    gst_caps_unref(caps);

  if (convertCaps)
    gst_caps_unref(convertCaps);

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static GstPadProbeReturn uri_list_video_pad_event_probe(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  (void)pad;
  auto* probe_data = static_cast<UriListPadProbeData*>(u_data);
  NvDsSrcBin* bin = probe_data ? probe_data->bin : nullptr;
  if (!bin) {
    return GST_PAD_PROBE_OK;
  }

  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
    if (!probe_data->is_video) {
      const GstPadProbeReturn audio_gate_result = gate_uri_playlist_audio_buffer(pad, info, probe_data);
      if (audio_gate_result != GST_PAD_PROBE_OK) {
        return audio_gate_result;
      }
    }
    GstBuffer* buf = GST_BUFFER(info->data);
    GMutex* mutex = uri_playlist_mutex(bin);
    g_mutex_lock(mutex);
    const gboolean terminal = uri_playlist_terminal_locked(bin);
    const gboolean drain_terminal_audio = !probe_data->is_video && terminal && bin->uri_terminal_audio_drain_pending;
    const gboolean is_current_uri = probe_data->uri_index == bin->uri_list_index;
    const guint64 decoded_sequence = bin->uri_list_decoded_frame_count;
    g_mutex_unlock(mutex);
    if (!probe_data->is_video && terminal && !drain_terminal_audio) {
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video && !is_current_uri) {
      // A pad from a decoder that has already been retired must never leak a late frame into the next chapter.
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video) {
      if (!gst_buffer_is_writable(buf)) {
        buf = gst_buffer_make_writable(buf);
        if (buf) {
          GST_PAD_PROBE_INFO_DATA(info) = buf;
        }
      }
      if (buf && probe_data->uri_index > 0) {
        // The demuxer marks the first buffer of each physical file DISCONT/RESYNC. For a logical camera playlist that
        // is not a discontinuity, and nvstreammux may flush the frame it already has from the peer camera when it sees
        // the flag. Timestamp rebasing plus the exact-sequence barrier provide the real continuity contract.
        GST_BUFFER_FLAG_UNSET(buf, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_FLAG_UNSET(buf, GST_BUFFER_FLAG_RESYNC);
        GST_BUFFER_FLAG_UNSET(buf, GST_BUFFER_FLAG_HEADER);
      }
      if (!buf || !hm::add_decoded_frame_sequence_meta(buf, bin->source_id, decoded_sequence)) {
        cancel_uri_playlist_source(bin, TRUE);
        GST_ELEMENT_ERROR(
            bin->src_elem,
            STREAM,
            FAILED,
            ("Could not attach the lossless decoded-frame sequence"),
            ("source=%u sequence=%" G_GUINT64_FORMAT, bin->source_id, decoded_sequence));
        return GST_PAD_PROBE_DROP;
      }
      GstClockTime video_duration = GST_BUFFER_DURATION(buf);
      if (!GST_CLOCK_TIME_IS_VALID(video_duration) && bin->config && bin->config->camera_fps_n > 0 &&
          bin->config->camera_fps_d > 0) {
        video_duration = gst_util_uint64_scale(GST_SECOND, bin->config->camera_fps_d, bin->config->camera_fps_n);
      }
      const GstClockTime logical_video_end =
          GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf)) && GST_CLOCK_TIME_IS_VALID(video_duration)
          ? GST_BUFFER_PTS(buf) + probe_data->base + video_duration
          : GST_CLOCK_TIME_NONE;
      if (!wait_at_uri_playlist_frame_barrier(bin, decoded_sequence, logical_video_end)) {
        g_mutex_lock(mutex);
        ++bin->uri_list_decoded_frame_count;
        if (uri_playlist_terminal_locked(bin)) {
          ++bin->uri_list_terminal_dropped_frame_count;
        }
        g_mutex_unlock(mutex);
        return GST_PAD_PROBE_DROP;
      }
    }
    if (probe_data->is_video) {
      g_mutex_lock(mutex);
      if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf))) {
        bin->uri_list_last_pts = GST_BUFFER_PTS(buf);
        bin->uri_list_last_duration = GST_BUFFER_DURATION(buf);
      }
      ++bin->uri_list_decoded_frame_count;
      g_mutex_unlock(mutex);
    }
    if (probe_data->base != 0 && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf))) {
      GST_BUFFER_PTS(buf) += probe_data->base;
    }
    if (probe_data->base != 0 && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(buf))) {
      GST_BUFFER_DTS(buf) += probe_data->base;
    }
  }
  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
    GstEvent* event = GST_EVENT(info->data);
    if (probe_data->uri_index > 0 && GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START) {
      // Every URI is a chapter of one logical camera stream. Exposing an intermediate STREAM_START makes
      // nvstreammux reset that source and flush a partial batch even though no camera actually ended.
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->uri_index > 0 && GST_EVENT_TYPE(event) == GST_EVENT_TAG) {
      // Per-file container tags are not a logical camera boundary. Some nvstreammux releases finalize the current
      // aggregate while forwarding sticky metadata, so retain only the first chapter's tag set.
      return GST_PAD_PROBE_DROP;
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_GROUP_DONE) {
      // This closes a decodebin stream group, not the logical camera. nvstreammux must see only the synthetic terminal
      // EOS after one camera's entire playlist is permanently exhausted.
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video && probe_data->uri_index > 0 && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps* chapter_caps = nullptr;
      gst_event_parse_caps(event, &chapter_caps);
      GstPad* tee_sink = gst_element_get_static_pad(bin->tee, "sink");
      GstCaps* logical_caps = tee_sink ? gst_pad_get_current_caps(tee_sink) : nullptr;
      const gboolean caps_match = chapter_caps && logical_caps && gst_caps_is_equal(chapter_caps, logical_caps);
      if (logical_caps) {
        gst_caps_unref(logical_caps);
      }
      if (tee_sink) {
        gst_object_unref(tee_sink);
      }
      if (!caps_match) {
        cancel_uri_playlist_source(bin, TRUE);
        GST_ELEMENT_ERROR(
            bin->src_elem,
            STREAM,
            FORMAT,
            ("Camera chapter video format changed"),
            ("source=%u uri_index=%u", bin->source_id, probe_data->uri_index));
      }
      // Equal caps are already sticky on the logical source. Re-emitting them can make nvstreammux close a batch at
      // a file boundary; changed caps are a hard error because the playlist can no longer be one lossless stream.
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video && probe_data->uri_index > 0 && GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      if (segment) {
        GstClockTime stop = segment->stop;
        if (stop == GST_CLOCK_TIME_NONE) {
          stop = segment->duration;
        }
        if (stop != GST_CLOCK_TIME_NONE) {
          GMutex* mutex = uri_playlist_mutex(bin);
          g_mutex_lock(mutex);
          if (probe_data->uri_index == bin->uri_list_index) {
            bin->uri_list_segment_stop = stop;
          }
          g_mutex_unlock(mutex);
        }
      }
      // Buffer PTS/DTS are rebased below, so a new SEGMENT would be a false logical reset. nvstreammux can flush its
      // current camera pair on this event even when its timeout is effectively infinite.
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->uri_index > 0 &&
        (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START || GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP)) {
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video && GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
      GMutex* mutex = uri_playlist_mutex(bin);
      g_mutex_lock(mutex);
      const gboolean is_current_uri = probe_data->uri_index == bin->uri_list_index;
      if (is_current_uri) {
        bin->uri_list_video_eos_seen = TRUE;
        if (bin->parent_bin) {
          g_cond_broadcast(&bin->parent_bin->uri_playlist_barrier_cond);
        }
      }
      g_mutex_unlock(mutex);
      if (is_current_uri) {
        // EOS has passed through the decoder, so every delayed frame from this chapter reached the frame barrier.
        // Do not expose either intermediate or final decoder EOS: final EOS is synthesized only after this URI's
        // video and audio have both drained, and then coordinated across both cameras.
        maybe_complete_uri_playlist_boundary(bin, probe_data->uri_index);
      }
      return GST_PAD_PROBE_DROP;
    }
    if (probe_data->is_video &&
        (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START || GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP)) {
      GMutex* mutex = uri_playlist_mutex(bin);
      g_mutex_lock(mutex);
      const gboolean switch_pending = bin->uri_switch_pending;
      g_mutex_unlock(mutex);
      if (switch_pending) {
        return GST_PAD_PROBE_DROP;
      }
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT && probe_data->base != 0) {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      if (segment && segment->format == GST_FORMAT_TIME) {
        if (probe_data->is_video) {
          GstClockTime stop = segment->stop;
          if (stop == GST_CLOCK_TIME_NONE) {
            stop = segment->duration;
          }
          if (stop != GST_CLOCK_TIME_NONE) {
            GMutex* mutex = uri_playlist_mutex(bin);
            g_mutex_lock(mutex);
            if (probe_data->uri_index == bin->uri_list_index) {
              bin->uri_list_segment_stop = stop;
            }
            g_mutex_unlock(mutex);
          }
        }
        GstSegment adjusted;
        gst_segment_copy_into(segment, &adjusted);
        if (GST_CLOCK_TIME_IS_VALID(adjusted.base))
          adjusted.base += probe_data->base;
        if (GST_CLOCK_TIME_IS_VALID(adjusted.start))
          adjusted.start += probe_data->base;
        if (GST_CLOCK_TIME_IS_VALID(adjusted.stop))
          adjusted.stop += probe_data->base;
        if (GST_CLOCK_TIME_IS_VALID(adjusted.time))
          adjusted.time += probe_data->base;
        if (GST_CLOCK_TIME_IS_VALID(adjusted.position))
          adjusted.position += probe_data->base;
        GstEvent* adjusted_event = gst_event_new_segment(&adjusted);
        gst_event_set_seqnum(adjusted_event, gst_event_get_seqnum(event));
        gst_event_unref(event);
        GST_PAD_PROBE_INFO_DATA(info) = adjusted_event;
      }
      return GST_PAD_PROBE_OK;
    }
    if (probe_data->is_video && GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      if (segment) {
        GstClockTime stop = segment->stop;
        if (stop == GST_CLOCK_TIME_NONE) {
          stop = segment->duration;
        }
        if (stop != GST_CLOCK_TIME_NONE) {
          GMutex* mutex = uri_playlist_mutex(bin);
          g_mutex_lock(mutex);
          if (probe_data->uri_index == bin->uri_list_index) {
            bin->uri_list_segment_stop = stop;
          }
          g_mutex_unlock(mutex);
        }
      }
    }
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn uri_list_audio_pad_event_probe(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  GstPadProbeReturn ret = uri_list_video_pad_event_probe(pad, info, u_data);
  auto* probe_data = static_cast<UriListPadProbeData*>(u_data);
  NvDsSrcBin* bin = probe_data ? probe_data->bin : nullptr;
  if (!bin) {
    return ret;
  }

  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
    GstEvent* event = GST_EVENT(info->data);

    if (GST_EVENT_TYPE(event) != GST_EVENT_EOS) {
      return ret;
    }
    GMutex* mutex = uri_playlist_mutex(bin);
    g_mutex_lock(mutex);
    const gboolean is_current_uri = probe_data->uri_index == bin->uri_list_index;
    const gboolean terminal_drain_pending = uri_playlist_terminal_locked(bin) && bin->uri_terminal_audio_drain_pending;
    if (is_current_uri) {
      bin->uri_audio_eos_seen = TRUE;
    }
    g_mutex_unlock(mutex);
    if (is_current_uri) {
      if (terminal_drain_pending) {
        g_idle_add(finish_uri_terminal_audio_drain, bin);
      } else {
        maybe_complete_uri_playlist_boundary(bin, probe_data->uri_index);
      }
    }
    // Raw chapter EOS must never close the shared audio branch. A serialized EOS is emitted after the full current
    // URI (or a peer camera's permanent end) has been coordinated.
    return GST_PAD_PROBE_DROP;
  }
  return ret;
}

static void cb_newpad(GstElement* decodebin, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_query_caps(pad, NULL);
  const GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (!strncmp(name, "video", 5)) {
    NvDsSrcBin* bin = (NvDsSrcBin*)data;
    if (bin->uri_list && bin->num_uri_list >= 1) {
      auto* probe_data = g_new0(UriListPadProbeData, 1);
      probe_data->bin = bin;
      GMutex* mutex = uri_playlist_mutex(bin);
      g_mutex_lock(mutex);
      probe_data->uri_index = bin->uri_list_index;
      probe_data->base = bin->prev_accumulated_base;
      g_mutex_unlock(mutex);
      probe_data->is_video = TRUE;
      gst_pad_add_probe(
          pad,
          (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER),
          uri_list_video_pad_event_probe,
          probe_data,
          reinterpret_cast<GDestroyNotify>(g_free));
    }
    GstPad* sinkpad = gst_element_get_static_pad(bin->tee, "sink");
    if (gst_pad_is_linked(sinkpad)) {
      GstPad* peer = gst_pad_get_peer(sinkpad);
      if (peer) {
        gst_pad_unlink(peer, sinkpad);
        gst_object_unref(peer);
      }
    }
    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
      NVGSTDS_ERR_MSG_V("Failed to link decodebin to pipeline");
      cancel_uri_playlist_source(bin, TRUE);
      GST_ELEMENT_ERROR(bin->src_elem, STREAM, FAILED, ("Failed to link decoded video"), ("source=%u", bin->source_id));
    } else {
      NvDsSourceConfig* config = (NvDsSourceConfig*)g_object_get_data(G_OBJECT(bin->cap_filter), SRC_CONFIG_KEY);

      gst_structure_get_int(str, "width", &config->camera_width);
      gst_structure_get_int(str, "height", &config->camera_height);
      gst_structure_get_fraction(str, "framerate", &config->camera_fps_n, &config->camera_fps_d);

      GST_CAT_DEBUG(NVDS_APP, "Decodebin linked to pipeline");
    }
    gst_object_unref(sinkpad);
  } else if (g_str_has_prefix(name, "audio/x-raw")) {
    NvDsSrcBin* bin = (NvDsSrcBin*)data;

    if (bin->uri_audio_tee) {
      if (bin->uri_list && bin->num_uri_list >= 1) {
        auto* probe_data = g_new0(UriListPadProbeData, 1);
        probe_data->bin = bin;
        GMutex* mutex = uri_playlist_mutex(bin);
        g_mutex_lock(mutex);
        probe_data->uri_index = bin->uri_list_index;
        probe_data->base = bin->prev_accumulated_base;
        g_mutex_unlock(mutex);
        probe_data->is_video = FALSE;
        gst_pad_add_probe(
            pad,
            (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER),
            uri_list_audio_pad_event_probe,
            probe_data,
            reinterpret_cast<GDestroyNotify>(g_free));
      }
      if (bin->uri_list && bin->num_uri_list >= 1) {
        GMutex* mutex = uri_playlist_mutex(bin);
        g_mutex_lock(mutex);
        bin->uri_audio_has_pad = TRUE;
        g_mutex_unlock(mutex);
      }

      GstPad* sinkpad = gst_element_get_static_pad(bin->uri_audio_tee, "sink");
      if (!sinkpad) {
        gst_caps_unref(caps);
        return;
      }
      if (gst_pad_is_linked(sinkpad)) {
        GstPad* peer = gst_pad_get_peer(sinkpad);
        if (peer) {
          gst_pad_unlink(peer, sinkpad);
          gst_object_unref(peer);
        }
      }
      if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
        NVGSTDS_ERR_MSG_V("Failed to link URI decodebin audio pad to audio tee");
        cancel_uri_playlist_source(bin, TRUE);
        GST_ELEMENT_ERROR(
            bin->src_elem, STREAM, FAILED, ("Failed to link decoded audio"), ("source=%u", bin->source_id));
      }
      gst_object_unref(sinkpad);
      gst_caps_unref(caps);
      return;
    }

    /** skip linking if we did not prepare for audio */
    if (!bin->audio_converter) {
      return;
    }

    GstPad* sinkpad = gst_element_get_static_pad(bin->audio_converter, "sink");

    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK)
      NVGSTDS_ERR_MSG_V("Failed to link decodebin to pipeline");
    gst_object_unref(sinkpad);
  }
}

static void cb_newpad_audio(GstElement* decodebin, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_query_caps(pad, NULL);
  const GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (g_str_has_prefix(name, "audio/x-raw")) {
    NvDsSrcBin* bin = (NvDsSrcBin*)data;

    GstPad* sinkpad = gst_element_get_static_pad(bin->audio_converter, "sink");

    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK)
      NVGSTDS_ERR_MSG_V("Failed to link decodebin to pipeline");
    gst_object_unref(sinkpad);
  } else if (!strncmp(name, "video", 5)) {
    /** connect video to fakesink and ignore the same */
    NvDsSrcBin* bin = (NvDsSrcBin*)data;
    bin->fakesink = gst_element_factory_make("fakesink", "src_fakesink");
    if (!bin->fakesink) {
      NVGSTDS_ERR_MSG_V("Could not create 'src_fakesink' for video path");
      return;
    }

    g_object_set(G_OBJECT(bin->fakesink), "sync", FALSE, "async", FALSE, NULL);
    gst_bin_add_many(GST_BIN(bin->bin), bin->fakesink, NULL);

    GstPad* sinkpad = gst_element_get_static_pad(bin->fakesink, "sink");

    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK)
      NVGSTDS_ERR_MSG_V("Failed to link decodebin to pipeline");
    gst_object_unref(sinkpad);
  }
  gst_caps_unref(caps);
}

static void cb_sourcesetup(GstElement* object, GstElement* arg0, gpointer data) {
  NvDsSrcBin* bin = (NvDsSrcBin*)data;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(arg0), "latency")) {
    g_object_set(G_OBJECT(arg0), "latency", bin->latency, NULL);
  }
  if (bin->udp_buffer_size && g_object_class_find_property(G_OBJECT_GET_CLASS(arg0), "udp-buffer-size")) {
    g_object_set(G_OBJECT(arg0), "udp-buffer-size", bin->udp_buffer_size, NULL);
  }
}

/*
 * Function to seek the source stream to start.
 * It is required to play the stream in loop.
 */
static gboolean seek_decode(gpointer data) {
  NvDsSrcBin* bin = (NvDsSrcBin*)data;
  gboolean ret = TRUE;

  gst_element_set_state(bin->bin, GST_STATE_PAUSED);

  ret = gst_element_seek(
      bin->bin,
      1.0,
      GST_FORMAT_TIME,
      (GstSeekFlags)(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH),
      GST_SEEK_TYPE_SET,
      0,
      GST_SEEK_TYPE_NONE,
      GST_CLOCK_TIME_NONE);

  if (!ret)
    GST_WARNING("Error in seeking pipeline");

  gst_element_set_state(bin->bin, GST_STATE_PLAYING);

  return FALSE;
}

static std::vector<std::string> split_semicolon_list(const gchar* input) {
  std::vector<std::string> out;
  if (!input || !*input) {
    return out;
  }
  std::string s(input);
  size_t start = 0;
  while (true) {
    size_t pos = s.find(';', start);
    std::string token = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
    // Trim spaces.
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
      token.erase(token.begin());
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
      token.pop_back();
    if (!token.empty()) {
      out.emplace_back(std::move(token));
    }
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 1;
  }
  return out;
}

static void init_uri_playlist(NvDsSrcBin* bin, NvDsSourceConfig* config) {
  if (!bin || !config) {
    return;
  }
  std::vector<std::string> uris = split_semicolon_list(config->uri_list);
  if (uris.empty() && config->type == NV_DS_SOURCE_URI_MULTIPLE && config->uri && *config->uri) {
    // A one-file camera is still a participant in the exact two-camera barrier. Production configuration historically
    // omitted uri-list for this case, which made a 3-vs-1 game silently exclude the shorter camera from pairing.
    uris.emplace_back(config->uri);
  }
  if (uris.empty()) {
    return;
  }
  g_mutex_init(&bin->uri_playlist_mutex);
  bin->num_uri_list = static_cast<guint>(uris.size());
  bin->uri_list = (gchar**)g_malloc0(sizeof(gchar*) * bin->num_uri_list);
  for (guint i = 0; i < bin->num_uri_list; ++i) {
    bin->uri_list[i] = g_strdup(uris[i].c_str());
  }
  bin->uri_list_index = 0;
  bin->uri_switch_count = 0;
  bin->uri_switch_pending = FALSE;
  bin->uri_list_decoded_frame_count = 0;
  bin->uri_list_released_video_end = GST_CLOCK_TIME_NONE;
  bin->uri_list_terminal_dropped_frame_count = 0;
  bin->uri_list_frame_ready_sequence = G_MAXUINT64;
  bin->uri_list_permanently_ended = FALSE;
  bin->accumulated_base = 0;
  bin->prev_accumulated_base = 0;
  bin->uri_list_segment_stop = GST_CLOCK_TIME_NONE;
  bin->uri_list_last_pts = GST_CLOCK_TIME_NONE;
  bin->uri_list_last_duration = GST_CLOCK_TIME_NONE;
  bin->uri_audio_has_pad = FALSE;
  bin->uri_audio_eos_seen = FALSE;
  bin->uri_terminal_audio_drain_pending = FALSE;
  bin->uri_terminal_audio_cutoff = GST_CLOCK_TIME_NONE;
  bin->uri_list_video_eos_seen = FALSE;
  bin->uri_list_pads_complete = FALSE;
  bin->uri_list_boundary_handled = FALSE;

  // Keep config->uri in sync with the current entry to match file/live detection elsewhere.
  g_free(config->uri);
  config->uri = g_strdup(bin->uri_list[0]);
}

static gboolean switch_to_next_uri(gpointer data) {
  NvDsSrcBin* bin = (NvDsSrcBin*)data;
  if (!bin || !bin->config || !bin->src_elem || !bin->bin || !bin->uri_list || bin->num_uri_list < 2) {
    return FALSE;
  }
  NvDsSourceConfig* config = bin->config;
  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  if (!bin->uri_switch_pending || uri_playlist_terminal_locked(bin)) {
    bin->uri_switch_pending = FALSE;
    g_mutex_unlock(mutex);
    return FALSE;
  }
  guint next_index = bin->uri_list_index + 1;
  if (next_index >= bin->num_uri_list) {
    if (config->uri_list_loop) {
      next_index = 0;
    } else {
      // No next URI to switch to.
      bin->uri_switch_pending = FALSE;
      g_mutex_unlock(mutex);
      return FALSE;
    }
  }
  g_mutex_unlock(mutex);

  if (gst_element_set_state(bin->src_elem, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR_OBJECT(bin->src_elem, "Can't set URI source element to NULL for uri switch");
    cancel_uri_playlist_source(bin, TRUE);
    GST_ELEMENT_ERROR(
        bin->src_elem,
        STREAM,
        FAILED,
        ("Could not stop the current camera chapter"),
        ("source=%u next_uri=%u", bin->source_id, next_index));
    return FALSE;
  }

  // Reset all state before the decoder can expose a pad for the new URI. The frame sequence deliberately does not
  // reset: URI files are chapters in one logical camera stream.
  g_mutex_lock(mutex);
  if (uri_playlist_terminal_locked(bin)) {
    bin->uri_switch_pending = FALSE;
    g_mutex_unlock(mutex);
    return FALSE;
  }
  bin->src_buffer_probe = 0;
  bin->uri_list_index = next_index;
  ++bin->uri_switch_count;
  bin->uri_list_segment_stop = GST_CLOCK_TIME_NONE;
  bin->uri_list_last_pts = GST_CLOCK_TIME_NONE;
  bin->uri_list_last_duration = GST_CLOCK_TIME_NONE;
  bin->uri_audio_has_pad = FALSE;
  bin->uri_audio_eos_seen = FALSE;
  bin->uri_list_video_eos_seen = FALSE;
  bin->uri_list_pads_complete = FALSE;
  bin->uri_list_boundary_handled = FALSE;
  bin->uri_switch_pending = FALSE;
  g_free(config->uri);
  config->uri = g_strdup(bin->uri_list[bin->uri_list_index]);
  const std::string next_uri(config->uri);
  g_mutex_unlock(mutex);

  g_object_set(G_OBJECT(bin->src_elem), "uri", next_uri.c_str(), NULL);

  // Restart only the URI decode element. Keeping the source bin and downstream muxer running avoids exposing a
  // chapter stream reset to nvstreammux.
  if (!gst_element_sync_state_with_parent(bin->src_elem)) {
    GST_ERROR_OBJECT(bin->src_elem, "Couldn't sync URI source element with parent after uri switch");
    cancel_uri_playlist_source(bin, TRUE);
    GST_ELEMENT_ERROR(
        bin->src_elem,
        STREAM,
        FAILED,
        ("Could not start the next camera chapter"),
        ("source=%u uri=%s", bin->source_id, next_uri.c_str()));
    return FALSE;
  }

  // Cancellation can race the state transition without holding the playlist mutex across GStreamer callbacks. If it
  // won that race, immediately retire the decoder instead of allowing the new chapter to produce a frame.
  g_mutex_lock(mutex);
  const gboolean cancelled_during_restart = uri_playlist_terminal_locked(bin);
  g_mutex_unlock(mutex);
  if (cancelled_during_restart) {
    gst_element_set_state(bin->src_elem, GST_STATE_NULL);
  }
  return FALSE;
}

static gboolean send_uri_audio_eos(NvDsSrcBin* bin, gboolean log_failure) {
  if (!bin || !bin->uri_audio_tee || bin->uri_audio_link_count == 0) {
    return FALSE;
  }

  gboolean sent = FALSE;
  GstIterator* it = gst_element_iterate_src_pads(bin->uri_audio_tee);
  GValue item = G_VALUE_INIT;
  gboolean done = FALSE;
  while (!done) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        GstPad* srcpad = GST_PAD(g_value_get_object(&item));
        if (srcpad && gst_pad_is_linked(srcpad)) {
          sent = gst_pad_push_event(srcpad, gst_event_new_eos()) || sent;
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  if (G_VALUE_TYPE(&item) != 0) {
    g_value_unset(&item);
  }
  gst_iterator_free(it);

  if (!sent && log_failure) {
    GST_DEBUG_OBJECT(bin->uri_audio_tee, "No URI audio tee src pad accepted synthetic final EOS");
  }
  return sent;
}

static void cb_no_more_pads(GstElement* decodebin, gpointer data) {
  (void)decodebin;
  NvDsSrcBin* bin = (NvDsSrcBin*)data;
  if (!bin || !bin->uri_list || bin->num_uri_list < 1) {
    return;
  }
  GMutex* mutex = uri_playlist_mutex(bin);
  g_mutex_lock(mutex);
  const guint uri_index = bin->uri_list_index;
  bin->uri_list_pads_complete = TRUE;
  const gboolean terminal_drain_without_audio =
      uri_playlist_terminal_locked(bin) && bin->uri_terminal_audio_drain_pending && !bin->uri_audio_has_pad;
  g_mutex_unlock(mutex);
  // If video EOS raced ahead of no-more-pads, this is what proves that the URI truly has no audio pad.
  if (terminal_drain_without_audio) {
    g_idle_add(finish_uri_terminal_audio_drain, bin);
  } else {
    maybe_complete_uri_playlist_boundary(bin, uri_index);
  }
}

/**
 * Probe function to drop certain events to support custom
 * logic of looping of each source stream.
 */
static GstPadProbeReturn restart_stream_buf_prob(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  GstEvent* event = GST_EVENT(info->data);
  NvDsSrcBin* bin = (NvDsSrcBin*)u_data;

  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
    GST_BUFFER_PTS(GST_BUFFER(info->data)) += bin->prev_accumulated_base;
  }
  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH)) {
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
      g_timeout_add(1, seek_decode, bin);
    }

    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      GstSegment* segment = NULL;

      gst_event_parse_segment(event, (const GstSegment**)&segment);
      segment->base = bin->accumulated_base;
      bin->prev_accumulated_base = bin->accumulated_base;
      bin->accumulated_base += segment->stop;
    }
    switch (GST_EVENT_TYPE(event)) {
      case GST_EVENT_EOS:
        /* QOS events from downstream sink elements cause decoder to drop
         * frames after looping the file since the timestamps reset to 0.
         * We should drop the QOS events since we have custom logic for
         * looping individual sources. */
      case GST_EVENT_QOS:
      case GST_EVENT_SEGMENT:
      case GST_EVENT_FLUSH_START:
      case GST_EVENT_FLUSH_STOP:
        return GST_PAD_PROBE_DROP;
      default:
        break;
    }
  }
  return GST_PAD_PROBE_OK;
}

/**
 * Probe function to implement URI playlist switching for file sources.
 *
 * Installed on the decoder sink pad so we can:
 * - drop QOS events coming from downstream sinks (prevents frame drops after timestamp discontinuities)
 * - drop EOS/SEGMENT/FLUSH events while we reconfigure to the next URI
 * - advance the accumulated base used by per-URI decoded pad timestamp probes.
 */
static GstPadProbeReturn uri_list_stream_buf_prob(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  GstEvent* event = GST_EVENT(info->data);
  NvDsSrcBin* bin = (NvDsSrcBin*)u_data;
  if (!bin) {
    return GST_PAD_PROBE_OK;
  }

  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
    GstBuffer* buf = GST_BUFFER(info->data);
    GstClockTime pts = GST_BUFFER_PTS(buf);
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
      GMutex* mutex = uri_playlist_mutex(bin);
      g_mutex_lock(mutex);
      bin->uri_list_last_pts = pts;
      g_mutex_unlock(mutex);
    }
  }

  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH)) {
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
      // Let EOS reach the decoder so it drains every delayed frame. The decoded-video pad probe suppresses only
      // intermediate chapter EOS and schedules the switch; final EOS continues to nvstreammux normally.
      return GST_PAD_PROBE_OK;
    }

    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      const GstSegment* segment = NULL;
      gst_event_parse_segment(event, &segment);
      if (segment) {
        // Cache the segment stop/duration as a hint for how far to advance the PTS base on EOS.
        // (SEEK also emits SEGMENT events, so we must not advance bases here.)
        guint64 seg_stop = segment->stop;
        if (seg_stop == GST_CLOCK_TIME_NONE) {
          seg_stop = segment->duration;
        }
        if (seg_stop != GST_CLOCK_TIME_NONE) {
          GMutex* mutex = uri_playlist_mutex(bin);
          g_mutex_lock(mutex);
          bin->uri_list_segment_stop = seg_stop;
          g_mutex_unlock(mutex);
        }
      }
    }

    switch (GST_EVENT_TYPE(event)) {
      case GST_EVENT_QOS:
        return GST_PAD_PROBE_DROP;
      case GST_EVENT_FLUSH_START:
      case GST_EVENT_FLUSH_STOP:
        // Allow flush events for normal pipeline seeks. During a URI switch we will reset the source bin anyway.
        {
          GMutex* mutex = uri_playlist_mutex(bin);
          g_mutex_lock(mutex);
          const gboolean switch_pending = bin->uri_switch_pending;
          g_mutex_unlock(mutex);
          return switch_pending ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
        }
      default:
        break;
    }
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn uri_list_audio_event_probe(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  (void)pad;
  NvDsSrcBin* bin = (NvDsSrcBin*)u_data;
  if (!bin || (info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH) == 0) {
    return GST_PAD_PROBE_OK;
  }

  GstEvent* event = GST_EVENT(info->data);
  if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
    const bool is_playlist = (bin->uri_list && bin->num_uri_list >= 1);
    if (is_playlist) {
      // Decoded-pad probes coordinate all real URI EOS. Synthetic terminal EOS is pushed directly on tee src pads
      // and therefore bypasses this tee sink-pad safety net.
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}

static void decodebin_child_added(GstChildProxy* child_proxy, GObject* object, gchar* name, gpointer user_data) {
  NvDsSrcBin* bin = (NvDsSrcBin*)user_data;
  NvDsSourceConfig* config = bin->config;
  struct cudaDeviceProp prop = {0};
  cudaGetDeviceProperties(&prop, config->gpu_id);
  if (cudaGetDeviceProperties(&prop, config->gpu_id) != cudaSuccess) {
    NVGSTDS_ERR_MSG_V("Failed to get properties for GPU %d", config->gpu_id);
  }
  if (g_strrstr(name, "decodebin") == name) {
    g_signal_connect(G_OBJECT(object), "child-added", G_CALLBACK(decodebin_child_added), user_data);
  }
  if ((g_strrstr(name, "h264parse") == name) || (g_strrstr(name, "h265parse") == name)) {
    g_object_set(object, "config-interval", -1, NULL);
  }
  if (g_strrstr(name, "fakesink") == name) {
    g_object_set(object, "enable-last-sample", FALSE, NULL);
  }
  if (g_strrstr(name, "nvcuvid") == name) {
    g_object_set(object, "gpu-id", config->gpu_id, NULL);

    g_object_set(G_OBJECT(object), "cuda-memory-type", config->cuda_memory_type, NULL);

    g_object_set(object, "source-id", config->camera_id, NULL);
    g_object_set(object, "num-decode-surfaces", config->num_decode_surfaces, NULL);
    if (config->intra_decode_enable)
      g_object_set(object, "Intra-decode", config->intra_decode_enable, NULL);
  }
  if (g_strstr_len(name, -1, "omx") == name) {
    if (config->intra_decode_enable)
      g_object_set(object, "skip-frames", 2, NULL);
    g_object_set(object, "disable-dvfs", TRUE, NULL);
  }
  if (g_strstr_len(name, -1, "nvv4l2decoder") == name) {
    if (config->low_latency_mode)
      g_object_set(object, "low-latency-mode", TRUE, NULL);
    if (config->intra_decode_enable)
      g_object_set(object, "skip-frames", 2, NULL);
#ifdef __aarch64__
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(object), "enable-max-performance")) {
      g_object_set(object, "enable-max-performance", TRUE, NULL);
    }
#endif

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(object), "gpu-id")) {
      g_object_set(object, "gpu-id", config->gpu_id, NULL);
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(object), "cuda_memory_type")) {
      g_object_set(G_OBJECT(object), "cuda_memory_type", config->cuda_memory_type, NULL);
    }
    g_object_set(object, "drop-frame-interval", config->drop_frame_interval, NULL);
    /* extract-sei-type5-data is a valid parameter for nvv4l2decoder
       on x86 and ARM_SBSA */
    if (!prop.integrated) {
      g_object_set(object, "extract-sei-type5-data", config->extract_sei_type5_data, NULL);
    }
    g_object_set(object, "num-extra-surfaces", config->num_extra_surfaces, NULL);

    /* Seek only if file is the source. */
    if (config->loop && g_strstr_len(config->uri, -1, "file:/") == config->uri) {
      NVGSTDS_ELEM_ADD_PROBE(
          bin->src_buffer_probe,
          GST_ELEMENT(object),
          "sink",
          restart_stream_buf_prob,
          (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_BOTH | GST_PAD_PROBE_TYPE_EVENT_FLUSH | GST_PAD_PROBE_TYPE_BUFFER),
          bin);
    }
  }

  // Playlist switching: install on any video decoder (not just nvv4l2decoder).
  if (!config->loop && bin->uri_list && bin->num_uri_list >= 1 && config->uri &&
      g_strstr_len(config->uri, -1, "file:/") == config->uri && !bin->src_buffer_probe) {
    GstElementFactory* factory = GST_ELEMENT_GET_CLASS(GST_ELEMENT(object))->elementfactory;
    const gchar* klass = factory ? gst_element_factory_get_klass(factory) : nullptr;
    const bool is_video_decoder = (klass && g_strrstr(klass, "Decoder") && g_strrstr(klass, "Video"));
    if (is_video_decoder) {
      NVGSTDS_ELEM_ADD_PROBE(
          bin->src_buffer_probe,
          GST_ELEMENT(object),
          "sink",
          uri_list_stream_buf_prob,
          (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_BOTH | GST_PAD_PROBE_TYPE_EVENT_FLUSH | GST_PAD_PROBE_TYPE_BUFFER),
          bin);
    }
  }
done:
  return;
}

gboolean link_uri_source_audio_src(NvDsSrcBin* bin, GstElement* sinkelem) {
  if (!bin || !bin->uri_audio_tee || !bin->bin || !sinkelem) {
    return FALSE;
  }

  GstPadTemplate* padtemplate =
      (GstPadTemplate*)gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(bin->uri_audio_tee), "src_%u");
  GstPad* tee_src_pad = gst_element_request_pad(bin->uri_audio_tee, padtemplate, NULL, NULL);
  if (!tee_src_pad) {
    NVGSTDS_ERR_MSG_V("Failed to get src pad from URI source audio tee");
    return FALSE;
  }

  const guint link_id = bin->uri_audio_link_count++;
  gchar ghost_name[64];
  g_snprintf(ghost_name, sizeof(ghost_name), "uri_audio_src_%u", link_id);
  GstPad* ghost_pad = gst_ghost_pad_new(ghost_name, tee_src_pad);
  gst_object_unref(tee_src_pad);
  if (!ghost_pad) {
    NVGSTDS_ERR_MSG_V("Failed to create URI source audio ghost pad");
    return FALSE;
  }
  if (!gst_element_add_pad(bin->bin, ghost_pad)) {
    NVGSTDS_ERR_MSG_V("Failed to add URI source audio ghost pad");
    gst_object_unref(ghost_pad);
    return FALSE;
  }

  gchar lift_name[96];
  g_snprintf(lift_name, sizeof(lift_name), "hmaudio_uri_%s_%u", GST_ELEMENT_NAME(bin->bin), link_id);
  return hm::connectElementsWithGhostPads(bin->bin, ghost_name, sinkelem, "sink", lift_name);
}

static void cb_newpad2(GstElement* decodebin, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_query_caps(pad, NULL);
  const GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (!strncmp(name, "video", 5)) {
    NvDsSrcBin* bin = (NvDsSrcBin*)data;
    GstPad* sinkpad = gst_element_get_static_pad(bin->cap_filter, "sink");
    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
      NVGSTDS_ERR_MSG_V("Failed to link decodebin to pipeline");
    } else {
      NvDsSourceConfig* config = (NvDsSourceConfig*)g_object_get_data(G_OBJECT(bin->cap_filter), SRC_CONFIG_KEY);

      gst_structure_get_int(str, "width", &config->camera_width);
      gst_structure_get_int(str, "height", &config->camera_height);
      gst_structure_get_fraction(str, "framerate", &config->camera_fps_n, &config->camera_fps_d);

      GST_CAT_DEBUG(NVDS_APP, "Decodebin linked to pipeline");
    }
    gst_object_unref(sinkpad);
  }
  gst_caps_unref(caps);
}

static void cb_newpad3(GstElement* decodebin, GstPad* pad, gpointer data) {
  GstCaps* caps = gst_pad_query_caps(pad, NULL);
  const GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(str);

  if (g_strrstr(name, "x-rtp")) {
    NvDsSrcBin* bin = (NvDsSrcBin*)data;
    GstPad* sinkpad = gst_element_get_static_pad(bin->depay, "sink");
    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
      NVGSTDS_ERR_MSG_V("Failed to link depay loader to rtsp src");
    }
    gst_object_unref(sinkpad);
  }
  gst_caps_unref(caps);
}

/* Returning FALSE from this callback will make rtspsrc ignore the stream.
 * Ignore audio and add the proper depay element based on codec. */
static gboolean cb_rtspsrc_select_stream(GstElement* rtspsrc, guint num, GstCaps* caps, gpointer user_data) {
  GstStructure* str = gst_caps_get_structure(caps, 0);
  const gchar* media = gst_structure_get_string(str, "media");
  const gchar* encoding_name = gst_structure_get_string(str, "encoding-name");
  gchar elem_name[50];
  NvDsSrcBin* bin = (NvDsSrcBin*)user_data;
  gboolean ret = FALSE;

  gboolean is_video = (!g_strcmp0(media, "video"));

  if (!is_video)
    return FALSE;

  /* Create and add depay element only if it is not created yet. */
  if (!bin->depay) {
    g_snprintf(elem_name, sizeof(elem_name), "depay_elem%d", bin->bin_id);

    /* Add the proper depay element based on codec. */
    if (!g_strcmp0(encoding_name, "H264")) {
      bin->depay = gst_element_factory_make("rtph264depay", elem_name);
      g_snprintf(elem_name, sizeof(elem_name), "h264parse_elem%d", bin->bin_id);
      bin->parser = gst_element_factory_make("h264parse", elem_name);
    } else if (!g_strcmp0(encoding_name, "H265")) {
      bin->depay = gst_element_factory_make("rtph265depay", elem_name);
      g_snprintf(elem_name, sizeof(elem_name), "h265parse_elem%d", bin->bin_id);
      bin->parser = gst_element_factory_make("h265parse", elem_name);
    } else {
      NVGSTDS_WARN_MSG_V("%s not supported", encoding_name);
      return FALSE;
    }

    if (!bin->depay) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      return FALSE;
    }

    gst_bin_add_many(GST_BIN(bin->bin), bin->depay, bin->parser, NULL);

    NVGSTDS_LINK_ELEMENT(bin->depay, bin->parser);
    NVGSTDS_LINK_ELEMENT(bin->parser, bin->tee_rtsp_pre_decode);

    if (!gst_element_sync_state_with_parent(bin->depay)) {
      NVGSTDS_ERR_MSG_V("'%s' failed to sync state with parent", elem_name);
      return FALSE;
    }
    gst_element_sync_state_with_parent(bin->parser);
  }

  ret = TRUE;
done:
  return ret;
}

void destroy_smart_record_bin(gpointer data) {
  unsigned int i = 0;
  NvDsSrcBin* src_bin;
  NvDsSrcParentBin* pbin = (NvDsSrcParentBin*)data;

  g_return_if_fail(pbin);

  for (i = 0; i < pbin->num_bins; i++) {
    src_bin = &pbin->sub_bins[i];
    if (src_bin && src_bin->recordCtx)
      NvDsSRDestroy((NvDsSRContext*)src_bin->recordCtx);
  }
  pbin->num_bins = 0;
}

static gpointer smart_record_callback(NvDsSRRecordingInfo* info, gpointer userData) {
  static GMutex mutex;
  FILE* logfile = NULL;
  g_return_val_if_fail(info, NULL);

  g_mutex_lock(&mutex);
  logfile = fopen("smart_record.log", "a");
  if (logfile) {
    fprintf(
        logfile,
        "%d:%d:%d:%ldms:%s:%s\n",
        info->sessionId,
        info->width,
        info->height,
        info->duration,
        info->dirpath,
        info->filename);
    fclose(logfile);
  } else {
    g_print("Error in opeing smart record log file\n");
  }
  g_mutex_unlock(&mutex);

  return NULL;
}

/**
 * Function called at regular interval to start and stop video recording.
 * This is dummy implementation to show the usages of smart record APIs.
 * startTime and Duration can be adjusted as per usecase.
 */
static gboolean smart_record_event_generator(gpointer data) {
  NvDsSRSessionId sessId = 0;
  NvDsSrcBin* src_bin = (NvDsSrcBin*)data;
  guint startTime = 7;
  guint duration = 8;

  if (src_bin->config->smart_rec_duration >= 0)
    duration = src_bin->config->smart_rec_duration;

  if (src_bin->config->smart_rec_start_time >= 0)
    startTime = src_bin->config->smart_rec_start_time;

  if (src_bin->recordCtx && !src_bin->reconfiguring) {
    NvDsSRContext* ctx = (NvDsSRContext*)src_bin->recordCtx;
    if (ctx->recordOn) {
      NvDsSRStop(ctx, 0);
    } else {
      NvDsSRStart(ctx, &sessId, startTime, duration, NULL);
    }
  }
  return TRUE;
}

static void check_rtsp_reconnection_attempts(NvDsSrcBin* src_bin) {
  gboolean remove_probe = TRUE;
  guint i = 0;
  for (i = 0; i < src_bin->parent_bin->num_bins; i++) {
    if (src_bin->parent_bin->sub_bins[i].config->type != NV_DS_SOURCE_RTSP)
      continue;
    if (src_bin->parent_bin->sub_bins[i].have_eos &&
        (src_bin->parent_bin->sub_bins[i].rtsp_reconnect_interval_sec == 0 ||
         src_bin->parent_bin->sub_bins[i].rtsp_reconnect_attempts == 0)) {
      remove_probe = FALSE;
      break;
    }
    if (src_bin->parent_bin->sub_bins[i].num_rtsp_reconnects <=
        src_bin->parent_bin->sub_bins[i].rtsp_reconnect_attempts) {
      if (src_bin->parent_bin->sub_bins[i].rtsp_reconnect_interval_sec || !src_bin->parent_bin->sub_bins[i].have_eos) {
        remove_probe = FALSE;
        break;
      }
    }
  }

  if (remove_probe) {
    GstElement* pipeline = GST_ELEMENT_PARENT(GST_ELEMENT_PARENT(src_bin->bin));
    NVGSTDS_ELEM_REMOVE_PROBE(src_bin->parent_bin->nvstreammux_eosmonitor_probe, src_bin->parent_bin->streammux, "src");
    GST_ELEMENT_ERROR(
        pipeline,
        STREAM,
        FAILED,
        ("Reconnection attempts exceeded for all sources or EOS received."
         " Stopping pipeline"),
        (NULL));
  }
}

/**
 * Function called at regular interval to check if NV_DS_SOURCE_RTSP type
 * source in the pipeline is down / disconnected. This function try to
 * reconnect the source by resetting that source pipeline.
 */
static gboolean watch_source_status(gpointer data) {
  NvDsSrcBin* src_bin = (NvDsSrcBin*)data;
  struct timeval current_time;
  gettimeofday(&current_time, NULL);
  static struct timeval last_reset_time_global = {0, 0};
  gdouble time_diff_msec_since_last_reset = 1000.0 * (current_time.tv_sec - last_reset_time_global.tv_sec) +
      (current_time.tv_usec - last_reset_time_global.tv_usec) / 1000.0;

  if (src_bin->reconfiguring) {
    guint time_since_last_reconnect_sec = current_time.tv_sec - src_bin->last_reconnect_time.tv_sec;
    if (time_since_last_reconnect_sec >= SOURCE_RESET_INTERVAL_SEC) {
      if (time_diff_msec_since_last_reset > 3000) {
        if (src_bin->rtsp_reconnect_attempts == -1 ||
            ++src_bin->num_rtsp_reconnects <= src_bin->rtsp_reconnect_attempts) {
          last_reset_time_global = current_time;
          // source is still not up, reconfigure it again.
          reset_source_pipeline(src_bin);
        } else {
          GST_ELEMENT_WARNING(
              src_bin->bin,
              STREAM,
              FAILED,
              ("Number of RTSP reconnect attempts exceeded, stopping source: %d", src_bin->source_id),
              (NULL));

          check_rtsp_reconnection_attempts(src_bin);

          GstElement* send_event_element = NULL;
          if (src_bin->dewarper_bin.bin != NULL) {
            send_event_element = src_bin->dewarper_bin.bin;
          } else {
            send_event_element = src_bin->cap_filter1;
          }
          gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_start());
          gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_stop(TRUE));
          if (!gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_eos())) {
            GST_ERROR_OBJECT(send_event_element, "Interrupted, Reconnection event not sent");
          }
          if (gst_element_set_state(src_bin->bin, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
            GST_ERROR_OBJECT(src_bin->bin, "Can't set source bin to NULL");
          }

          return FALSE;
        }
      }
    }
  } else {
    gint time_since_last_buf_sec = 0;
    g_mutex_lock(&src_bin->bin_lock);
    if (src_bin->last_buffer_time.tv_sec != 0) {
      time_since_last_buf_sec = current_time.tv_sec - src_bin->last_buffer_time.tv_sec;
    }
    g_mutex_unlock(&src_bin->bin_lock);

    // Reset source bin if no buffers are received in the last
    // `rtsp_reconnect_interval_sec` seconds.
    if (src_bin->rtsp_reconnect_interval_sec > 0 && time_since_last_buf_sec >= src_bin->rtsp_reconnect_interval_sec) {
      if (time_diff_msec_since_last_reset > 3000) {
        if (src_bin->rtsp_reconnect_attempts == -1 ||
            ++src_bin->num_rtsp_reconnects <= src_bin->rtsp_reconnect_attempts) {
          last_reset_time_global = current_time;

          NVGSTDS_WARN_MSG_V(
              "No data from source %d since last %u sec. Trying reconnection",
              src_bin->bin_id,
              time_since_last_buf_sec);
          reset_source_pipeline(src_bin);
        } else {
          GST_ELEMENT_WARNING(
              src_bin->bin,
              STREAM,
              FAILED,
              ("Number of RTSP reconnect attempts exceeded, stopping source: %d", src_bin->source_id),
              (NULL));

          check_rtsp_reconnection_attempts(src_bin);

          GstElement* send_event_element = NULL;
          if (src_bin->dewarper_bin.bin != NULL) {
            send_event_element = src_bin->dewarper_bin.bin;
          } else {
            send_event_element = src_bin->cap_filter1;
          }
          gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_start());
          gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_stop(TRUE));
          if (!gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_eos())) {
            GST_ERROR_OBJECT(send_event_element, "Interrupted, Reconnection event not sent");
          }
          if (gst_element_set_state(src_bin->bin, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
            GST_ERROR_OBJECT(src_bin->bin, "Can't set source bin to NULL");
          }

          return FALSE;
        }
      }
    }
  }
  return TRUE;
}

/**
 * Function called at regular interval when source bin is
 * changing state async. This function watches the state of
 * the source bin and sets it to PLAYING if the state of source
 * bin stops at PAUSED when changing state ASYNC.
 */
static gboolean watch_source_async_state_change(gpointer data) {
  NvDsSrcBin* src_bin = (NvDsSrcBin*)data;
  GstState state = GST_STATE_NULL, pending = GST_STATE_NULL;
  GstStateChangeReturn ret;

  ret = gst_element_get_state(src_bin->bin, &state, &pending, 0);

  GST_CAT_DEBUG(
      NVDS_APP,
      "Bin %d %p: state:%s pending:%s ret:%s",
      src_bin->bin_id,
      src_bin,
      gst_element_state_get_name(state),
      gst_element_state_get_name(pending),
      gst_element_state_change_return_get_name(ret));

  // Bin is still changing state ASYNC. Wait for some more time.
  if (ret == GST_STATE_CHANGE_ASYNC)
    return TRUE;

  // Bin state change failed / failed to get state
  if (ret == GST_STATE_CHANGE_FAILURE) {
    src_bin->async_state_watch_running = FALSE;
    return FALSE;
  }
  // Bin successfully changed state to PLAYING. Stop watching state
  if (state == GST_STATE_PLAYING) {
    src_bin->reconfiguring = FALSE;
    src_bin->async_state_watch_running = FALSE;
    src_bin->num_rtsp_reconnects = 0;
    return FALSE;
  }
  // Bin has stopped ASYNC state change but has not gone into
  // PLAYING. Expliclity set state to PLAYING and keep watching
  // state
  gst_element_set_state(src_bin->bin, GST_STATE_PLAYING);

  return TRUE;
}

/**
 * Probe function to monitor data output from rtspsrc.
 */
static GstPadProbeReturn rtspsrc_monitor_probe_func(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  NvDsSrcBin* bin = (NvDsSrcBin*)u_data;
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    g_mutex_lock(&bin->bin_lock);
    gettimeofday(&bin->last_buffer_time, NULL);
    bin->have_eos = FALSE;
    g_mutex_unlock(&bin->bin_lock);
  }
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    if (GST_EVENT_TYPE(info->data) == GST_EVENT_EOS) {
      bin->have_eos = TRUE;
      check_rtsp_reconnection_attempts(bin);
    }
  }
  return GST_PAD_PROBE_OK;
}

/**
 * Probe function to drop EOS events from nvstreammux when RTSP sources
 * are being used so that app does not quit from EOS in case of RTSP
 * connection errors and tries to reconnect.
 */
static GstPadProbeReturn nvstreammux_eosmonitor_probe_func(GstPad* pad, GstPadProbeInfo* info, gpointer u_data) {
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent* event = (GstEvent*)info->data;
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS)
      return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}

static gboolean create_rtsp_src_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  NvDsSRContext* ctx = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  bin->config = config;
  GstCaps* caps = NULL;
  GstCapsFeatures* feature = NULL;

  bin->latency = config->latency;
  bin->udp_buffer_size = config->udp_buffer_size;
  bin->rtsp_reconnect_interval_sec = config->rtsp_reconnect_interval_sec;
  bin->rtsp_reconnect_attempts = config->rtsp_reconnect_attempts;
  bin->num_rtsp_reconnects = 0;

  g_snprintf(elem_name, sizeof(elem_name), "src_elem%d", bin->bin_id);
  bin->src_elem = gst_element_factory_make("rtspsrc", elem_name);
  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_signal_connect(G_OBJECT(bin->src_elem), "select-stream", G_CALLBACK(cb_rtspsrc_select_stream), bin);

  if (config->udp_buffer_size) {
    g_object_set(G_OBJECT(bin->src_elem), "udp-buffer-size", config->udp_buffer_size, NULL);
  }

  g_object_set(G_OBJECT(bin->src_elem), "location", config->uri, NULL);
  g_object_set(G_OBJECT(bin->src_elem), "latency", config->latency, NULL);
  g_object_set(G_OBJECT(bin->src_elem), "drop-on-latency", TRUE, NULL);
  configure_source_for_ntp_sync(bin->src_elem);

  // 0x4 for TCP and 0x7 for All (UDP/UDP-MCAST/TCP)
  if ((config->select_rtp_protocol == GST_RTSP_LOWER_TRANS_TCP) ||
      (config->select_rtp_protocol ==
       (GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP))) {
    g_object_set(G_OBJECT(bin->src_elem), "protocols", config->select_rtp_protocol, NULL);
    GST_DEBUG_OBJECT(
        bin->src_elem, "RTP Protocol=0x%x (0x4=TCP and 0x7=UDP,TCP,UDPMCAST)----\n", config->select_rtp_protocol);
  }
  g_signal_connect(G_OBJECT(bin->src_elem), "pad-added", G_CALLBACK(cb_newpad3), bin);

  g_snprintf(elem_name, sizeof(elem_name), "tee_rtsp_elem%d", bin->bin_id);
  bin->tee_rtsp_pre_decode = gst_element_factory_make("tee", elem_name);
  if (!bin->tee_rtsp_pre_decode) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf(elem_name, sizeof(elem_name), "tee_rtsp_post_decode_elem%d", bin->bin_id);
  bin->tee_rtsp_post_decode = gst_element_factory_make("tee", elem_name);
  if (!bin->tee_rtsp_post_decode) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  if (config->smart_record) {
    NvDsSRInitParams params = {0};
    params.containerType = (NvDsSRContainerType)config->smart_rec_container;
    if (config->start_rec_file_prefix)
      params.fileNamePrefix = g_strdup_printf("%s_%d", config->start_rec_file_prefix, config->camera_id);
    params.dirpath = config->start_rec_dir_path;
    params.cacheSize = config->smart_rec_cache_size;
    params.defaultDuration = config->smart_rec_def_duration;
    params.callback = smart_record_callback;
    if (NvDsSRCreate(&ctx, &params) != NVDSSR_STATUS_OK) {
      NVGSTDS_ERR_MSG_V("Failed to create smart record bin");
      g_free(params.fileNamePrefix);
      goto done;
    }
    g_free(params.fileNamePrefix);
    gst_bin_add(GST_BIN(bin->bin), ctx->recordbin);
    bin->recordCtx = (gpointer)ctx;
  }

  g_snprintf(elem_name, sizeof(elem_name), "dec_que%d", bin->bin_id);
  bin->dec_que = gst_element_factory_make("queue", elem_name);
  if (!bin->dec_que) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  if (bin->rtsp_reconnect_interval_sec > 0) {
    NVGSTDS_ELEM_ADD_PROBE(
        bin->rtspsrc_monitor_probe, bin->dec_que, "sink", rtspsrc_monitor_probe_func, GST_PAD_PROBE_TYPE_BUFFER, bin);
    install_mux_eosmonitor_probe = TRUE;
  } else {
    NVGSTDS_ELEM_ADD_PROBE(
        bin->rtspsrc_monitor_probe,
        bin->dec_que,
        "sink",
        rtspsrc_monitor_probe_func,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        bin);
  }

  g_snprintf(elem_name, sizeof(elem_name), "decodebin_elem%d", bin->bin_id);
  bin->decodebin = gst_element_factory_make("decodebin", elem_name);
  if (!bin->decodebin) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_signal_connect(G_OBJECT(bin->decodebin), "pad-added", G_CALLBACK(cb_newpad2), bin);
  g_signal_connect(G_OBJECT(bin->decodebin), "child-added", G_CALLBACK(decodebin_child_added), bin);

  g_snprintf(elem_name, sizeof(elem_name), "src_que%d", bin->bin_id);
  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_QUEUE, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
    goto done;
  }

  g_mutex_init(&bin->bin_lock);
  if (config->dewarper_config.enable) {
    if (!create_dewarper_bin(&config->dewarper_config, &bin->dewarper_bin)) {
      g_print("Failed to create dewarper bin \n");
      goto done;
    }
    gst_bin_add_many(
        GST_BIN(bin->bin),
        bin->src_elem,
        bin->tee_rtsp_pre_decode,
        bin->dec_que,
        bin->decodebin,
        bin->cap_filter,
        bin->tee_rtsp_post_decode,
        bin->dewarper_bin.bin,
        NULL);
  } else {
    g_snprintf(elem_name, sizeof(elem_name), "nvvidconv_elem%d", bin->bin_id);
    bin->nvvidconv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
    if (!bin->nvvidconv) {
      NVGSTDS_ERR_MSG_V("Could not create element 'nvvidconv_elem'");
      goto done;
    }
    g_object_set(
        G_OBJECT(bin->nvvidconv), "gpu-id", config->gpu_id, "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    /* For Jetson, with copy-hw=1 and memory-type=nvbuf-mem-surface-array,
       cudaMemcopy fail is observed. This is a WAR till root cause is fixed */
    g_object_set(G_OBJECT(bin->nvvidconv), "copy-hw", 2, NULL);
#endif

    if (config->video_format) {
      caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, config->video_format, NULL);
    } else {
      caps = gst_caps_new_empty_simple("video/x-raw");
    }
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(caps, 0, feature);

    bin->cap_filter1 = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter_nvvidconv");
    if (!bin->cap_filter1) {
      NVGSTDS_ERR_MSG_V("Could not create 'queue'");
      goto done;
    }

    g_object_set(G_OBJECT(bin->cap_filter1), "caps", caps, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(
        GST_BIN(bin->bin),
        bin->src_elem,
        bin->tee_rtsp_pre_decode,
        bin->dec_que,
        bin->decodebin,
        bin->cap_filter,
        bin->tee_rtsp_post_decode,
        bin->nvvidconv,
        bin->cap_filter1,
        NULL);
  }

  link_element_to_tee_src_pad(bin->tee_rtsp_pre_decode, bin->dec_que);
  NVGSTDS_LINK_ELEMENT(bin->dec_que, bin->decodebin);

  if (ctx)
    link_element_to_tee_src_pad(bin->tee_rtsp_pre_decode, ctx->recordbin);

  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->tee_rtsp_post_decode);
  if (config->dewarper_config.enable) {
    link_element_to_tee_src_pad(bin->tee_rtsp_post_decode, bin->dewarper_bin.bin);
    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->dewarper_bin.bin, "src");
  } else {
    link_element_to_tee_src_pad(bin->tee_rtsp_post_decode, bin->nvvidconv);
    NVGSTDS_LINK_ELEMENT(bin->nvvidconv, bin->cap_filter1);
    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter1, "src");
  }

  ret = TRUE;

  g_timeout_add(1000, watch_source_status, bin);

  // Enable local start / stop events in addition to the one
  // received from the server.
  if (config->smart_record == 2) {
    if (bin->config->smart_rec_interval)
      g_timeout_add(bin->config->smart_rec_interval * 1000, smart_record_event_generator, bin);
    else
      g_timeout_add(10000, smart_record_event_generator, bin);
  }

  GST_CAT_DEBUG(NVDS_APP, "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static gboolean create_audiodecode_src_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  gboolean ret = FALSE;
  guint const MAX_CAPS_LEN = 256;
  gchar caps_audio_resampler[MAX_CAPS_LEN];
  GstCaps* caps = NULL;
  bin->config = config;

  config->live_source = FALSE;

  if (config->type == NV_DS_SOURCE_AUDIO_WAV) {
    bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_MULTIFILE, "src_elem");
    if (!bin->src_elem) {
      NVGSTDS_ERR_MSG_V("Could not create element 'src_elem'");
      goto done;
    }

    g_object_set(G_OBJECT(bin->src_elem), "location", config->uri, NULL);
    g_object_set(G_OBJECT(bin->src_elem), "loop", config->loop, NULL);

    bin->decodebin = gst_element_factory_make(NVDS_ELEM_WAVPARSE, "decodebin_elem");
    if (!bin->decodebin) {
      NVGSTDS_ERR_MSG_V("Could not create element 'decodebin_elem'");
      goto done;
    }
    g_object_set(G_OBJECT(bin->decodebin), "ignore-length", config->loop, NULL);
  } else if (config->type == NV_DS_SOURCE_ALSA_SRC) {
    bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_ALSA, "src_elem");
    if (!bin->src_elem) {
      NVGSTDS_ERR_MSG_V("Could not create element 'src_elem'");
      goto done;
    }
    if (config->alsa_device) {
      g_object_set(G_OBJECT(bin->src_elem), "device", config->alsa_device, NULL);
    }
  } else {
    NVGSTDS_ERR_MSG_V("Source Type (%d) not supported\n", config->type);
    goto done;
  }

  bin->audio_converter = gst_element_factory_make("audioconvert", "audio-convert");
  if (!bin->audio_converter) {
    NVGSTDS_ERR_MSG_V("Could not create 'audioconvert'");
    goto done;
  }

  bin->audio_resample = gst_element_factory_make("audioresample", "audio-resample");
  if (!bin->audio_resample) {
    NVGSTDS_ERR_MSG_V("Could not create 'audioresample'");
    goto done;
  }

  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter_audioresample");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Could not create src_cap_filter_audioresample");
    goto done;
  }

  if (snprintf(caps_audio_resampler, MAX_CAPS_LEN, "audio/x-raw, rate=%d", config->input_audio_rate) <= 0) {
    NVGSTDS_ERR_MSG_V("Could not create caps to force rate=%d", config->input_audio_rate);
    goto done;
  }
  caps = gst_caps_from_string(caps_audio_resampler);
  g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);
  gst_caps_unref(caps);

  if (config->type == NV_DS_SOURCE_AUDIO_WAV) {
    gst_bin_add_many(
        GST_BIN(bin->bin),
        bin->src_elem,
        bin->decodebin,
        bin->audio_converter,
        bin->audio_resample,
        bin->cap_filter,
        NULL);

    gst_element_link_many(
        bin->src_elem, bin->decodebin, bin->audio_converter, bin->audio_resample, bin->cap_filter, NULL);
  } else if (config->type == NV_DS_SOURCE_ALSA_SRC) {
    gst_bin_add_many(
        GST_BIN(bin->bin), bin->src_elem, bin->audio_converter, bin->audio_resample, bin->cap_filter, NULL);

    gst_element_link_many(bin->src_elem, bin->audio_converter, bin->audio_resample, bin->cap_filter, NULL);
  }

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter, "src");

  ret = TRUE;

  GST_CAT_DEBUG(NVDS_APP, "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static gboolean create_uridecode_src_bin_audio(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  gboolean ret = FALSE;
  guint const MAX_CAPS_LEN = 256;
  gchar caps_audio_resampler[MAX_CAPS_LEN];
  GstCaps* caps = NULL;
  bin->config = config;

  bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_URI, "src_elem");
  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V("Could not create element 'src_elem'");
    goto done;
  }
  bin->latency = config->latency;
  bin->udp_buffer_size = config->udp_buffer_size;

  if (g_strrstr(config->uri, "file:/")) {
    config->live_source = FALSE;
  }
  if (g_strrstr(config->uri, "rtsp://") == config->uri) {
    configure_source_for_ntp_sync(bin->src_elem);
  }

  g_object_set(G_OBJECT(bin->src_elem), "uri", config->uri, NULL);
  g_signal_connect(G_OBJECT(bin->src_elem), "pad-added", G_CALLBACK(cb_newpad_audio), bin);

  bin->audio_converter = gst_element_factory_make(NVDS_ELEM_AUDIO_CONV, "audioconv_elem");
  if (!bin->audio_converter) {
    NVGSTDS_ERR_MSG_V("Could not create element audio_converter");
    goto done;
  }

  bin->audio_resample = gst_element_factory_make(NVDS_ELEM_AUDIO_RESAMPLER, "audioresampler_elem");
  if (!bin->audio_resample) {
    NVGSTDS_ERR_MSG_V("Could not create element audio_resample");
    goto done;
  }

  if (config->input_audio_rate) {
    bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter_audioresample");
    if (!bin->cap_filter) {
      NVGSTDS_ERR_MSG_V("Could not create src_cap_filter_audioresample");
      goto done;
    }

    if (snprintf(caps_audio_resampler, MAX_CAPS_LEN, "audio/x-raw, rate=%d", config->input_audio_rate) <= 0) {
      NVGSTDS_ERR_MSG_V("Could not create caps to force rate=%d", config->input_audio_rate);
      goto done;
    }
    caps = gst_caps_from_string(caps_audio_resampler);
    g_object_set(G_OBJECT(bin->cap_filter), "caps", caps, NULL);
    gst_caps_unref(caps);
    gst_bin_add(GST_BIN(bin->bin), bin->cap_filter);
  }
  gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem, bin->audio_converter, bin->audio_resample, NULL);

  NVGSTDS_LINK_ELEMENT(bin->audio_converter, bin->audio_resample);

  if (bin->cap_filter) {
    NVGSTDS_LINK_ELEMENT(bin->audio_resample, bin->cap_filter);
    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter, "src");
  } else {
    NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->audio_resample, "src");
  }

  ret = TRUE;

  GST_CAT_DEBUG(NVDS_APP, "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static gboolean create_uridecode_src_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  gboolean ret = FALSE;
  GstCaps* caps = NULL;
  GstCapsFeatures* feature = NULL;
  bin->config = config;

  bin->src_elem = gst_element_factory_make(NVDS_ELEM_SRC_URI, "src_elem");
  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V("Could not create element 'src_elem'");
    goto done;
  }

  if (config->dewarper_config.enable) {
    if (!create_dewarper_bin(&config->dewarper_config, &bin->dewarper_bin)) {
      g_print("Creating Dewarper bin failed \n");
      goto done;
    }
  }
  bin->latency = config->latency;
  bin->udp_buffer_size = config->udp_buffer_size;

  // Initialize optional playlist support before setting properties on uridecodebin.
  init_uri_playlist(bin, config);

  if (g_strrstr(config->uri, "file:/")) {
    config->live_source = FALSE;
  }
  if (g_strrstr(config->uri, "rtsp://") == config->uri) {
    configure_source_for_ntp_sync(bin->src_elem);
  }

  g_object_set(G_OBJECT(bin->src_elem), "uri", config->uri, NULL);
  g_signal_connect(G_OBJECT(bin->src_elem), "pad-added", G_CALLBACK(cb_newpad), bin);
  g_signal_connect(G_OBJECT(bin->src_elem), "no-more-pads", G_CALLBACK(cb_no_more_pads), bin);
  g_signal_connect(G_OBJECT(bin->src_elem), "child-added", G_CALLBACK(decodebin_child_added), bin);
  g_signal_connect(G_OBJECT(bin->src_elem), "source-setup", G_CALLBACK(cb_sourcesetup), bin);
  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_QUEUE, "queue");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Could not create 'queue'");
    goto done;
  }

  bin->nvvidconv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "nvvidconv_elem");
  if (!bin->nvvidconv) {
    NVGSTDS_ERR_MSG_V("Could not create element 'nvvidconv_elem'");
    goto done;
  }

  g_object_set(
      G_OBJECT(bin->nvvidconv), "gpu-id", config->gpu_id, "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  /* For Jetson, with copy-hw=1 and memory-type=nvbuf-mem-surface-array,
     cudaMemcopy fail is observed. This is a WAR till root cause is fixed */
  g_object_set(G_OBJECT(bin->nvvidconv), "copy-hw", 2, NULL);
#endif

  if (config->video_format) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, config->video_format, NULL);
  } else {
    caps = gst_caps_new_empty_simple("video/x-raw");
  }
  feature = gst_caps_features_new("memory:NVMM", NULL);
  gst_caps_set_features(caps, 0, feature);

  bin->cap_filter1 = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter_nvvidconv");
  if (!bin->cap_filter1) {
    NVGSTDS_ERR_MSG_V("Could not create 'queue'");
    goto done;
  }

  g_object_set(G_OBJECT(bin->cap_filter1), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set_data(G_OBJECT(bin->cap_filter), SRC_CONFIG_KEY, config);

  gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem, bin->cap_filter, bin->nvvidconv, bin->cap_filter1, NULL);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->cap_filter1, "src");

  bin->fakesink = gst_element_factory_make("fakesink", "src_fakesink");
  if (!bin->fakesink) {
    NVGSTDS_ERR_MSG_V("Could not create 'src_fakesink'");
    goto done;
  }

  bin->fakesink_queue = gst_element_factory_make("queue", "fakequeue");
  if (!bin->fakesink_queue) {
    NVGSTDS_ERR_MSG_V("Could not create 'fakequeue'");
    goto done;
  }

  bin->tee = gst_element_factory_make("tee", NULL);
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V("Could not create 'tee'");
    goto done;
  }
  bin->uri_audio_tee = gst_element_factory_make("tee", "uri_audio_tee");
  if (!bin->uri_audio_tee) {
    NVGSTDS_ERR_MSG_V("Could not create 'uri_audio_tee'");
    goto done;
  }
  g_object_set(G_OBJECT(bin->uri_audio_tee), "allow-not-linked", TRUE, NULL);
  NVGSTDS_ELEM_ADD_PROBE(
      bin->uri_audio_probe,
      bin->uri_audio_tee,
      "sink",
      uri_list_audio_event_probe,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      bin);
  gst_bin_add_many(GST_BIN(bin->bin), bin->fakesink, bin->tee, bin->fakesink_queue, bin->uri_audio_tee, NULL);

  NVGSTDS_LINK_ELEMENT(bin->fakesink_queue, bin->fakesink);

  if (config->dewarper_config.enable) {
    gst_bin_add_many(GST_BIN(bin->bin), bin->dewarper_bin.bin, NULL);
    NVGSTDS_LINK_ELEMENT(bin->tee, bin->dewarper_bin.bin);
    NVGSTDS_LINK_ELEMENT(bin->dewarper_bin.bin, bin->cap_filter);
  } else {
    link_element_to_tee_src_pad(bin->tee, bin->cap_filter);
  }
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->nvvidconv);
  NVGSTDS_LINK_ELEMENT(bin->nvvidconv, bin->cap_filter1);
  link_element_to_tee_src_pad(bin->tee, bin->fakesink_queue);

  g_object_set(G_OBJECT(bin->fakesink), "sync", FALSE, "async", FALSE, NULL);
  g_object_set(G_OBJECT(bin->fakesink), "enable-last-sample", FALSE, NULL);

  ret = TRUE;

  GST_CAT_DEBUG(NVDS_APP, "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

gboolean create_audio_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  static guint bin_cnt = 0;
  gchar bin_name[64];

  g_snprintf(bin_name, 64, "src_bin_%d", bin_cnt++);
  bin->bin = gst_bin_new(bin_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'src_bin'");
    return FALSE;
  }

  if (!create_audiodecode_src_bin(config, bin)) {
    return FALSE;
  }
  bin->live_source = config->live_source;

  GST_CAT_DEBUG(NVDS_APP, "Source bin created");

  return TRUE;
}

gboolean create_source_bin(NvDsSourceConfig* config, NvDsSrcBin* bin) {
  static guint bin_cnt = 0;
  gchar bin_name[64];
  g_snprintf(bin_name, 64, "src_bin_%d", bin_cnt++);
  bin->bin = gst_bin_new(bin_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'src_bin'");
    return FALSE;
  }

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_V4L2:
      if (!create_camera_source_bin(config, bin)) {
        return FALSE;
      }
      break;
    case NV_DS_SOURCE_URI:
    case NV_DS_SOURCE_URI_MULTIPLE:
      if (!create_uridecode_src_bin(config, bin)) {
        return FALSE;
      }
      bin->live_source = config->live_source;
      break;
    case NV_DS_SOURCE_RTSP:
      if (!create_rtsp_src_bin(config, bin)) {
        return FALSE;
      }
      break;
    default:
      NVGSTDS_ERR_MSG_V("Source type not yet implemented!\n");
      return FALSE;
  }
  set_videoconvert_params(config, bin);
  GST_CAT_DEBUG(NVDS_APP, "Source bin created");

  return TRUE;
}

gboolean create_multi_source_bin(guint num_sub_bins, NvDsSourceConfig* configs, NvDsSrcParentBin* bin) {
  gboolean ret = FALSE;
  guint i = 0;
  guint uri_playlist_source_count = 0;
  const gchar* streammux_factory = NVDS_ELEM_STREAM_MUX;

  for (i = 0; i < num_sub_bins; ++i) {
    const gboolean has_explicit_playlist = configs[i].uri_list && *configs[i].uri_list;
    if (configs[i].enable && (configs[i].type == NV_DS_SOURCE_URI_MULTIPLE || has_explicit_playlist)) {
      ++uri_playlist_source_count;
    }
  }

  bin->reset_thread = NULL;
  g_mutex_init(&bin->uri_playlist_barrier_mutex);
  g_cond_init(&bin->uri_playlist_barrier_cond);
  bin->uri_playlist_next_frame_sequence = 0;
  bin->uri_playlist_paired_video_end = GST_CLOCK_TIME_NONE;
  bin->uri_playlist_exact_pairing_enabled = uri_playlist_source_count == 2;
  bin->uri_playlist_terminal = FALSE;
  bin->uri_playlist_barrier_failed = FALSE;

  bin->bin = gst_bin_new("multi_src_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'multi_src_bin'");
    goto done;
  }

  g_object_set(bin->bin, "message-forward", TRUE, NULL);

  if (uri_playlist_source_count == 2 && g_strcmp0(g_getenv("USE_NEW_NVSTREAMMUX"), "yes") == 0) {
    if (!register_hstream_lossless_nvstreammux()) {
      NVGSTDS_ERR_MSG_V("Failed to register the lossless URI-playlist stream mux");
      goto done;
    }
    streammux_factory = "hstreamlosslessmux";
  }
  bin->streammux = gst_element_factory_make(streammux_factory, "src_bin_muxer");
  if (!bin->streammux) {
    std::cout << "Could not create element " << streammux_factory
              << ", are all plugins registered properly, or possible libyaml-cpp.so.7 is not installed?" << std::endl;
    NVGSTDS_ERR_MSG_V("Failed to create element 'src_bin_muxer'");
    goto done;
  }
  gst_bin_add(GST_BIN(bin->bin), bin->streammux);

  for (i = 0; i < num_sub_bins; i++) {
    if (!configs[i].enable) {
      continue;
    }

    gchar elem_name[50];
    g_snprintf(elem_name, sizeof(elem_name), "src_sub_bin%d", i);
    bin->sub_bins[i].bin = gst_bin_new(elem_name);
    if (!bin->sub_bins[i].bin) {
      NVGSTDS_ERR_MSG_V("Failed to create '%s'", elem_name);
      goto done;
    }

    bin->sub_bins[i].bin_id = bin->sub_bins[i].source_id = i;
    configs[i].live_source = TRUE;
    bin->live_source = TRUE;
    bin->sub_bins[i].eos_done = TRUE;
    bin->sub_bins[i].reset_done = TRUE;

    bin->sub_bins[i].parent_bin = bin;

    switch (configs[i].type) {
      case NV_DS_SOURCE_CAMERA_CSI:
      case NV_DS_SOURCE_CAMERA_V4L2:
        if (!create_camera_source_bin(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      case NV_DS_SOURCE_URI:
      case NV_DS_SOURCE_URI_MULTIPLE:
        if (!create_uridecode_src_bin(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        bin->live_source = configs[i].live_source;
        break;
      case NV_DS_SOURCE_RTSP:
        if (!create_rtsp_src_bin(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      case NV_DS_SOURCE_AUDIO_WAV:
        if (!create_audio_source_bin(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      case NV_DS_SOURCE_AUDIO_URI:
        if (!create_uridecode_src_bin_audio(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        bin->live_source = configs->live_source;
        break;
      case NV_DS_SOURCE_ALSA_SRC:
        if (!create_audio_source_bin(&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      default:
        NVGSTDS_ERR_MSG_V("Source type not yet implemented!\n");
        return FALSE;
    }
    set_videoconvert_params(&configs[i], &bin->sub_bins[i]);
    gst_bin_add(GST_BIN(bin->bin), bin->sub_bins[i].bin);
    // print_pads(bin->streammux);
    // print_pads(bin->sub_bins[i].bin);
    if (!link_element_to_streammux_sink_pad(bin->streammux, bin->sub_bins[i].bin, i)) {
      NVGSTDS_ERR_MSG_V("source %d cannot be linked to mux's sink pad %p\n", i, bin->streammux);
      goto done;
    }

    bin->num_bins++;
  }
  if (!configure_lossless_uri_playlist_mux(bin)) {
    goto done;
  }
  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->streammux, "src");

  if (install_mux_eosmonitor_probe) {
    NVGSTDS_ELEM_ADD_PROBE(
        bin->nvstreammux_eosmonitor_probe,
        bin->streammux,
        "src",
        nvstreammux_eosmonitor_probe_func,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        bin);
  }

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

static void set_properties_nvuribin(GstElement* element_, NvDsSourceConfig const* config) {
  GstElementFactory* factory = GST_ELEMENT_GET_CLASS(element_)->elementfactory;
  if (!g_strcmp0(GST_OBJECT_NAME(factory), "nvurisrcbin"))
    g_object_set(element_, "uri", config->uri, NULL);
  if (config->num_extra_surfaces)
    g_object_set(element_, "num-extra-surfaces", config->num_extra_surfaces, NULL);
  if (config->gpu_id)
    g_object_set(element_, "gpu-id", config->gpu_id, NULL);
  g_object_set(element_, "cuda_memory_type", config->cuda_memory_type, NULL);
  g_object_set(element_, "low-latency-mode", config->low_latency_mode, NULL);
  if (config->drop_frame_interval)
    g_object_set(element_, "drop-frame-interval", config->drop_frame_interval, NULL);
  if (config->extract_sei_type5_data)
    g_object_set(element_, "extract-sei-type5-data", config->extract_sei_type5_data, NULL);
  if (config->select_rtp_protocol)
    g_object_set(element_, "select-rtp-protocol", config->select_rtp_protocol, NULL);
  if (config->loop)
    g_object_set(element_, "file-loop", config->loop, NULL);
  if (config->smart_record)
    g_object_set(element_, "smart-record", config->smart_record, NULL);
  if (config->smart_rec_cache_size)
    g_object_set(element_, "smart-rec-cache", config->smart_rec_cache_size, NULL);
  if (config->smart_rec_container)
    g_object_set(element_, "smart-rec-container", config->smart_rec_container, NULL);
  if (config->smart_rec_def_duration)
    g_object_set(element_, "smart-rec-default-duration", config->smart_rec_def_duration, NULL);
  if (config->rtsp_reconnect_interval_sec) {
    g_object_set(element_, "rtsp-reconnect-interval", config->rtsp_reconnect_interval_sec, NULL);
    g_object_set(element_, "rtsp-reconnect-attempts", config->rtsp_reconnect_attempts, NULL);
  }
  if (config->latency)
    g_object_set(element_, "latency", config->latency, NULL);
  if (config->udp_buffer_size)
    g_object_set(element_, "udp-buffer-size", config->udp_buffer_size, NULL);
}

gboolean create_nvmultiurisrcbin_bin(guint num_sub_bins, NvDsSourceConfig* configs, NvDsSrcParentBin* bin) {
  gboolean ret = FALSE;
  guint i = 0;

  bin->reset_thread = NULL;

  bin->bin = gst_bin_new("multiuri_src_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'multiuri_src_bin'");
    goto done;
  }

  g_object_set(bin->bin, "message-forward", TRUE, NULL);

  bin->nvmultiurisrcbin = bin->streammux = gst_element_factory_make(NVDS_ELEM_NVMULTIURISRCBIN, "src_nvmultiurisrcbin");
  if (!bin->streammux) {
    NVGSTDS_ERR_MSG_V("Failed to create element 'src_nvmultiurisrcbin'");
    goto done;
  }
  gst_bin_add(GST_BIN(bin->bin), bin->streammux);

  /** set properties for the nvurisrcbin if atleast one uri was provided */
  for (i = 0; i < num_sub_bins; i++) {
    if (!configs[i].enable) {
      continue;
    }
    set_properties_nvuribin(bin->nvmultiurisrcbin, &configs[i]);
  }
  if (num_sub_bins == 0) {
    set_properties_nvuribin(bin->nvmultiurisrcbin, configs);
  }

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->streammux, "src");

  if (install_mux_eosmonitor_probe) {
    NVGSTDS_ELEM_ADD_PROBE(
        bin->nvstreammux_eosmonitor_probe,
        bin->streammux,
        "src",
        nvstreammux_eosmonitor_probe_func,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        bin);
  }

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

gboolean reset_source_pipeline(gpointer data) {
  NvDsSrcBin* src_bin = (NvDsSrcBin*)data;
  GstState state = GST_STATE_NULL, pending = GST_STATE_NULL;
  GstStateChangeReturn ret;

  g_mutex_lock(&src_bin->bin_lock);
  gettimeofday(&src_bin->last_buffer_time, NULL);
  gettimeofday(&src_bin->last_reconnect_time, NULL);
  g_mutex_unlock(&src_bin->bin_lock);

  GstElement* send_event_element = NULL;
  if (src_bin->dewarper_bin.bin != NULL) {
    send_event_element = src_bin->dewarper_bin.bin;
  } else {
    send_event_element = src_bin->cap_filter1;
  }
  gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_start());
  gst_element_send_event(GST_ELEMENT(send_event_element), gst_event_new_flush_stop(TRUE));
  if (gst_element_set_state(src_bin->bin, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR_OBJECT(src_bin->bin, "Can't set source bin to NULL");
    return FALSE;
  }
  NVGSTDS_INFO_MSG_V("Resetting source %d", src_bin->bin_id);

  GST_CAT_INFO(NVDS_APP, "Reset source pipeline %s %p\n,", __func__, src_bin);
  if (!gst_element_sync_state_with_parent(src_bin->bin)) {
    GST_ERROR_OBJECT(src_bin->bin, "Couldn't sync state with parent");
  }

  if (src_bin->parser != NULL) {
    if (!gst_element_send_event(GST_ELEMENT(src_bin->parser), gst_nvevent_new_stream_reset(0)))
      GST_ERROR_OBJECT(src_bin->parser, "Interrupted, Reconnection event not sent");
  }

  ret = gst_element_get_state(src_bin->bin, &state, &pending, 0);

  GST_CAT_DEBUG(
      NVDS_APP,
      "Bin %d %p: state:%s pending:%s ret:%s",
      src_bin->bin_id,
      src_bin,
      gst_element_state_get_name(state),
      gst_element_state_get_name(pending),
      gst_element_state_change_return_get_name(ret));

  if (ret == GST_STATE_CHANGE_ASYNC || ret == GST_STATE_CHANGE_NO_PREROLL) {
    if (!src_bin->async_state_watch_running)
      g_timeout_add(20, watch_source_async_state_change, src_bin);
    src_bin->async_state_watch_running = TRUE;
    src_bin->reconfiguring = TRUE;
  } else if (ret == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PLAYING) {
    src_bin->reconfiguring = FALSE;
  }
  return FALSE;
}

gboolean set_source_to_playing(gpointer data) {
  NvDsSrcBin* subBin = (NvDsSrcBin*)data;
  if (subBin->reconfiguring) {
    gst_element_set_state(subBin->bin, GST_STATE_PLAYING);
    GST_CAT_INFO(NVDS_APP, "Reconfiguring %s  %p\n,", __func__, subBin);

    subBin->reconfiguring = FALSE;
  }
  return FALSE;
}

gpointer reset_encodebin(gpointer data) {
  NvDsSrcBin* src_bin = (NvDsSrcBin*)data;
  g_usleep(10000);
  GST_CAT_INFO(NVDS_APP, "Reset called %s %p\n,", __func__, src_bin);

  GST_CAT_INFO(NVDS_APP, "Reset setting null for sink %s %p\n,", __func__, src_bin);
  src_bin->reset_done = TRUE;

  return NULL;
}
