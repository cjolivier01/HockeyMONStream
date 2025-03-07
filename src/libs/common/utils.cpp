#include "utils.h"

namespace hm {
GPrintOStream gout;

std::optional<std::pair<int, int>> extract_width_height(GstCaps* caps) {
  if (caps == NULL) {
    return std::nullopt;
  }

  // Extract the first structure from the caps
  GstStructure* structure = gst_caps_get_structure(caps, 0);
  if (structure == NULL) {
    return std::nullopt;
  }

  // Retrieve width and height
  int width = 0, height = 0;
  if (gst_structure_get_int(structure, "width", &width) && gst_structure_get_int(structure, "height", &height)) {
    return std::make_pair(width, height);
  }
  return std::nullopt;
}

YAML::Node deep_copy(const YAML::Node& node) {
    // Serialize the original node to a string
    std::string dumped = YAML::Dump(node);
    // Parse the string back to a new YAML::Node
    return YAML::Load(dumped);
}

} // namespace hm
