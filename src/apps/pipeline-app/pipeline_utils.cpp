#include "pipeline_utils.h"

#include <fstream>
#include <iostream>
#include <vector>

namespace hm {

bool has_node(const YAML::Node& n, const std::string& dot_string) {
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
  return true;
}

std::optional<YAML::Node> get_node(YAML::Node& n, const std::string& dot_string) {
  if (dot_string.empty()) {
    return n;
  }
  std::vector<std::string> keys = absl::StrSplit(dot_string, '.');

  // Start from the root node
  const YAML::Node* current = &n;

  // Traverse the node tree according to the keys
  for (const auto& key : keys) {
    if (!current->IsMap()) {
      // If the current node is not a map, no further keys can be accessed
      return std::nullopt;
    }

    // Access the next node in the path
    auto tmp = (*current)[key];
    current = &tmp;

    // Check if the current node is defined
    if (!current->IsDefined()) {
      return std::nullopt;
    }
  }

  // If all keys are accessed and nodes are defined, return true
  return *current;
}

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

} // namespace hm
