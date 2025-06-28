#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stack>
#include <vector>

#include <opencv2/opencv.hpp>

#include "absl/strings/str_split.h"
#include "absl/cleanup/cleanup.h"

#define CHECK_MESSAGE_TYPE(message, type)                      \
  do {                                                         \
    const GstStructure* str;                                   \
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT)      \
      return FALSE;                                            \
    str = gst_message_get_structure(message);                  \
    return (str != NULL) && gst_structure_has_name(str, type); \
  } while (0)

namespace hm {

namespace {
constexpr const char* kHM_PIPELINE_EOS_STRUCT_NAME = "force-pipeline-eos";

#undef gst_element_get_parent

inline GstElement* gst_element_get_parent(GstElement* elem) {
  return (GstElement*)gst_object_get_parent(GST_OBJECT_CAST(elem));
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

[[maybe_unused]] void save_pipeline(const std::string& label, GstElement* any_element) {
  hm::GstReferencedObject<GstElement*> pipeline = hm::get_pipeline_element(any_element);
  if (pipeline) {
    hm::save_dot_file(pipeline.get(), GST_DEBUG_GRAPH_SHOW_ALL, label);
  }
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

Videoinfo getVideoInfo(const std::string& videoPath) {
  Videoinfo info;
  // Open the video file.
  cv::VideoCapture cap(videoPath);
  if (!cap.isOpened()) {
    std::cerr << "Error: Could not open video file: " << videoPath << std::endl;
    return info;
  }

  // Retrieve the FPS property.
  info.fps = cap.get(cv::CAP_PROP_FPS);
  info.frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);
  info.width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
  info.height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
  info.video_bit_rate = cap.get(cv::CAP_PROP_BITRATE) * 1000;
#if 0
  info.audio_samples_per_second = cap.get(cv::CAP_PROP_AUDIO_SAMPLES_PER_SECOND);
  info.num_audio_channels = cap.get(cv::CAP_PROP_AUDIO_TOTAL_CHANNELS);
  info.num_audio_streams = cap.get(cv::CAP_PROP_AUDIO_TOTAL_STREAMS);
#endif
  return info;
}

bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null) {
  if (dot_string.empty()) {
    return true;
  }
  std::vector<std::string> keys = absl::StrSplit(dot_string, '.');

  // Start from the root node
  const YAML::Node* current = &n;

  // Traverse the node tree according to the keys
  for (const auto& key : keys) {
    if (!current->IsMap()) {
      // If the current node is not a map, no further keys can be accessed
      return false;
    }

    // Access the next node in the path
    auto tmp = (*current)[key];
    current = &tmp;

    // Check if the current node is defined
    if (!current->IsDefined()) {
      return false;
    }
  }

  // If all keys are accessed and nodes are defined, return true
  return non_null ? !current->IsNull() : true;
}

/**
 * Split a dot-delimited string into its component parts
 *
 * @param input The dot-delimited string to split
 * @return A vector of string components
 */
std::vector<std::string> split_by_dot(const std::string& input) {
  std::vector<std::string> result;
  std::string current;

  for (char c : input) {
    if (c == '.') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (!current.empty()) {
    result.push_back(current);
  }

  return result;
}

/**
 * Set a value in a YAML node based on a dot-delimited path
 *
 * @param node The YAML node to modify
 * @param dot_string A dot-delimited string representing the path to the value
 * @param value The string value to set
 */
YAML::Node set_node_value(YAML::Node node, const std::string& dot_string, const std::string& value) {
  // std::cout << node << std::endl;

  std::vector<std::string> path = split_by_dot(dot_string);

  if (path.empty()) {
    return node; // Nothing to do with an empty path
  }

  // YAML::Node current = node;
  std::vector<YAML::Node> current_node;
  current_node.push_back(node);

  // Navigate to the second-to-last element in the path, creating nodes as needed
  for (size_t i = 0; i < path.size() - 1; ++i) {
    const std::string& key = path[i];
    // if (key == "hmplaycropper") {
    //   usleep(0);
    // }
    YAML::Node& current = current_node.back();
    // Check if the key exists and is a map
    if (!current[key] || !current[key].IsMap()) {
      // Create a new map node if it doesn't exist or isn't a map
      current[key] = YAML::Node(YAML::NodeType::Map);
    }
    // std::cout << node << std::endl;
    // std::cout << current << std::endl;
    // current = current[key];
    current_node.push_back(current[key]);
    // std::cout << current_node.back() << std::endl;
    // std::cout << node << std::endl;
  }

  // Set the value at the final path element
  current_node.back()[path.back()] = value;
  // std::cout << node << std::endl;
  return node;
}
#if 0
std::optional<YAML::Node> get_node(const YAML::Node& root, const std::string& dot_string) {
  // empty path → return the root itself
  if (dot_string.empty()) {
    return root;
  }

  // split on '.'
  std::vector<std::string> parts = absl::StrSplit(dot_string, '.');
  YAML::Node current = root;

  for (const auto& part : parts) {
    // parse "key" and optional "index"
    std::string key = part;
    std::optional<std::size_t> idx;
    auto colon_pos = part.find(':');
    if (colon_pos != std::string::npos) {
      key = part.substr(0, colon_pos);
      std::string idx_str = part.substr(colon_pos + 1);
      try {
        idx = static_cast<std::size_t>(std::stoul(idx_str));
      } catch (const std::exception&) {
        // invalid integer
        return std::nullopt;
      }
    }

    // if there's a non-empty key, descend into the map
    if (!key.empty()) {
      if (!current.IsMap()) {
        return std::nullopt;
      }
      current = current[key];
      if (!current.IsDefined()) {
        return std::nullopt;
      }
    }

    // if an index was specified, descend into the sequence
    if (idx.has_value()) {
      if (!current.IsSequence()) {
        return std::nullopt;
      }
      std::size_t i = *idx;
      if (i >= current.size()) {
        return std::nullopt;
      }
      current = current[i];
      if (!current.IsDefined()) {
        return std::nullopt;
      }
    }
  }

  return current;
}
#else
std::optional<YAML::Node> get_node(const YAML::Node& n, const std::string& dot_string) {
  if (dot_string.empty()) {
    return n;
  }
  std::vector<std::string> parts = absl::StrSplit(dot_string, '.');

  // Start from the root node
  const YAML::Node* current = &n;

  YAML::Node outer_tmp;

  // Traverse the node tree according to the keys
  for (const auto& part : parts) {
    std::string key = part;
    std::optional<std::size_t> idx;
    auto colon_pos = part.find(':');
    if (colon_pos != std::string::npos) {
      key = part.substr(0, colon_pos);
      std::string idx_str = part.substr(colon_pos + 1);
      try {
        idx = static_cast<std::size_t>(std::stoul(idx_str));
      } catch (const std::exception&) {
        // invalid integer
        return std::nullopt;
      }
    }

    if (idx.has_value()) {
      // DOES NOT CURRENT WORK
      std::cout << *current << std::endl;
      if (!current->IsSequence()) {
        return std::nullopt;
      }
      std::size_t i = *idx;
      if (i >= current->size()) {
        return std::nullopt;
      }
      auto tmp = (*current)[i];
      current = &tmp;
      if (!current->IsDefined()) {
        return std::nullopt;
      }
      continue;
    }

    if (!current->IsMap()) {
      // If the current node is not a map, no further keys can be accessed
      return std::nullopt;
    }

    // Access the next node in the path
    YAML::Node tmp = (*current)[key];
    current = &tmp;

    // Check if the current node is defined
    if (!current->IsDefined()) {
      return std::nullopt;
    }
  }

  // If all keys are accessed and nodes are defined, return true
  return *current;
}
#endif
bool seek_element(GstElement* seek_element, size_t seek_to_nanoseconds) {
  // size_t seekTarget = static_cast<size_t>(abs_seconds * 60 * GST_SECOND);
  size_t seekTarget = seek_to_nanoseconds;
  if (!gst_element_seek(
          seek_element,
          1.0, // Normal playback rate.
          GST_FORMAT_TIME,
          // (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          GST_SEEK_TYPE_SET, // Start from the target position.
          seekTarget,
          GST_SEEK_TYPE_NONE, // No specific end position.
          GST_CLOCK_TIME_NONE)) {
    g_printerr("Seek failed\n");
    return false;
  } else {
    g_print("Seek successful to %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(seekTarget));
    return true;
  }
}

void save_dot_file(GstElement* pipeline, GstDebugGraphDetails details, const std::string& filename) {
  gchar* dot_data = gst_debug_bin_to_dot_data(GST_BIN(pipeline), details);
  if (dot_data) {
    // Print to stdout
    // std::cout << dot_data << std::endl;

    // Or save to a file
    std::ofstream dot_file(TO_STRING(filename << ".dot"));
    if (dot_file.is_open()) {
      dot_file << dot_data;
      dot_file.close();
      std::cout << "DOT file saved as '" << filename << ".dot'" << std::endl;
    } else {
      std::cerr << "Failed to open file for writing." << std::endl;
    }

    g_free(dot_data);
  } else {
    std::cerr << "Failed to generate DOT data" << std::endl;
  }
}

const char* gstStateToString(GstState state) {
  switch (state) {
    case GST_STATE_VOID_PENDING:
      return "VOID_PENDING";
    case GST_STATE_NULL:
      return "NULL";
    case GST_STATE_READY:
      return "READY";
    case GST_STATE_PAUSED:
      return "PAUSED";
    case GST_STATE_PLAYING:
      return "PLAYING";
    default:
      return "UNKNOWN";
  }
}

void waitForPipelineStop(GstElement* pipeline) {
  // Get the pipeline's bus
  GstBus* bus = gst_element_get_bus(pipeline);

  // Record the time of the last message print
  auto lastPrintTime = std::chrono::steady_clock::now();
  bool done = false;

  while (!done) {
    // Poll the bus for messages with a timeout of 1 second (in nanoseconds)
    GstMessage* msg = gst_bus_timed_pop(bus, 1000000000);
    if (msg) {
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
          std::cout << "End-of-stream reached." << std::endl;
          done = true;
          break;
        case GST_MESSAGE_ERROR: {
          GError* err = nullptr;
          gchar* debug = nullptr;
          gst_message_parse_error(msg, &err, &debug);
          std::cerr << "Error from element " << GST_OBJECT_NAME(msg->src) << ": " << err->message << std::endl;
          if (debug)
            std::cerr << "Debug info: " << debug << std::endl;
          g_error_free(err);
          g_free(debug);
          done = true;
          break;
        }
        default:
          break;
      }
      gst_message_unref(msg);
    }

    // Check the current state of the pipeline without blocking
    GstState currentState, pendingState;
    gst_element_get_state(pipeline, &currentState, &pendingState, 0);
    if (currentState != GST_STATE_PLAYING) {
      done = true;
    }

    // Every 5 seconds, print a waiting message
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrintTime).count() >= 5) {
      std::cout << "Waiting for pipeline to stop..." << std::endl;
      lastPrintTime = now;
    }
  }

  gst_object_unref(bus);
}

// Returns a new reference to the pipeline element or nullptr if not found.
// Caller is responsible for unref'ing the returned pipeline.
GstElement* get_pipeline_element_raw(GstElement* element) {
  if (!element)
    return nullptr;

  GstElement* parent = GST_ELEMENT(gst_element_get_parent(element));
  if (!parent)
    return nullptr;

  if (GST_IS_PIPELINE(parent)) {
    return parent; // Found the pipeline; caller must unref it.
  }

  // Otherwise, recursively search the parent's parent.
  GstElement* pipeline = get_pipeline_element_raw(parent);

  gst_object_unref(parent);
  return pipeline;
}

GstReferencedObject<GstElement*> get_pipeline_element(GstElement* element) {
  return get_pipeline_element_raw(element);
}

//
// Force EOS messages
//
GstMessage* gst_nvmessage_force_pipeline_eos(GstObject* obj, bool force_eos) {
  GstStructure* str = gst_structure_new(kHM_PIPELINE_EOS_STRUCT_NAME, "force_eos", G_TYPE_BOOLEAN, force_eos, NULL);

  GstMessage* message = gst_message_new_custom(GST_MESSAGE_ELEMENT, obj, str);

  return message;
}

bool gst_message_is_force_pipeline_eos(GstMessage* message) {
  CHECK_MESSAGE_TYPE(message, kHM_PIPELINE_EOS_STRUCT_NAME);
}

bool gst_message_parse_force_pipeline_eos(GstMessage* message, bool* force_eos) {
  const GstStructure* str;

  if (!gst_message_is_force_pipeline_eos(message)) {
    return false;
  }

  str = gst_message_get_structure(message);

  gboolean b = FALSE;
  if (!gst_structure_get_boolean(str, "force_eos", &b)) {
    *force_eos = !!b;
    return false;
  }
  *force_eos = !!b;
  return true;
}

bool post_force_pipeline_eos(GstElement* element) {
  GstReferencedObject<GstElement*> pipeline = get_pipeline_element(element);
  absl::Cleanup cl([&pipeline]() { pipeline.release(); });
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  assert(bus);
  if (bus) {
    gst_bus_post(bus, gst_nvmessage_force_pipeline_eos(GST_OBJECT(pipeline.get()), true));
    gst_object_unref(bus);
    return true;
  }
  return false;
}

bool getCapsDimensions(GstCaps* caps, int& width, int& height) {
  // Ensure there is at least one structure.
  if (gst_caps_get_size(caps) == 0)
    return false;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  // Retrieve "width" and "height". Both must exist.
  if (!gst_structure_get_int(structure, "width", &width) || !gst_structure_get_int(structure, "height", &height)) {
    return false;
  }
  return true;
}

// Function to set width and height in a GstCaps.
// It returns a pointer to a writable GstCaps (which may be a new pointer if the original was immutable).
GstCaps* setCapsDimensions(GstCaps* caps, int width, int height) {
  // If the caps aren't writable, make a writable copy.
  if (!gst_caps_is_writable(caps)) {
    caps = gst_caps_make_writable(caps);
  }

  // Iterate over each structure in the caps and update the fields.
  guint n = gst_caps_get_size(caps);
  for (guint i = 0; i < n; i++) {
    GstStructure* structure = gst_caps_get_structure(caps, i);
    gst_structure_set(structure, "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, NULL);
  }
  return caps;
}

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
              << "\" (gst_pad_link return: " << gst_pad_link_get_name(link_ret) << ")." << std::endl;
    save_pipeline("link_failed", elem1);
    gst_object_unref(pad1);
    gst_object_unref(pad2);
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

} // namespace hm
