#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/libs/common/pipeline_utils.h" // For gst_element_request_pad_simple on jetson

#include <glib-2.0/glib.h>
#include <gst/gstelement.h>
#include <gstreamer-1.0/gst/gstbin.h>
#include <gstreamer-1.0/gst/gstelementfactory.h>
#include <gstreamer-1.0/gst/gstobject.h>
#include <gstreamer-1.0/gst/gstpad.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define HMGST_ELEMENT_MAKE(dest$, factoryname$, name$)                                 \
  do {                                                                                 \
    (dest$) = gst_element_factory_make(factoryname$, name$);                           \
    if (!(dest$)) {                                                                    \
      std::stringstream ss;                                                            \
      ss << "Failed to create '" << (name$) << "' of type '" << (factoryname$) << "'"; \
      std::string msg = ss.str();                                                      \
      g_print("** ERROR: <%s:%d>: %s\n", __func__, __LINE__, msg.c_str());             \
      goto done;                                                                       \
    }                                                                                  \
  } while (false)

#define HMGST_ELEMENT_MAKE_BINADD(dest$, factoryname$, name$) \
  do {                                                        \
    HMGST_ELEMENT_MAKE(dest$, factoryname$, name$);           \
    gst_bin_add(GST_BIN(bin->bin), dest$);                    \
  } while (false)

namespace {

struct HmAudioSinkTarget {
  const NvDsSinkSubBinConfig* config{nullptr};
  NvDsSinkBinSubBin* sub_bin{nullptr};
};

bool link_elements(GstElement* elem1, GstElement* elem2) {
  if (!gst_element_link(elem1, elem2)) {
    GstCaps* src_caps = nullptr;
    GstCaps* sink_caps = nullptr;
    gchar* src_caps_str = nullptr;
    gchar* sink_caps_str = nullptr;
    if ((elem1)->srcpads) {
      src_caps = gst_pad_query_caps((GstPad*)(elem1)->srcpads->data, NULL);
      src_caps_str = gst_caps_to_string(src_caps);
    }
    if ((elem2)->sinkpads) {
      sink_caps = gst_pad_query_caps((GstPad*)(elem2)->sinkpads->data, NULL);
      sink_caps_str = gst_caps_to_string(sink_caps);
    }
    NVGSTDS_ERR_MSG_V(
        "Failed to link '%s' (%s) and '%s' (%s)",
        GST_ELEMENT_NAME(elem1),
        src_caps_str ? src_caps_str : "none",
        GST_ELEMENT_NAME(elem2),
        sink_caps_str ? sink_caps_str : "none");
    if (src_caps) {
      gst_caps_unref(src_caps);
    }
    if (sink_caps) {
      gst_caps_unref(sink_caps);
    }
    if (src_caps_str) {
      g_free(src_caps_str);
    }
    if (sink_caps_str) {
      g_free(sink_caps_str);
    }
    return false;
  }
  return true;
}

bool link_to_tee(GstElement* src_tee, GstElement* target) {
  auto tee_src_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(src_tee), "src_%u");
  GstPad* tee_pad = gst_element_request_pad(src_tee, tee_src_pad_template, NULL, NULL);
  GstPad* target_pad = gst_element_get_static_pad(target, "sink");
  bool link_ok = tee_pad && target_pad && gst_pad_link(tee_pad, target_pad) == GST_PAD_LINK_OK;
  if (target_pad) {
    gst_object_unref(target_pad);
  }
  if (tee_pad) {
    gst_object_unref(tee_pad);
  }

  if (!link_ok) {
    g_printerr("Tee could not be linked to the target.\n");
    return false;
  }
  return true;
}

[[maybe_unused]] NvDsSinkBinSubBin* find_sink_sub_bin(
    int sink_id,
    const NvDsSinkSubBinConfig* sink_config,
    NvDsSinkBin* sink_bins) {
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    const NvDsSinkSubBinConfig& config = sink_config[i];
    if (config.sink_id == (long)sink_id) {
      return &sink_bins->sub_bins[i];
    }
  }
  return nullptr;
}

[[maybe_unused]] std::map<NvDsSinkType, std::vector<std::pair<const NvDsSinkSubBinConfig*, NvDsSinkBinSubBin*>>>
find_enabled_sink_sub_bins(const NvDsSinkSubBinConfig* sink_config, NvDsSinkBin* sink_bins) {
  std::map<NvDsSinkType, std::vector<std::pair<const NvDsSinkSubBinConfig*, NvDsSinkBinSubBin*>>> results;
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    const NvDsSinkSubBinConfig& config = sink_config[i];
    if (config.enable) {
      NvDsSinkBinSubBin* sink_sub_bin = &sink_bins->sub_bins[i];
      results[config.type].emplace_back(std::make_pair(&config, sink_sub_bin));
    }
  }
  return results;
}

bool link_audio_pad_to_muxer(GstElement* postParse, GstElement* muxer, const char* audio_pad_name = "audio_%u") {
  gboolean ret = false;
  GstPad* muxer_audio_pad{nullptr};
  gchar* src_pad_name = nullptr;
  gchar* dest_pad_name = nullptr;
  std::string ghost_pad_name;
  static std::atomic<int> audio_in_counter = 0;

  GstPad* postParse_src = gst_element_get_static_pad(postParse, "src");
  if (!postParse_src) {
    g_printerr("Could not get postParse src pad.\n");
    goto done;
  }
  src_pad_name = gst_pad_get_name(postParse_src);

  muxer_audio_pad = gst_element_request_pad_simple(muxer, audio_pad_name);
  if (!muxer_audio_pad) {
    g_printerr("Could not get request pad from muxer for audio.\n");
    goto done;
  }
  dest_pad_name = gst_pad_get_name(muxer_audio_pad);

  ghost_pad_name = std::string("audio_in_") + std::to_string(audio_in_counter++);

  ret = hm::connectElementsWithGhostPads(postParse, src_pad_name, muxer, dest_pad_name, ghost_pad_name);

done:
  if (postParse_src) {
    gst_object_unref(postParse_src);
  }
  if (muxer_audio_pad) {
    gst_object_unref(muxer_audio_pad);
  }
  if (src_pad_name) {
    g_free(src_pad_name);
  }
  if (dest_pad_name) {
    g_free(dest_pad_name);
  }
  return ret;
}

bool is_rtmp_server_sink(const NvDsSinkSubBinConfig* sink_config) {
  const char* output_file_path = sink_config ? sink_config->encoder_config.output_file_path : nullptr;
  return output_file_path && !strncmp(output_file_path, "rtmp:/", 6);
}

bool is_render_audio_sink(const NvDsSinkSubBinConfig* sink_config) {
#ifndef IS_TEGRA
  return sink_config->type == NvDsSinkType::NV_DS_SINK_RENDER_EGL;
#else
  return sink_config->type == NvDsSinkType::NV_DS_SINK_RENDER_3D;
#endif
}

bool hmaudio_supports_sink(const NvDsSinkSubBinConfig* sink_config) {
  if (!sink_config) {
    return false;
  }
  switch (sink_config->type) {
    case NV_DS_SINK_ENCODE_FILE:
    case NV_DS_SINK_UDPSINK:
    case NV_DS_SINK_WEBRTC:
    case NV_DS_SINK_FAKE:
      return true;
    default:
      return is_render_audio_sink(sink_config);
  }
}

bool hmaudio_sink_requires_raw_audio(const NvDsSinkSubBinConfig* sink_config) {
  if (!sink_config) {
    return false;
  }
  if (is_render_audio_sink(sink_config)) {
    return true;
  }
  switch (sink_config->type) {
    case NV_DS_SINK_UDPSINK:
    case NV_DS_SINK_WEBRTC:
      return true;
    default:
      return false;
  }
}

bool sink_sub_bin_is_created(const NvDsSinkBinSubBin* sub_bin) {
  return sub_bin && (sub_bin->bin || sub_bin->mux || sub_bin->sink || sub_bin->rtppay_or_flvmux);
}

std::set<gint> configured_multi_sink_ids(const NvDsHmAudioConfig* config) {
  std::set<gint> sink_ids;
  if (config->dest != DEST_MULTI_SINK) {
    return sink_ids;
  }
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    if (config->multi_sink_ids[i] >= 0) {
      sink_ids.insert(config->multi_sink_ids[i]);
    }
  }
  return sink_ids;
}

std::vector<HmAudioSinkTarget> collect_hmaudio_sink_targets(
    const NvDsHmAudioConfig* config,
    const NvDsSinkSubBinConfig* sink_config_array,
    NvDsSinkBin* sink_bin) {
  std::vector<HmAudioSinkTarget> targets;
  const std::set<gint> multi_sink_ids = configured_multi_sink_ids(config);

  if (config->dest != DEST_SINK && config->dest != DEST_MULTI_SINK) {
    return targets;
  }
  if (config->dest == DEST_MULTI_SINK && multi_sink_ids.empty()) {
    return targets;
  }

  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    const NvDsSinkSubBinConfig* sink_config = &sink_config_array[i];
    NvDsSinkBinSubBin* sub_bin = &sink_bin->sub_bins[i];
    if (!sink_config->enable || sink_config->link_to_demux || !hmaudio_supports_sink(sink_config)) {
      continue;
    }
    if (!sink_sub_bin_is_created(sub_bin)) {
      continue;
    }

    if (!multi_sink_ids.empty()) {
      if (!multi_sink_ids.count(sink_config->sink_id)) {
        continue;
      }
    } else if (config->sink_id >= 0 && static_cast<guint>(config->sink_id) != sink_config->sink_id) {
      continue;
    }

    targets.push_back({sink_config, sub_bin});
  }
  return targets;
}

bool hmaudio_targets_require_raw_audio(const std::vector<HmAudioSinkTarget>& targets) {
  return std::any_of(targets.begin(), targets.end(), [](const HmAudioSinkTarget& target) {
    return hmaudio_sink_requires_raw_audio(target.config);
  });
}

std::string branch_element_name(const char* prefix, const NvDsSinkSubBinConfig* sink_config, const char* suffix) {
  std::stringstream ss;
  ss << "hmaudio_" << prefix << "_sink" << sink_config->sink_id << "_" << suffix;
  return ss.str();
}

bool make_audio_bin_element(
    NvDsHmAudioBin* bin,
    GstElement** element,
    const char* factory_name,
    const std::string& element_name) {
  *element = gst_element_factory_make(factory_name, element_name.c_str());
  if (!*element) {
    g_printerr("Failed to create '%s' of type '%s'\n", element_name.c_str(), factory_name);
    return false;
  }
  gst_bin_add(GST_BIN(bin->bin), *element);
  return true;
}

bool create_audio_branch_queue(NvDsHmAudioBin* bin, const NvDsSinkSubBinConfig* sink_config, GstElement** queue) {
  if (!make_audio_bin_element(bin, queue, NVDS_ELEM_QUEUE, branch_element_name("branch", sink_config, "queue"))) {
    return false;
  }
  if (sink_config->type != NV_DS_SINK_ENCODE_FILE) {
    g_object_set(G_OBJECT(*queue), "leaky", 2, "max-size-buffers", 30, "max-size-time", 0, "max-size-bytes", 0, NULL);
  }
  return link_to_tee(bin->tee, *queue);
}

bool link_audio_elements(std::initializer_list<GstElement*> elements) {
  if (elements.size() < 2) {
    return true;
  }
  auto it = elements.begin();
  GstElement* previous = *it;
  ++it;
  for (; it != elements.end(); ++it) {
    if (!link_elements(previous, *it)) {
      return false;
    }
    previous = *it;
  }
  return true;
}

bool create_file_audio_branch(NvDsHmAudioBin* bin, const HmAudioSinkTarget& target, bool input_encoded_aac) {
  if (!target.sub_bin->mux) {
    g_printerr("HMAudio could not find file muxer for sink id %u\n", target.config->sink_id);
    return false;
  }

  GstElement* queue{nullptr};
  GstElement* encoder{nullptr};
  GstElement* parser{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue)) {
    return false;
  }
  if (input_encoded_aac) {
    if (!make_audio_bin_element(bin, &parser, "aacparse", branch_element_name("file", target.config, "aacparse"))) {
      return false;
    }
    if (!link_audio_elements({queue, parser})) {
      return false;
    }
  } else {
    if (!make_audio_bin_element(bin, &encoder, "voaacenc", branch_element_name("file", target.config, "encoder")) ||
        !make_audio_bin_element(bin, &parser, "aacparse", branch_element_name("file", target.config, "aacparse"))) {
      return false;
    }
    if (!link_audio_elements({queue, encoder, parser})) {
      return false;
    }
  }
  return link_audio_pad_to_muxer(parser, target.sub_bin->mux);
}

bool create_rtmp_audio_branch(NvDsHmAudioBin* bin, const HmAudioSinkTarget& target) {
  if (!target.sub_bin->rtppay_or_flvmux) {
    g_printerr("HMAudio could not find RTMP muxer for sink id %u\n", target.config->sink_id);
    return false;
  }

  GstElement* queue{nullptr};
  GstElement* encoder{nullptr};
  GstElement* parser{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue) ||
      !make_audio_bin_element(bin, &encoder, "voaacenc", branch_element_name("rtmp", target.config, "encoder")) ||
      !make_audio_bin_element(bin, &parser, "aacparse", branch_element_name("rtmp", target.config, "aacparse"))) {
    return false;
  }
  if (!link_audio_elements({queue, encoder, parser})) {
    return false;
  }
  return link_audio_pad_to_muxer(parser, target.sub_bin->rtppay_or_flvmux, /*audio_pad_name=*/"audio");
}

bool create_rtsp_audio_branch(NvDsHmAudioBin* bin, const HmAudioSinkTarget& target) {
  if (!target.sub_bin->rtppay_or_flvmux) {
    g_printerr("HMAudio could not find RTSP video payloader for sink id %u\n", target.config->sink_id);
    return false;
  }

  GstElement* queue{nullptr};
  GstElement* audioconvert{nullptr};
  GstElement* audioresample{nullptr};
  GstElement* capsfilter{nullptr};
  GstElement* payloader{nullptr};
  GstElement* udpsink{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue) ||
      !make_audio_bin_element(
          bin, &audioconvert, NVDS_ELEM_AUDIO_CONV, branch_element_name("rtsp", target.config, "audioconvert")) ||
      !make_audio_bin_element(bin, &audioresample, "audioresample", branch_element_name("rtsp", target.config, "resample")) ||
      !make_audio_bin_element(bin, &capsfilter, "capsfilter", branch_element_name("rtsp", target.config, "caps")) ||
      !make_audio_bin_element(bin, &payloader, "rtpL16pay", branch_element_name("rtsp", target.config, "l16pay")) ||
      !make_audio_bin_element(bin, &udpsink, "udpsink", branch_element_name("rtsp", target.config, "udpsink"))) {
    return false;
  }

  GstCaps* rtsp_audio_caps = gst_caps_new_simple(
      "audio/x-raw",
      "format",
      G_TYPE_STRING,
      "S16BE",
      "layout",
      G_TYPE_STRING,
      "interleaved",
      "rate",
      G_TYPE_INT,
      hm::kRtspAudioRate,
      "channels",
      G_TYPE_INT,
      hm::kRtspAudioChannels,
      NULL);
  if (!rtsp_audio_caps) {
    g_printerr("Failed to create RTSP audio caps\n");
    return false;
  }
  g_object_set(G_OBJECT(capsfilter), "caps", rtsp_audio_caps, NULL);
  gst_caps_unref(rtsp_audio_caps);

  g_object_set(G_OBJECT(payloader), "pt", hm::kRtspAudioPayloadType, NULL);
  g_object_set(
      G_OBJECT(udpsink),
      "host",
      "127.0.0.1",
      "port",
      target.config->encoder_config.udp_port + hm::kRtspAudioUdpPortOffset,
      "async",
      FALSE,
      "sync",
      target.config->sync,
      NULL);

  return link_audio_elements({queue, audioconvert, audioresample, capsfilter, payloader, udpsink});
}

bool create_webrtc_audio_branch(NvDsHmAudioBin* bin, const HmAudioSinkTarget& target) {
  constexpr guint kWebRtcAudioPayloadType = 97;
  if (!target.sub_bin->sink) {
    g_printerr("HMAudio could not find WebRTC sink for sink id %u\n", target.config->sink_id);
    return false;
  }

  GstElement* queue{nullptr};
  GstElement* audioconvert{nullptr};
  GstElement* audioresample{nullptr};
  GstElement* raw_capsfilter{nullptr};
  GstElement* opusenc{nullptr};
  GstElement* rtppay{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue) ||
      !make_audio_bin_element(
          bin, &audioconvert, NVDS_ELEM_AUDIO_CONV, branch_element_name("webrtc", target.config, "audioconvert")) ||
      !make_audio_bin_element(bin, &audioresample, "audioresample", branch_element_name("webrtc", target.config, "resample")) ||
      !make_audio_bin_element(bin, &raw_capsfilter, "capsfilter", branch_element_name("webrtc", target.config, "rawcaps")) ||
      !make_audio_bin_element(bin, &opusenc, "opusenc", branch_element_name("webrtc", target.config, "opusenc")) ||
      !make_audio_bin_element(bin, &rtppay, "rtpopuspay", branch_element_name("webrtc", target.config, "rtpopuspay"))) {
    return false;
  }

  GstCaps* raw_caps = gst_caps_new_simple(
      "audio/x-raw",
      "format",
      G_TYPE_STRING,
      "S16LE",
      "layout",
      G_TYPE_STRING,
      "interleaved",
      "rate",
      G_TYPE_INT,
      48000,
      "channels",
      G_TYPE_INT,
      2,
      NULL);
  if (!raw_caps) {
    g_printerr("Failed to create WebRTC raw audio caps\n");
    return false;
  }
  g_object_set(G_OBJECT(raw_capsfilter), "caps", raw_caps, NULL);
  gst_caps_unref(raw_caps);

  g_object_set(G_OBJECT(opusenc), "bitrate", 128000, NULL);
  g_object_set(G_OBJECT(rtppay), "pt", kWebRtcAudioPayloadType, NULL);

  if (!link_audio_elements({queue, audioconvert, audioresample, raw_capsfilter, opusenc, rtppay})) {
    return false;
  }

  GstCaps* rtp_caps = gst_caps_new_simple(
      "application/x-rtp",
      "media",
      G_TYPE_STRING,
      "audio",
      "encoding-name",
      G_TYPE_STRING,
      "OPUS",
      "payload",
      G_TYPE_INT,
      kWebRtcAudioPayloadType,
      "clock-rate",
      G_TYPE_INT,
      48000,
      NULL);
  if (!rtp_caps) {
    g_printerr("Failed to create WebRTC RTP audio caps\n");
    return false;
  }
  const gboolean linked = link_webrtc_rtp_src_to_sink(target.sub_bin->sink, rtppay, rtp_caps, "audio");
  gst_caps_unref(rtp_caps);
  return linked;
}

bool create_render_audio_branch(
    NvDsHmAudioBin* bin,
    const HmAudioSinkTarget& target,
    const NvDsHmAudioConfig* hmaudio_config) {
  GstElement* queue{nullptr};
  GstElement* audioconvert{nullptr};
  GstElement* audioresample{nullptr};
  GstElement* audiosink{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue) ||
      !make_audio_bin_element(
          bin, &audioconvert, NVDS_ELEM_AUDIO_CONV, branch_element_name("render", target.config, "audioconvert")) ||
      !make_audio_bin_element(bin, &audioresample, "audioresample", branch_element_name("render", target.config, "resample")) ||
      !make_audio_bin_element(bin, &audiosink, "alsasink", branch_element_name("render", target.config, "alsasink"))) {
    return false;
  }
  if (*hmaudio_config->alsa_dest_device) {
    g_object_set(G_OBJECT(audiosink), "device", hmaudio_config->alsa_dest_device, NULL);
  }
  return link_audio_elements({queue, audioconvert, audioresample, audiosink});
}

bool create_fake_audio_branch(NvDsHmAudioBin* bin, const HmAudioSinkTarget& target) {
  GstElement* queue{nullptr};
  GstElement* fakesink{nullptr};
  if (!create_audio_branch_queue(bin, target.config, &queue) ||
      !make_audio_bin_element(bin, &fakesink, "fakesink", branch_element_name("fake", target.config, "sink"))) {
    return false;
  }
  g_object_set(G_OBJECT(fakesink), "sync", FALSE, "async", FALSE, NULL);
  return link_audio_elements({queue, fakesink});
}

bool create_audio_branch_for_target(
    NvDsHmAudioBin* bin,
    const HmAudioSinkTarget& target,
    const NvDsHmAudioConfig* hmaudio_config,
    bool input_encoded_aac) {
  if (input_encoded_aac && hmaudio_sink_requires_raw_audio(target.config)) {
    g_printerr(
        "HMAudio sink id %u requires raw audio, but the shared audio branch is encoded AAC\n", target.config->sink_id);
    return false;
  }

  if (target.config->type == NV_DS_SINK_ENCODE_FILE) {
    return create_file_audio_branch(bin, target, input_encoded_aac);
  }
  if (target.config->type == NV_DS_SINK_UDPSINK) {
    return is_rtmp_server_sink(target.config) ? create_rtmp_audio_branch(bin, target) : create_rtsp_audio_branch(bin, target);
  }
  if (target.config->type == NV_DS_SINK_WEBRTC) {
    return create_webrtc_audio_branch(bin, target);
  }
  if (target.config->type == NV_DS_SINK_FAKE) {
    return create_fake_audio_branch(bin, target);
  }
  if (is_render_audio_sink(target.config)) {
    return create_render_audio_branch(bin, target, hmaudio_config);
  }

  g_printerr("hmaudio doesn't know how to link to sink of type %d\n", static_cast<int>(target.config->type));
  return false;
}

void setup_rgb_nvvm_caps_filter(GstCaps* caps, GstElement* cap_filter) {
  if (!caps) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL);
  }

  GstCapsFeatures* feature = gst_caps_features_new(MEMORY_FEATURES, NULL);
  gst_caps_set_features(caps, 0, feature);
  g_object_set(G_OBJECT(cap_filter), "caps", caps, NULL);
  gst_caps_unref(caps);
}
} // namespace

gboolean create_hmstitcher_bin(HmStitcherConfig* config, HmStitcherBin* bin) {
  gboolean ret = FALSE;
  std::stringstream ppc;
  std::string private_config;

  bin->bin = gst_bin_new("hmstitcher_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hmstitcher_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "hmstitcher_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hmstitcher_queue'");
    goto done;
  }

  bin->elem_hmstitcher = gst_element_factory_make("videoprep", "hmstitcher0");
  if (!bin->elem_hmstitcher) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hmstitcher0'");
    goto done;
  }

  bin->pre_conv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "hmstitcher_conv0");
  if (!bin->pre_conv) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hmstitcher_conv0'");
    goto done;
  }

  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "hmstitcher_caps");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hmstitcher_caps'");
    goto done;
  }

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->pre_conv, bin->cap_filter, bin->elem_hmstitcher, NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->pre_conv);
  NVGSTDS_LINK_ELEMENT(bin->pre_conv, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->elem_hmstitcher);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");
  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->elem_hmstitcher, "src");
  // assert(false);
  // assert(strlen(config->detection_mask_file) > 0);

  ppc << "left-frame-offset-ns=" << config->left_frame_offset_ns;
  ppc << ";right-frame-offset-ns=" << config->right_frame_offset_ns;
  ppc << ";configure-only=" << config->configure_only;
  ppc << ";one-pass-mode=" << config->one_pass_mode;
  ppc << ";show=" << config->show;
  ppc << ";force-scoreboard-config=" << config->force_scoreboard_config;
  ppc << ";post-stitch-rotate-degrees=" << config->post_stitch_rotate_degrees;
  private_config = hm::gst::serialize_plugin_properties(config->private_properties, ppc.str());
  g_object_set(G_OBJECT(bin->elem_hmstitcher), "plugin-private-config", private_config.c_str(), NULL);

  g_object_set(G_OBJECT(bin->elem_hmstitcher), "unique-id", config->unique_id, "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->elem_hmstitcher), "plugin-type", "hmstitcher", NULL);
  g_object_set(G_OBJECT(bin->elem_hmstitcher), "config-file", config->config_file, NULL);
  g_object_set(G_OBJECT(bin->pre_conv), "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->pre_conv), "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

  if (config->num_output_buffers) {
    g_object_set(G_OBJECT(bin->elem_hmstitcher), "num-output-buffers", config->num_output_buffers, NULL);
  }
  if (config->num_batch_buffers) {
    g_object_set(G_OBJECT(bin->elem_hmstitcher), "num-batch-buffers", config->num_batch_buffers, NULL);
  }

  if (config->output_width) {
    g_object_set(G_OBJECT(bin->elem_hmstitcher), "output-width", config->output_width, NULL);
  }
  if (config->output_height) {
    g_object_set(G_OBJECT(bin->elem_hmstitcher), "output-height", config->output_height, NULL);
  }
  if (!hm::gst::apply_plugin_properties(G_OBJECT(bin->elem_hmstitcher), config->plugin_properties)) {
    goto done;
  }

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;

  return true;
}

/**
 *  ______  _       _     _ __  __             _
 * |  ____|(_)     | |   | |  \/  |           | |
 * | |__    _  ___ | | __| | \  / | __ _  ___ | | __
 * |  __|  | |/ _ \| |/ _` | |\/| |/ _` |/ __|| |/ /
 * | |     | |  __/| | (_| | |  | | (_| |\__ \|   <
 * |_|     |_|\___||_|\__,_|_|  |_|\__,_||___/|_|\_\
 *
 */

// Create bin, add queue and the element, link all elements and ghost pads,
// Set the element properties from the parsed config
gboolean create_dsfieldmask_bin(const NvDsDsFieldMaskConfig* config, NvDsDsFieldMaskBin* bin) {
  gboolean ret = FALSE;
  bin->bin = gst_bin_new("dsfieldmask_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsfieldmask_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "dsfieldmask_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsfieldmask_queue'");
    goto done;
  }

  bin->elem_dsfieldmask = gst_element_factory_make(NVDS_ELEM_DSFIELDMASK_ELEMENT, "dsfieldmask0");
  if (!bin->elem_dsfieldmask) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsfieldmask0'");
    goto done;
  }

  bin->pre_conv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "dsfieldmask_conv0");
  if (!bin->pre_conv) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsfieldmask_conv0'");
    goto done;
  }

  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "dsfieldmask_caps");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsfieldmask_caps'");
    goto done;
  }

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->pre_conv, bin->cap_filter, bin->elem_dsfieldmask, NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->pre_conv);
  NVGSTDS_LINK_ELEMENT(bin->pre_conv, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->elem_dsfieldmask);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");
  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->elem_dsfieldmask, "src");
  // assert(false);
  // assert(strlen(config->detection_mask_file) > 0);
  g_object_set(G_OBJECT(bin->elem_dsfieldmask), "unique-id", config->unique_id, "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->elem_dsfieldmask), "detection-mask", config->detection_mask_file, NULL);
  if (!hm::gst::apply_plugin_properties(G_OBJECT(bin->elem_dsfieldmask), config->plugin_properties)) {
    goto done;
  }
  g_object_set(G_OBJECT(bin->pre_conv), "gpu-id", config->gpu_id, NULL);

  g_object_set(G_OBJECT(bin->pre_conv), "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;
}

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

gboolean create_dsplaytracker_bin(NvDsDsPlayTrackerConfig* config, NvDsDsPlayTrackerBin* bin) {
  gboolean ret = FALSE;
  std::stringstream ppc;
  std::string private_config;
  bin->bin = gst_bin_new("dsplaytracker_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsplaytracker_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "dsplaytracker_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsplaytracker_queue'");
    goto done;
  }

  bin->elem_dsplaytracker = gst_element_factory_make(NVDS_ELEM_DSPLAYTRACKER_ELEMENT, "dsplaytracker0");
  if (!bin->elem_dsplaytracker) {
    NVGSTDS_ERR_MSG_V("Failed to create 'dsplaytracker0'");
    goto done;
  }

  gst_bin_add_many(GST_BIN(bin->bin), bin->queue, bin->elem_dsplaytracker, NULL);

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->elem_dsplaytracker);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");
  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->elem_dsplaytracker, "src");
  assert(strlen(config->config_file) > 0);
  g_object_set(G_OBJECT(bin->elem_dsplaytracker), "unique-id", config->unique_id, "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->elem_dsplaytracker), "config-file", config->config_file, NULL);
  g_object_set(G_OBJECT(bin->elem_dsplaytracker), "plugin-type", "vpplaytracker", NULL);
  // g_object_set(G_OBJECT(bin->elem_dsplaytracker), "draw", config->draw, NULL);

  ppc << "draw=" << config->draw;
  ppc << ";show=" << config->show;
  if (config->fixed_edge_rotation_angle != 0) {
    ppc << ";fixed-edge-rotation-angle=" << config->fixed_edge_rotation_angle;
  }
  ppc << ";dynamic-acceleration-scaling=" << config->dynamic_acceleration_scaling;
  private_config = hm::gst::serialize_plugin_properties(config->private_properties, ppc.str());
  g_object_set(G_OBJECT(bin->elem_dsplaytracker), "plugin-private-config", private_config.c_str(), NULL);
  if (!hm::gst::apply_plugin_properties(G_OBJECT(bin->elem_dsplaytracker), config->plugin_properties)) {
    goto done;
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;
}

/**
 *  _____  _              _____
 * |  __ \| |            / ____|
 * | |__) | | __ _ _   _| |     _ __  ___  _ __  _ __   ___  _ __
 * |  ___/| |/ _` | | | | |    | '__|/ _ \| '_ \| '_ \ / _ \| '__|
 * | |    | | (_| | |_| | |____| |  | (_) | |_) | |_) |  __/| |
 * |_|    |_|\__,_|\__, |\_____|_|   \___/| .__/| .__/ \___||_|
 *                  __/ |                 | |   | |
 *                 |___/                  |_|   |_|
 */
gboolean create_hmplaycropper_bin(HmPlayCropperConfig* config, NvDsHmVideoPrepBin* bin) {
  gboolean ret = FALSE;
  std::stringstream ppc;
  std::string private_config;
  constexpr size_t poly_int_count =
      sizeof(config->scoreboard_perspective_polygon) / sizeof(config->scoreboard_perspective_polygon[0]);

  bin->bin = gst_bin_new("playcropper_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_bin'");
    goto done;
  }

  bin->nvvidconv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, "playcropper_conv");

  if (!bin->nvvidconv) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_conv'");
    goto done;
  }

  bin->queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "playcropper_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_queue'");
    goto done;
  }

  bin->src_queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "playcropper_src_queue");
  if (!bin->src_queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_src_queue'");
    goto done;
  }

  if (config->fps_n) {
    assert(config->fps_d);
    bin->videorate = gst_element_factory_make("videorate", "playcropper_videorate");
    if (!bin->videorate) {
      NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_videorate'");
      goto done;
    }
    assert(false);
    // g_object_set(G_OBJECT(bin->videorate), "drop-only", TRUE, NULL);
  }

  bin->conv_queue = gst_element_factory_make(NVDS_ELEM_QUEUE, "playcropper_conv_queue");
  if (!bin->conv_queue) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_conv_queue'");
    goto done;
  }

  bin->cap_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "playcropper_caps");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper_caps'");
    goto done;
  }

  setup_rgb_nvvm_caps_filter(nullptr, bin->cap_filter);

  bin->playcropper = gst_element_factory_make("playcropper", NULL);
  if (!bin->playcropper) {
    NVGSTDS_ERR_MSG_V("Failed to create 'playcropper'");
    goto done;
  }

  bin->playcropper_caps_filter = gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "playcropper_caps_filter");
  if (!bin->playcropper_caps_filter) {
    NVGSTDS_ERR_MSG_V("Could not create 'playcropper_caps_filter'");
    goto done;
  }

  // We expect only RGBA images incoming (any size)
  setup_rgb_nvvm_caps_filter(
      gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "RGBA",
          "width",
          GST_TYPE_INT_RANGE,
          1,
          G_MAXINT,
          "height",
          GST_TYPE_INT_RANGE,
          1,
          G_MAXINT,
          NULL),
      bin->playcropper_caps_filter);

  gst_bin_add_many(
      GST_BIN(bin->bin),
      bin->queue,
      bin->src_queue,
      bin->conv_queue,
      bin->nvvidconv,
      bin->cap_filter,
      bin->playcropper,
      bin->playcropper_caps_filter,
      NULL);

  g_object_set(G_OBJECT(bin->nvvidconv), "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->nvvidconv), "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

  g_object_set(G_OBJECT(bin->playcropper), "gpu-id", config->gpu_id, NULL);
  g_object_set(G_OBJECT(bin->playcropper), "config-file", config->config_file, NULL);
  g_object_set(G_OBJECT(bin->playcropper), "plugin-type", config->plugin_type, NULL);

  g_object_set(G_OBJECT(bin->playcropper), "source-id", config->source_id, NULL);
  g_object_set(G_OBJECT(bin->playcropper), "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

  if (config->num_output_buffers) {
    g_object_set(G_OBJECT(bin->playcropper), "num-output-buffers", config->num_output_buffers, NULL);
  }
  if (config->num_batch_buffers) {
    g_object_set(G_OBJECT(bin->playcropper), "num-batch-buffers", config->num_batch_buffers, NULL);
  }
  if (config->output_width) {
    g_object_set(G_OBJECT(bin->playcropper), "output-width", config->output_width, NULL);
  }
  if (config->output_height) {
    g_object_set(G_OBJECT(bin->playcropper), "output-height", config->output_height, NULL);
  }

  ppc << "show=" << config->show;

  if (std::any_of(
          &config->scoreboard_perspective_polygon[0],
          &config->scoreboard_perspective_polygon[poly_int_count],
          [](const auto& i) { return i != 0; }) != 0) {
    ppc << ";scoreboard-perspective-polygon=";
    for (size_t i = 0, n = poly_int_count >> 1; i < n; ++i) {
      const size_t index = i << 1;
      if (i) {
        ppc << ",";
      }
      ppc << std::to_string(config->scoreboard_perspective_polygon[index]) << ","
          << config->scoreboard_perspective_polygon[index + 1];
    }
  }
  ppc << ";show-scoreboard=" << config->show_scoreboard;
  if (config->scoreboard_projected_width[0]) {
    ppc << ";scoreboard-projected-width=" << config->scoreboard_projected_width;
  }
  if (config->scoreboard_projected_height[0]) {
    ppc << ";scoreboard-projected-height=" << config->scoreboard_projected_height;
  }
  if (config->scoreboard_scale > 0) {
    ppc << ";scoreboard-scale=" << config->scoreboard_scale;
  }
  ppc << ";plot-play-tracking=" << config->plot_play_tracking;
  ppc << ";plot-player-tracking=" << config->plot_player_tracking;
  ppc << ";transform-object-meta=" << config->transform_object_meta;
  if (config->runtime_output_max_width) {
    ppc << ";runtime-output-max-width=" << config->runtime_output_max_width;
  }
  if (config->runtime_output_max_height) {
    ppc << ";runtime-output-max-height=" << config->runtime_output_max_height;
  }
  if (config->fixed_edge_rotation_angle != 0) {
    ppc << ";fixed-edge-rotation-angle=" << config->fixed_edge_rotation_angle;
  }
  ppc << ";no-crop=" << config->no_crop;

  private_config = hm::gst::serialize_plugin_properties(config->private_properties, ppc.str());
  g_object_set(G_OBJECT(bin->playcropper), "plugin-private-config", private_config.c_str(), NULL);
  if (!hm::gst::apply_plugin_properties(G_OBJECT(bin->playcropper), config->plugin_properties)) {
    goto done;
  }

#if 0
  NVGSTDS_LINK_ELEMENT(bin->nvvidconv, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->nvplaytracker);
  NVGSTDS_LINK_ELEMENT(bin->nvplaytracker, bin->playtracker_caps_filter);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->nvvidconv, "sink");
  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->playtracker_caps_filter, "src");
#else
  NVGSTDS_LINK_ELEMENT(bin->queue, bin->nvvidconv);

  NVGSTDS_LINK_ELEMENT(bin->nvvidconv, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT(bin->cap_filter, bin->conv_queue);

  NVGSTDS_LINK_ELEMENT(bin->conv_queue, bin->playcropper);

  NVGSTDS_LINK_ELEMENT(bin->playcropper, bin->playcropper_caps_filter);
  NVGSTDS_LINK_ELEMENT(bin->playcropper_caps_filter, bin->src_queue);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "sink");

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->src_queue, "src");
#endif
  ret = TRUE;
done:
  // if (caps) {
  //   gst_caps_unref(caps);
  // }

  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }
  return ret;
}

//
// HmImageMetaMerger
//
// OBSOLETE
gboolean create_hmimagemetamerger_bin(NvDsHmImageMetaMergerConfig* config, NvDsHmImageMetaMergerBin* bin) {
  gboolean ret = FALSE;
  // GstPad *bin_src_pad, *ghost_pad, *tee_src_pad;
  bin->bin = gst_bin_new("hm_image_meta_merger");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hm_image_meta_merger'");
    goto done;
  }

  g_object_set(bin->bin, "message-forward", TRUE, NULL);

  bin->image_identity_in = gst_element_factory_make("identity", "image_identity_in0");
  bin->meta_identity_in = gst_element_factory_make("identity", "meta_identity_in0");

  gst_bin_add_many(GST_BIN(bin->bin), bin->image_identity_in, bin->meta_identity_in, NULL);

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->meta_identity_in, "sink");

  NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->meta_identity_in, "src");
  // NVGSTDS_BIN_ADD_GHOST_PAD_NAMED(bin->bin, bin->meta_identity_in, "src", "src_1");

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;
}

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
//  GstElement *audiosrc = gst_element_factory_make("alsasrc", "my_audiosource");
bool isAudioPad(GstPad* pad) {
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps)
    caps = gst_pad_query_caps(pad, NULL);

  if (!caps)
    return false;

  const GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* mediaType = gst_structure_get_name(structure);

  bool result = (g_str_has_prefix(mediaType, "audio/") != 0);

  gst_caps_unref(caps);
  return result;
}

// template <typename DATA_T = GstElement*>
static void on_decode_pad_added(GstElement* element, GstPad* pad, gpointer* data) {
  GstElement* convert = (GstElement*)data;
  const bool is_audio_pad = isAudioPad(pad);
  if (is_audio_pad) {
    {
      hm::GstReferencedObject<GstElement*> pipeline = hm::get_pipeline_element(element);
      hm::save_dot_file(pipeline.get(), GST_DEBUG_GRAPH_SHOW_ALL, "on_audio_decode_pad_added");
    }

    GstPad* sinkpad = gst_element_get_static_pad(convert, "sink");
    GstPadLinkReturn ret;
    if (gst_pad_is_linked(sinkpad)) {
      GstPad* peer = gst_pad_get_peer(sinkpad);
      if (peer) {
        gst_pad_unlink(peer, sinkpad);
        gst_object_unref(peer);
      }
    }
    ret = gst_pad_link(pad, sinkpad);
    if (ret == GST_PAD_LINK_WRONG_HIERARCHY) {
      if (hm::connectElementsWithGhostPads(element, GST_PAD_NAME(pad), convert, "sink", "hmaudio_source_bin")) {
        std::cout << "Linked " << GST_ELEMENT_NAME(element) << "." << GST_PAD_NAME(pad) << " to "
                  << GST_ELEMENT_NAME(convert) << ".sink" << std::endl;
        ret = GST_PAD_LINK_OK;
      } else {
        std::cout << "Failed linked " << GST_ELEMENT_NAME(element) << "." << GST_PAD_NAME(pad) << " to "
                  << GST_ELEMENT_NAME(convert) << ".sink, reason: " << gst_pad_link_get_name(ret) << std::endl;
      }
    }
    if (GST_PAD_LINK_FAILED(ret)) {
      g_printerr("Decoder pad link failed: %d: %s\n", ret, gst_pad_link_get_name(ret));
      assert(false);
    }
    gst_object_unref(sinkpad);
  }
}

static void on_demuxer_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  GstElement* decoder = (GstElement*)data;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstStructure* str = gst_caps_get_structure(caps, 0);

  if (g_str_has_prefix(gst_structure_get_name(str), "audio/")) {
    GstPad* sinkpad = gst_element_get_static_pad(decoder, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
      g_printerr("Failed to link demuxer to decoder\n");
    }
    gst_object_unref(sinkpad);
  }

  gst_caps_unref(caps);
}

/**
 *  _    _                               _  _
 * | |  | |              /\             | |(_)
 * | |__| |_ __ ___     /  \   _   _  __| | _  ___
 * |  __  | '_ ` _ \   / /\ \ | | | |/ _` || |/ _ \
 * | |  | | | | | | | / ____ \| |_| | (_| || | (_) |
 * |_|  |_|_| |_| |_|/_/    \_\\__,_|\__,_||_|\___/
 *
 *
 */
gboolean create_hmaudio_bin(
    GstBin* parent_bin,
    const NvDsHmAudioConfig* config,
    NvDsHmAudioBin* bin,
    NvDsSrcBin* src_sub_bins,
    const NvDsSinkSubBinConfig* sink_config_array,
    NvDsSinkBin* sink_bin) {
  gboolean ret = FALSE;

  NvDsSrcBin* source_bin = nullptr;
  const NvDsSourceConfig* source_config{nullptr};
  if (config->src == SRC_SOURCE_BIN) {
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      if (src_sub_bins[i].source_id == config->source_id) {
        source_bin = &src_sub_bins[i];
        source_config = source_bin->config;
        if (!source_config) {
          std::cerr << "Source bin with source-id " << config->source_id
                    << " does not have the config pointer set, so aboring HMAudio" << std::endl;
          return true;
        }
        assert(source_config);
        if (source_config->type != NV_DS_SOURCE_URI && source_config->type != NV_DS_SOURCE_URI_MULTIPLE) {
          std::cerr << "HMAudio source-bin mode only supports URI sources, so disabling audio" << std::endl;
          return true;
        }
        break;
      }
    }
    if (!source_bin) {
      std::cout << "HMAudio references missing or disabled source-id " << config->sink_id << ", so disabling audio"
                << std::endl;
      return true;
    }
  }

  const std::string file_prefix = "file://";
  const bool is_file_prefix = !strncmp(config->audio_location, file_prefix.c_str(), file_prefix.size());
  std::string audio_location = is_file_prefix ? &config->audio_location[file_prefix.size()] : config->audio_location;
  const bool is_src_file = config->src == SRC_FILE || is_file_prefix;

  const std::vector<HmAudioSinkTarget> targets = collect_hmaudio_sink_targets(config, sink_config_array, sink_bin);
  if (targets.empty()) {
    std::cout << "HMAudio found no enabled compatible sink targets for sink-id " << config->sink_id
              << ", so disabling audio" << std::endl;
    return true;
  }
  const bool source_audio_needs_decode = is_src_file && hmaudio_targets_require_raw_audio(targets);
  const bool shared_audio_is_encoded_aac = is_src_file && !source_audio_needs_decode;
  const bool shared_audio_is_raw = !shared_audio_is_encoded_aac;

  bin->bin = gst_bin_new("hmaudio_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V("Failed to create 'hm_image_meta_merger'");
    goto done;
  }

  if (!gst_bin_add(parent_bin, bin->bin)) {
    g_printerr("Could not add to parent bin (hmaudio_bin)");
    goto done;
  }

  if (config->src == SRC_FILE || is_src_file) {
    HMGST_ELEMENT_MAKE_BINADD(bin->audiosrc, "filesrc", "hmaudio_filsrc");
    HMGST_ELEMENT_MAKE_BINADD(bin->qtdemux, "qtdemux", "hmaudio_demuxer");
    if (source_audio_needs_decode) {
      HMGST_ELEMENT_MAKE_BINADD(bin->decodebin, "decodebin", "hmaudio_decoder");
    }
    g_object_set(G_OBJECT(bin->audiosrc), "location", audio_location.c_str(), NULL);
  } else if (config->src == SRC_SOURCE_BIN) {
    // Source-bin audio arrives from the URI source's audio tee or decodebin pad.
  } else {
    HMGST_ELEMENT_MAKE_BINADD(bin->audiosrc, NVDS_ELEM_SRC_ALSA, "hmaudio_alsasrc0");
  }

  if (shared_audio_is_raw) {
    HMGST_ELEMENT_MAKE_BINADD(bin->audioconvert, NVDS_ELEM_AUDIO_CONV, "hmaudio_audioconvert0");
    HMGST_ELEMENT_MAKE_BINADD(bin->audioresample, "audioresample", "hmaudio_audioresample");
  }

  HMGST_ELEMENT_MAKE_BINADD(bin->queue, NVDS_ELEM_QUEUE, "hmaudio_audioout_queue");
  HMGST_ELEMENT_MAKE_BINADD(bin->tee, "tee", "hmaudio_tee");
  g_object_set(G_OBJECT(bin->tee), "allow-not-linked", TRUE, NULL);

  if (config->src == SRC_FILE || is_src_file) {
    // Handle dynamic pad creation from demuxer
    if (bin->decodebin) {
      g_signal_connect(bin->qtdemux, "pad-added", G_CALLBACK(on_demuxer_pad_added), bin->decodebin);
      g_signal_connect(bin->decodebin, "pad-added", G_CALLBACK(on_decode_pad_added), bin->audioconvert);
    } else {
      g_signal_connect(bin->qtdemux, "pad-added", G_CALLBACK(on_demuxer_pad_added), bin->queue);
    }
    NVGSTDS_LINK_ELEMENT(bin->audiosrc, bin->qtdemux);

    if (bin->audioconvert) {
      NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->audioresample);
      NVGSTDS_LINK_ELEMENT(bin->audioresample, bin->queue);
    }
  } else if (config->src == SRC_SOURCE_BIN) {
    if (bin->audioconvert) {
      assert(source_bin->src_elem);
      if (source_bin->uri_audio_tee) {
        if (!link_uri_source_audio_src(source_bin, bin->audioconvert)) {
          goto done;
        }
      } else {
        g_signal_connect(source_bin->src_elem, "pad-added", G_CALLBACK(on_decode_pad_added), bin->audioconvert);
      }
      NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->audioresample);
      NVGSTDS_LINK_ELEMENT(bin->audioresample, bin->queue);
    } else {
      assert(!bin->audioresample);
      assert(source_bin->src_elem);
      if (source_bin->uri_audio_tee) {
        if (!link_uri_source_audio_src(source_bin, bin->queue)) {
          goto done;
        }
      } else {
        g_signal_connect(source_bin->src_elem, "pad-added", G_CALLBACK(on_decode_pad_added), bin->queue);
      }
    }
  } else {
    assert(bin->audiosrc);
    assert(bin->audioconvert);
    NVGSTDS_LINK_ELEMENT(bin->audiosrc, bin->audioconvert);
    NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->audioresample);
    NVGSTDS_LINK_ELEMENT(bin->audioresample, bin->queue);
  }

  NVGSTDS_LINK_ELEMENT(bin->queue, bin->tee);

  for (const HmAudioSinkTarget& target : targets) {
    if (!create_audio_branch_for_target(bin, target, config, shared_audio_is_encoded_aac)) {
      goto done;
    }
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;
}
