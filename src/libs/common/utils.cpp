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

std::tuple<size_t, size_t> resize_to_fit(size_t origWidth, size_t origHeight, size_t maxWidth, size_t maxHeight) {
  // If the image is already within the bounds, return the original dimensions.
  if (origWidth <= maxWidth && origHeight <= maxHeight) {
    return {origWidth, origHeight};
  }

  // Calculate the scale factor for each dimension.
  double scaleWidth = static_cast<double>(maxWidth) / origWidth;
  double scaleHeight = static_cast<double>(maxHeight) / origHeight;

  // Use the smaller scale factor to ensure both dimensions fit within the bounds.
  double scale = std::min(scaleWidth, scaleHeight);

  // Compute new dimensions using the scale factor.
  size_t newWidth = static_cast<size_t>(origWidth * scale);
  size_t newHeight = static_cast<size_t>(origHeight * scale);

  return {newWidth, newHeight};
}

uint64_t hhmmss_to_nanoseconds(const std::string& hhmmss_string) {
  std::vector<std::string> tokens;
  size_t start = 0;
  size_t pos = 0;

  // Split the string by colon.
  while ((pos = hhmmss_string.find(':', start)) != std::string::npos) {
    tokens.push_back(hhmmss_string.substr(start, pos - start));
    start = pos + 1;
  }
  tokens.push_back(hhmmss_string.substr(start));

  double seconds = 0.0;
  if (tokens.size() == 3) {
    // Format: HH:MM:SS.ssss
    double hours = std::stod(tokens[0]);
    double minutes = std::stod(tokens[1]);
    double secs = std::stod(tokens[2]);
    seconds = hours * 3600.0 + minutes * 60.0 + secs;
  } else if (tokens.size() == 2) {
    // Format: MM:SS.ssss
    double minutes = std::stod(tokens[0]);
    double secs = std::stod(tokens[1]);
    seconds = minutes * 60.0 + secs;
  } else if (tokens.size() == 1) {
    // Format: SS.ssss (or just seconds)
    seconds = std::stod(tokens[0]);
  } else {
    throw std::invalid_argument("Invalid time string format: " + hhmmss_string);
  }

  // Convert seconds to nanoseconds.
  uint64_t nanoseconds = static_cast<uint64_t>(seconds * 1e9);
  return nanoseconds;
}

} // namespace hm
