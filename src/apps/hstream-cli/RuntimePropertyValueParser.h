#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace hm {
namespace pipeline {

inline bool parse_runtime_boolean_property_value(
    std::string_view property_name,
    std::string_view raw_value,
    bool* parsed_value) {
  if (!parsed_value) {
    return false;
  }
  while (!raw_value.empty() && std::isspace(static_cast<unsigned char>(raw_value.front()))) {
    raw_value.remove_prefix(1);
  }
  while (!raw_value.empty() && std::isspace(static_cast<unsigned char>(raw_value.back()))) {
    raw_value.remove_suffix(1);
  }
  std::string value(raw_value);
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (value == "1" || value == "true") {
    *parsed_value = true;
    return true;
  }
  if (value == "0" || value == "false") {
    *parsed_value = false;
    return true;
  }
  if (property_name != "shadow-lift-black-point") {
    if (value == "yes" || value == "on") {
      *parsed_value = true;
      return true;
    }
    if (value == "no" || value == "off") {
      *parsed_value = false;
      return true;
    }
  }
  return false;
}

} // namespace pipeline
} // namespace hm
