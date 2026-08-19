#pragma once

#include <string_view>

namespace hm {
namespace pipeline {

inline bool is_allowlisted_runtime_property(std::string_view element_name, std::string_view property_name) {
  return (element_name == "hmstitcher0" && property_name == "post-stitch-rotate-degrees") ||
      (element_name == "dsplaytracker0" &&
       (property_name == "runtime-tuning-config-file" || property_name == "fixed-edge-rotation-angle" ||
        property_name == "fixed-edge-rotation-angle-left" || property_name == "fixed-edge-rotation-angle-right" ||
        property_name == "dynamic-acceleration-scaling")) ||
      ((element_name == "playcropper0" || element_name == "playcropper") &&
       (property_name == "fixed-edge-rotation-angle" || property_name == "fixed-edge-rotation-angle-left" ||
        property_name == "fixed-edge-rotation-angle-right" || property_name == "shadow-lift"));
}

} // namespace pipeline
} // namespace hm
