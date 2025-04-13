#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"
#include "deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/libs/common/pipeline_utils.h"

#include <glib-2.0/glib.h>
#include <gst/gstelement.h>
#include <gstreamer-1.0/gst/gstbin.h>
#include <gstreamer-1.0/gst/gstelementfactory.h>
#include <gstreamer-1.0/gst/gstpad.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
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

#undef gst_element_get_parent

namespace {
inline GstElement* gst_element_get_parent(GstElement* elem) {
  return (GstElement*)gst_object_get_parent(GST_OBJECT_CAST(elem));
}

NvDsSinkBinSubBin* find_sink_sub_bin(int sink_id, const NvDsSinkSubBinConfig* sink_config, NvDsSinkBin* sink_bins) {
  size_t sink_bin_index = 0;
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    const NvDsSinkSubBinConfig& config = sink_config[i];
    if (config.sink_id == (long)sink_id) {
      return &sink_bins->sub_bins[sink_bin_index];
    }
    if (config.enable) {
      ++sink_bin_index;
    }
  }
  return nullptr;
}

std::map<NvDsSinkType, std::vector<std::pair<const NvDsSinkSubBinConfig*, NvDsSinkBinSubBin*>>>
find_enabled_sink_sub_bins(const NvDsSinkSubBinConfig* sink_config, NvDsSinkBin* sink_bins) {
  std::map<NvDsSinkType, std::vector<std::pair<const NvDsSinkSubBinConfig*, NvDsSinkBinSubBin*>>> results;
  size_t sink_bin_index = 0;
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    const NvDsSinkSubBinConfig& config = sink_config[i];
    if (config.enable) {
      NvDsSinkBinSubBin* sink_sub_bin = &sink_bins->sub_bins[sink_bin_index];
      results[config.type].emplace_back(std::make_pair(&config, sink_sub_bin));
      ++sink_bin_index;
    }
  }
  return results;
}

//---------------------------------------------------------------------
// Helper: Find the lowest common ancestor (LCA) of two elements.
//---------------------------------------------------------------------
GstElement* findLowestCommonAncestor(GstElement* elem1, GstElement* elem2) {
  std::vector<GstElement*> chain1, chain2;

  for (GstElement* cur = elem1; cur; cur = gst_element_get_parent(cur)) {
    std::cout << GST_ELEMENT_NAME(cur) << std::endl;
    chain1.push_back(cur);
  }

  for (GstElement* cur = elem2; cur; cur = gst_element_get_parent(cur)) {
    std::cout << GST_ELEMENT_NAME(cur) << std::endl;
    chain2.push_back(cur);
  }

  std::reverse(chain1.begin(), chain1.end());
  std::reverse(chain2.begin(), chain2.end());

  GstElement* lca = nullptr;
  size_t minSize = std::min(chain1.size(), chain2.size());
  for (size_t i = 0; i < minSize; i++) {
    if (chain1[i] == chain2[i])
      lca = chain1[i];
    else
      break;
  }
  return lca;
}

//---------------------------------------------------------------------
// Updated Helper: Lift a pad from an element up to just below a given
// ancestor bin. Instead of lifting to the ancestor itself, we stop
// when the element's parent is the ancestor, so that no ghost pad is
// created on the least common ancestor.
// ghost_pad_name is the name to use for the ghost pad added to the
// immediate parent.
//---------------------------------------------------------------------
GstPad* liftPadToAncestor(
    GstElement* element,
    const char* pad_name,
    GstElement* ancestor,
    const std::string& ghost_pad_name) {
  // Get the pad from the element.
  GstPad* current_pad = gst_element_get_static_pad(element, pad_name);
  if (!current_pad) {
    std::cerr << "Error: Element \"" << GST_ELEMENT_NAME(element) << "\" has no pad named \"" << pad_name << "\"."
              << std::endl;
    return nullptr;
  }

  GstElement* current_element = element;
  // Continue lifting until the parent is the ancestor (not the ancestor itself).
  while (gst_element_get_parent(current_element) != ancestor) {
    GstElement* parent = gst_element_get_parent(current_element);
    if (!parent) {
      std::cerr << "Error: Could not get parent of element \"" << GST_ELEMENT_NAME(current_element)
                << "\" while lifting pad." << std::endl;
      gst_object_unref(current_pad);
      return nullptr;
    }
    // Check if the parent already has a ghost pad with the desired name.
    GstPad* existing_pad = gst_element_get_static_pad(parent, ghost_pad_name.c_str());
    if (existing_pad) {
      gst_object_unref(current_pad);
      current_pad = existing_pad;
    } else {
      // Create a ghost pad on the parent.
      GstPad* ghost_pad = gst_ghost_pad_new(ghost_pad_name.c_str(), current_pad);
      if (!ghost_pad) {
        std::cerr << "Error: Failed to create ghost pad \"" << ghost_pad_name << "\" for element \""
                  << GST_ELEMENT_NAME(current_element) << "\"." << std::endl;
        gst_object_unref(current_pad);
        return nullptr;
      }
      if (!gst_element_add_pad(parent, ghost_pad)) {
        std::cerr << "Error: Failed to add ghost pad \"" << ghost_pad_name << "\" to parent \""
                  << GST_ELEMENT_NAME(parent) << "\"." << std::endl;
        gst_object_unref(ghost_pad);
        gst_object_unref(current_pad);
        return nullptr;
      }
      // ghost_pad becomes our new current pad.
      current_pad = ghost_pad;
    }
    current_element = parent;
  }
  return current_pad;
}
} // namespace

//---------------------------------------------------------------------
// Main function: Given two GstElements and a pad name for each, and a
// ghost pad name, link the two pads. If the two pads are not in the same
// bin, ghost pads are created (and “lifted”) to the common ancestor so
// that they can be linked.
//---------------------------------------------------------------------
bool connectElementsWithGhostPads(
    GstElement* elem1,
    const char* pad1_name,
    GstElement* elem2,
    const char* pad2_name,
    const std::string& ghost_pad_name) {
  if (!elem1 || !elem2) {
    std::cerr << "Error: One or both elements are null." << std::endl;
    return false;
  }

  // Find the lowest common ancestor (LCA) of the two elements.
  GstElement* lca = findLowestCommonAncestor(elem1, elem2);
  if (!lca) {
    std::cerr << "Error: No common ancestor found between elements \"" << GST_ELEMENT_NAME(elem1) << "\" and \""
              << GST_ELEMENT_NAME(elem2) << "\"." << std::endl;
    return false;
  }
  std::cout << "Lowest common ancestor is: " << GST_ELEMENT_NAME(lca) << std::endl;

  GstPad *unref_pad1 = nullptr, *unref_pad2 = nullptr;
  GstPad* pad1 = nullptr;
  if (gst_element_get_parent(elem1) == lca) {
    pad1 = gst_element_get_static_pad(elem1, pad1_name);
    if (!pad1) {
      std::cerr << "Error: Element \"" << GST_ELEMENT_NAME(elem1) << "\" does not have pad \"" << pad1_name << "\"."
                << std::endl;
      return false;
    }
    unref_pad1 = pad1;
  } else {
    pad1 = liftPadToAncestor(elem1, pad1_name, lca, ghost_pad_name);
    if (!pad1) {
      std::cerr << "Error: Failed to lift pad \"" << pad1_name << "\" of element \"" << GST_ELEMENT_NAME(elem1)
                << "\" to just below the common ancestor." << std::endl;
      return false;
    }
  }

  // Use a different ghost pad name for the second element to avoid collisions.
  std::string ghost_pad_name2 = std::string("ghost_") + pad2_name;
  GstPad* pad2 = nullptr;
  if (gst_element_get_parent(elem2) == lca) {
    pad2 = gst_element_get_static_pad(elem2, pad2_name);
    if (!pad2) {
      std::cerr << "Error: Element \"" << GST_ELEMENT_NAME(elem2) << "\" does not have pad \"" << pad2_name << "\"."
                << std::endl;
      gst_object_unref(pad1);
      return false;
    }
    unref_pad2 = pad2;
  } else {
    pad2 = liftPadToAncestor(elem2, pad2_name, lca, ghost_pad_name2);
    if (!pad2) {
      std::cerr << "Error: Failed to lift pad \"" << pad2_name << "\" of element \"" << GST_ELEMENT_NAME(elem2)
                << "\" to just below the common ancestor." << std::endl;
      gst_object_unref(pad1);
      return false;
    }
  }

  // Attempt to link the pads.
  GstPadLinkReturn link_ret = gst_pad_link(pad1, pad2);
  if (link_ret != GST_PAD_LINK_OK) {
    std::cerr << "Error: Failed to link pad \"" << GST_PAD_NAME(pad1) << "\" to pad \"" << GST_PAD_NAME(pad2)
              << "\" (gst_pad_link return: " << link_ret << ")." << std::endl;
    gst_object_unref(pad1);
    // gst_object_unref(pad2);
    return false;
  }

  std::cout << "Successfully linked pad \"" << GST_PAD_NAME(pad1) << "\" to pad \"" << GST_PAD_NAME(pad2) << "\"."
            << std::endl;
  if (unref_pad1) {
    gst_object_unref(pad1);
  }
  if (unref_pad2) {
    gst_object_unref(pad2);
  }
  return true;
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

  ghost_pad_name = std::string("audio_in_") + std::to_string(audio_in_counter);

  ret = connectElementsWithGhostPads(postParse, src_pad_name, muxer, dest_pad_name, ghost_pad_name);

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

void setup_rgb_nvvm_caps_filter(GstCaps* caps, GstElement* cap_filter) {
  if (!caps) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL);
  }

  GstCapsFeatures* feature = gst_caps_features_new(MEMORY_FEATURES, NULL);
  gst_caps_set_features(caps, 0, feature);
  g_object_set(G_OBJECT(cap_filter), "caps", caps, NULL);
  gst_caps_unref(caps);
}

gboolean create_hmstitcher_bin(HmStitcherConfig* config, HmStitcherBin* bin) {
  gboolean ret = FALSE;
  std::stringstream ppc;

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
  ppc << ";show=" << config->show;
  g_object_set(G_OBJECT(bin->elem_hmstitcher), "plugin-private-config", ppc.str().c_str(), NULL);

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
gboolean create_dsfieldmask_bin(NvDsDsFieldMaskConfig* config, NvDsDsFieldMaskBin* bin) {
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
  g_object_set(G_OBJECT(bin->elem_dsplaytracker), "plugin-private-config", ppc.str().c_str(), NULL);

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

  bin->playcropper = gst_element_factory_make("videoprep", NULL);
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
  ppc << ";plot-play-tracking=" << config->plot_play_tracking;
  ppc << ";plot-player-tracking=" << config->plot_player_tracking;
  if (config->fixed_edge_rotation_angle != 0) {
    ppc << ";fixed-edge-rotation-angle=" << config->fixed_edge_rotation_angle;
  }
  ppc << ";no-crop=" << config->no_crop;

  g_object_set(G_OBJECT(bin->playcropper), "plugin-private-config", ppc.str().c_str(), NULL);

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

static void on_decode_pad_added(GstElement* element, GstPad* pad, gpointer data) {
  GstElement* convert = (GstElement*)data;
  const bool is_audio_pad = isAudioPad(pad);
  if (is_audio_pad) {
    GstPad* sinkpad = gst_element_get_static_pad(convert, "sink");
    GstPadLinkReturn ret;
    ret = gst_pad_link(pad, sinkpad);
    if (ret == GST_PAD_LINK_WRONG_HIERARCHY) {
      if (connectElementsWithGhostPads(element, GST_PAD_NAME(pad), convert, "sink", "hmaudio_source_bin")) {
        std::cout << "Linked " << GST_ELEMENT_NAME(element) << "." << GST_PAD_NAME(pad) << " to "
                  << GST_ELEMENT_NAME(convert) << ".sink" << std::endl;
        ret = GST_PAD_LINK_OK;
      }
    }
    if (GST_PAD_LINK_FAILED(ret)) {
      g_printerr("Decoder pad link failed: %d\n", ret);
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

bool link_elements(GstElement* elem1, GstElement* elem2) {
  if (!gst_element_link(elem1, elem2)) {
    GstCaps *src_caps, *sink_caps;
    const char* src_caps_str = "none";
    if ((elem1)->srcpads) {
      src_caps = gst_pad_query_caps((GstPad*)(elem1)->srcpads->data, NULL);
      src_caps_str = gst_caps_to_string(src_caps);
    }
    const char* sink_pad_str = "none";
    if ((elem2)->sinkpads) {
      sink_caps = gst_pad_query_caps((GstPad*)(elem2)->sinkpads->data, NULL);
      sink_pad_str = gst_caps_to_string(sink_caps);
    }
    NVGSTDS_ERR_MSG_V(
        "Failed to link '%s' (%s) and '%s' (%s)",
        GST_ELEMENT_NAME(elem1),
        src_caps_str,
        GST_ELEMENT_NAME(elem2),
        sink_pad_str);
    return false;
  }
  return true;
}

bool link_to_tee(GstElement* src_tee, GstElement* target) {
  // Manually link the tee to each queue
  auto tee_src_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(src_tee), "src_%u");
  GstPad* tee_pad = gst_element_request_pad(src_tee, tee_src_pad_template, NULL, NULL);
  GstPad* target_pad = gst_element_get_static_pad(target, "sink");
  bool link_ok = gst_pad_link(tee_pad, target_pad) == GST_PAD_LINK_OK;
  gst_object_unref(target_pad);

  if (!link_ok) {
    g_printerr("Tee could not be linked to the target.\n");
    return false;
  }
  return true;
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
  bool linked = false;

  std::map<NvDsSinkType, std::vector<std::pair<const NvDsSinkSubBinConfig*, NvDsSinkBinSubBin*>>>
      enabled_sink_sub_bins = find_enabled_sink_sub_bins(sink_config_array, sink_bin);

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
        // atm, only source uri is supported
        assert(source_config->type == NV_DS_SOURCE_URI);
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
  const NvDsSinkSubBinConfig* sink_config{nullptr};
  bool is_dest_file_sink = false;
  bool is_dest_alsa_sink = false;

  std::map<NvDsSinkType, const NvDsSinkSubBinConfig*> multi_sink_configs;
  NvDsSinkBinSubBin* target_sink_bin{nullptr};

  if (config->dest == DEST_SINK && config->sink_id == -1 && enabled_sink_sub_bins.size() == 1) {
    sink_config = enabled_sink_sub_bins.begin()->second.at(0).first;
    target_sink_bin = enabled_sink_sub_bins.begin()->second.at(0).second;
  } else if (config->dest == DEST_SINK || config->dest == DEST_MULTI_SINK) {
    for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
      if (sink_config_array[i].sink_id == config->sink_id) {
        sink_config = &sink_config_array[i];
        multi_sink_configs[sink_config->type] = sink_config;
        break;
      }
    }
    if (!sink_config) {
      std::cout << "HMAudio references missing or disabled sink-id " << config->sink_id << ", so disabling audio"
                << std::endl;
      return true;
    }
  }

  if (sink_config) {
    is_dest_file_sink = sink_config->type == NvDsSinkType::NV_DS_SINK_ENCODE_FILE;
#ifndef IS_TEGRA
    is_dest_alsa_sink = sink_config->type == NvDsSinkType::NV_DS_SINK_RENDER_EGL;
#else
    is_dest_alsa_sink = sink_config->type == NvDsSinkType::NV_DS_SINK_RENDER_3D;
#endif
  }

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
    if (!is_dest_file_sink) {
      HMGST_ELEMENT_MAKE_BINADD(bin->decodebin, "decodebin", "hmaudio_decoder");
      HMGST_ELEMENT_MAKE_BINADD(bin->audioresample, "audioresample", "hmaudio_audioresample");
    }
    g_object_set(G_OBJECT(bin->audiosrc), "location", audio_location.c_str(), NULL);
  } else if (config->src == SRC_SOURCE_BIN) {
    if (!is_dest_file_sink) {
      HMGST_ELEMENT_MAKE_BINADD(bin->audioresample, "audioresample", "hmaudio_audioresample");
    }
  } else {
    HMGST_ELEMENT_MAKE_BINADD(bin->audiosrc, NVDS_ELEM_SRC_ALSA, "hmaudio_alsasrc0");
  }

  if (bin->decodebin || config->src == SRC_SOURCE_BIN) {
    if (!is_dest_file_sink) {
      bin->audioconvert = gst_element_factory_make(NVDS_ELEM_AUDIO_CONV, "hmaudio_audioconvert0");
      if (!bin->audioconvert) {
        NVGSTDS_ERR_MSG_V("Failed to create 'audioconvert0'");
        goto done;
      }
    }
  }

  HMGST_ELEMENT_MAKE_BINADD(bin->queue, NVDS_ELEM_QUEUE, "hmaudio_audioout_queue");

  if (is_dest_alsa_sink) {
    bin->audiosink = gst_element_factory_make("alsasink", "hmaudio_audiosink0");
    if (!bin->audiosink) {
      NVGSTDS_ERR_MSG_V("Failed to create 'audioout_queue'");
      goto done;
    }
    gst_bin_add(GST_BIN(bin->bin), bin->audiosink);
  }

  if (bin->audioconvert) {
    gst_bin_add_many(GST_BIN(bin->bin), bin->audioconvert, bin->queue, NULL);
  }

  HMGST_ELEMENT_MAKE_BINADD(bin->tee, "tee", "hmaudio_tee");

  if (config->src == SRC_FILE || is_src_file) {
    // Handle dynamic pad creation from demuxer
    if (bin->decodebin) {
      g_signal_connect(bin->qtdemux, "pad-added", G_CALLBACK(on_demuxer_pad_added), bin->decodebin);
      g_signal_connect(bin->decodebin, "pad-added", G_CALLBACK(on_decode_pad_added), bin->audioconvert);
    } else {
      g_signal_connect(bin->qtdemux, "pad-added", G_CALLBACK(on_demuxer_pad_added), bin->queue);
    }
    NVGSTDS_LINK_ELEMENT(bin->audiosrc, bin->tee);

    if (!link_to_tee(bin->tee, bin->qtdemux)) {
      goto done;
    }

    if (bin->audioconvert) {
      NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->audioresample);
      NVGSTDS_LINK_ELEMENT(bin->audioresample, bin->queue);
    }
  } else if (config->src == SRC_SOURCE_BIN) {
    assert(source_bin->src_elem);
    if (bin->audioconvert) {
      g_signal_connect(source_bin->src_elem, "pad-added", G_CALLBACK(on_decode_pad_added), bin->audioconvert);
      NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->audioresample);
      NVGSTDS_LINK_ELEMENT(bin->audioresample, bin->queue);
    } else {
      assert(!bin->audioresample);
      g_signal_connect(source_bin->src_elem, "pad-added", G_CALLBACK(on_decode_pad_added), bin->queue);
    }
  } else {
    NVGSTDS_LINK_ELEMENT(bin->audiosrc, bin->audioconvert);
    NVGSTDS_LINK_ELEMENT(bin->audioconvert, bin->queue);
  }

  if (sink_config) {
    // Ok lets look at the sink we're supposed to be paired with
    if (sink_config->enable) {
      if (sink_config->type == NV_DS_SINK_ENCODE_FILE) {
        assert(is_dest_file_sink);
        if (!target_sink_bin) {
          target_sink_bin = find_sink_sub_bin(config->sink_id, sink_config_array, sink_bin);
        }
        if (target_sink_bin) {
          assert(target_sink_bin->mux);
          if (config->src == SRC_SOURCE_BIN) {
            // We need to encode it back to aac and not save it raw to the video file
            HMGST_ELEMENT_MAKE_BINADD(bin->encoder, "voaacenc", "hmaudio_encoder");
            HMGST_ELEMENT_MAKE_BINADD(bin->audioparse, "aacparse", "hmaudio_aacparse");
            HMGST_ELEMENT_MAKE_BINADD(bin->post_queue, NVDS_ELEM_QUEUE, "hmaudio_post_queue");
            NVGSTDS_LINK_ELEMENT(bin->queue, bin->encoder);
            NVGSTDS_LINK_ELEMENT(bin->encoder, bin->audioparse);
            NVGSTDS_LINK_ELEMENT(bin->audioparse, bin->post_queue);
            NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->post_queue, "src");
          } else {
            HMGST_ELEMENT_MAKE_BINADD(bin->audioparse, "aacparse", "hmaudio_aacparse");
            NVGSTDS_LINK_ELEMENT(bin->queue, bin->audioparse);
            NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->audioparse, "src");
          }
          if (!link_audio_pad_to_muxer(bin->bin, target_sink_bin->mux)) {
            goto done;
          }
          linked = true;
        } else {
          std::cerr << "No sink available for sink id " << config->sink_id << std::endl;
        }
      } else if (sink_config->type == NV_DS_SINK_UDPSINK) {
        if (!target_sink_bin) {
          target_sink_bin = find_sink_sub_bin(config->sink_id, sink_config_array, sink_bin);
        }
        assert(target_sink_bin->rtppay_or_flvmux);
        HMGST_ELEMENT_MAKE_BINADD(bin->encoder, "voaacenc", "hmaudio_encoder");
        HMGST_ELEMENT_MAKE_BINADD(bin->audioparse, "aacparse", "hmaudio_aacparse");
        NVGSTDS_LINK_ELEMENT(bin->queue, bin->encoder);
        NVGSTDS_LINK_ELEMENT(bin->encoder, bin->audioparse);
        NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->audioparse, "src");
        if (!link_audio_pad_to_muxer(bin->bin, target_sink_bin->rtppay_or_flvmux, /*audio_pad_name=*/"audio")) {
          goto done;
        }
        linked = true;
      } else if (sink_config->type == NvDsSinkType::NV_DS_SINK_FAKE) {
        if (!create_fakesink_bin(&sink_config->render_config, &bin->fakesink_bin)) {
          g_printerr("Failed to make fakesink bin for hmaudio\n");
        }
        gboolean ok = gst_bin_add(parent_bin, bin->fakesink_bin.bin);
        assert(ok);
        HMGST_ELEMENT_MAKE_BINADD(bin->encoder, "voaacenc", "hmaudio_encoder");
        HMGST_ELEMENT_MAKE_BINADD(bin->audioparse, "aacparse", "hmaudio_aacparse");
        NVGSTDS_LINK_ELEMENT(bin->queue, bin->encoder);
        NVGSTDS_LINK_ELEMENT(bin->encoder, bin->audioparse);
        NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->audioparse, "src");
        NVGSTDS_LINK_ELEMENT(bin->bin, bin->fakesink_bin.bin);
        linked = true;
      } else {
        g_printerr("hmaudio doesn't know how to link to sink of type %d\n", (int)sink_config->type);
      }
    } else {
      g_printerr("hmaudio can't link to sink id %d because it is disabled\n", config->sink_id);
    }
  } else {
    g_printerr("Could not find sink-id %d referenced in hmaudio instance\n", config->sink_id);
    goto done;
  }
  if (!linked) {
    if (!is_dest_alsa_sink) {
      NVGSTDS_BIN_ADD_GHOST_PAD(bin->bin, bin->queue, "src");
    } else {
      if (*config->alsa_dest_device) {
        g_object_set(
            G_OBJECT(bin->audiosink),
            "device",
            config->alsa_dest_device, // Specify ALSA device
            NULL);
      }
      NVGSTDS_LINK_ELEMENT(bin->queue, bin->audiosink);
    }
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V("%s failed", __func__);
  }

  return ret;
}

/**
 * get_parent_pipeline:
 * @element: a GstElement which may be nested inside bins.
 *
 * Returns: (transfer full): the parent pipeline if found, or NULL otherwise.
 *
 * This function walks up the parent chain by calling gst_element_get_parent()
 * until it finds an element that is a pipeline (i.e. GST_IS_PIPELINE() is true).
 * The returned pipeline is ref'ed so the caller is responsible for unrefing it.
 */
// GstElement* get_parent_pipeline(GstElement* element) {
//   GstElement* current = element;

//   while (current) {
//     GstObject* parent_obj = gst_element_get_parent(current);
//     if (!parent_obj)
//       break;

//     GstElement* parent_elem = GST_ELEMENT(parent_obj); // explicit cast

//     if (GST_IS_PIPELINE(parent_elem)) {
//       // Add a reference before returning.
//       gst_object_ref(parent_elem);
//       return parent_elem;
//     }

//     // Move up one level.
//     current = parent_elem;
//   }

//   return NULL;
// }
