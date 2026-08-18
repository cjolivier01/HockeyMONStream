#include "utils.h"

#include <cmath>
#include <limits>

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

  auto parse_component = [&hhmmss_string](const std::string& token, const char* component) {
    if (token.empty()) {
      throw std::invalid_argument("Invalid " + std::string(component) + " in time string: " + hhmmss_string);
    }
    size_t consumed = 0;
    double value = 0.0;
    try {
      value = std::stod(token, &consumed);
    } catch (const std::exception&) {
      throw std::invalid_argument("Invalid " + std::string(component) + " in time string: " + hhmmss_string);
    }
    if (consumed != token.size() || !std::isfinite(value) || value < 0.0) {
      throw std::invalid_argument("Invalid " + std::string(component) + " in time string: " + hhmmss_string);
    }
    return value;
  };
  auto require_whole = [&hhmmss_string](double value, const char* component) {
    if (std::floor(value) != value) {
      throw std::invalid_argument("Fractional " + std::string(component) + " in time string: " + hhmmss_string);
    }
  };

  long double seconds = 0.0;
  if (tokens.size() == 3) {
    // Format: HH:MM:SS.ssss
    const double hours = parse_component(tokens[0], "hours");
    const double minutes = parse_component(tokens[1], "minutes");
    const double secs = parse_component(tokens[2], "seconds");
    require_whole(hours, "hours");
    require_whole(minutes, "minutes");
    if (minutes >= 60.0 || secs >= 60.0) {
      throw std::invalid_argument("Minutes and seconds must be below 60 in time string: " + hhmmss_string);
    }
    seconds = hours * 3600.0 + minutes * 60.0 + secs;
  } else if (tokens.size() == 2) {
    // Format: MM:SS.ssss
    const double minutes = parse_component(tokens[0], "minutes");
    const double secs = parse_component(tokens[1], "seconds");
    require_whole(minutes, "minutes");
    if (secs >= 60.0) {
      throw std::invalid_argument("Seconds must be below 60 in time string: " + hhmmss_string);
    }
    seconds = minutes * 60.0 + secs;
  } else if (tokens.size() == 1) {
    // Format: SS.ssss (or just seconds)
    seconds = parse_component(tokens[0], "seconds");
  } else {
    throw std::invalid_argument("Invalid time string format: " + hhmmss_string);
  }

  // Convert seconds to nanoseconds.
  constexpr long double kNanosecondsPerSecond = 1000000000.0L;
  const long double nanoseconds = seconds * kNanosecondsPerSecond;
  if (!std::isfinite(nanoseconds) || nanoseconds > std::numeric_limits<uint64_t>::max()) {
    throw std::out_of_range("Time string is too large: " + hhmmss_string);
  }
  return static_cast<uint64_t>(nanoseconds);
}

namespace utils {

// size_t
size_t getenv(const std::string& name, const size_t& default_value) {
  const char* value = std::getenv(name.c_str());
  if (value) {
    try {
      return std::stoul(value);
    } catch (const std::invalid_argument&) {
      return default_value;
    } catch (const std::out_of_range&) {
      return default_value;
    }
  }
  return default_value;
}

// long
long getenv(const std::string& name, const long& default_value) {
  const char* value = std::getenv(name.c_str());
  if (value) {
    try {
      return std::stol(value);
    } catch (const std::invalid_argument&) {
      return default_value;
    } catch (const std::out_of_range&) {
      return default_value;
    }
  }
  return default_value;
}

// int
int getenv(const std::string& name, const int& default_value) {
  const char* value = std::getenv(name.c_str());
  if (value) {
    try {
      return std::stoi(value);
    } catch (const std::invalid_argument&) {
      return default_value;
    } catch (const std::out_of_range&) {
      return default_value;
    }
  }
  return default_value;
}

// double
double getenv(const std::string& name, const double& default_value) {
  const char* value = std::getenv(name.c_str());
  if (value) {
    try {
      return std::stod(value);
    } catch (const std::invalid_argument&) {
      return default_value;
    } catch (const std::out_of_range&) {
      return default_value;
    }
  }
  return default_value;
}

// std::string
std::string getenv(const std::string& name, const std::string& default_value) {
  const char* value = std::getenv(name.c_str());
  return value ? std::string(value) : default_value;
}

// bool (accepts "1","true","yes","on","y" → true; "0","false","no","off","n" → false)
bool getenv(const std::string& name, const bool& default_value) {
  const char* raw = std::getenv(name.c_str());
  if (!raw) {
    return default_value;
  }
  std::string v(raw);
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
  static const std::vector<std::string> true_vals = {"1", "true", "yes", "on", "y"};
  static const std::vector<std::string> false_vals = {"0", "false", "no", "off", "n"};
  if (std::find(true_vals.begin(), true_vals.end(), v) != true_vals.end())
    return true;
  if (std::find(false_vals.begin(), false_vals.end(), v) != false_vals.end())
    return false;
  return default_value;
}
} // namespace utils

} // namespace hm
