#pragma once

#include <string_view>

namespace hm {
namespace pipeline {

inline bool is_allowlisted_runtime_property(std::string_view element_name, std::string_view property_name) {
  return (element_name == "hmstitcher0" &&
          (property_name == "stitched-output-epoch" || property_name == "shadow-lift" ||
           property_name == "shadow-lift-black-point" || property_name == "exposure")) ||
      (element_name == "dsplaytracker0" &&
       (property_name == "draw" || property_name == "runtime-tuning-config-file" ||
        property_name == "fixed-edge-rotation-angle" || property_name == "fixed-edge-rotation-angle-left" ||
        property_name == "fixed-edge-rotation-angle-right" || property_name == "dynamic-acceleration-scaling")) ||
      ((element_name == "playcropper0" || element_name == "playcropper") &&
       (property_name == "fixed-edge-rotation-angle" || property_name == "fixed-edge-rotation-angle-left" ||
        property_name == "fixed-edge-rotation-angle-right" || property_name == "shadow-lift" ||
        property_name == "shadow-lift-black-point" || property_name == "exposure" ||
        property_name == "scoreboard-perspective-polygon" || property_name == "scoreboard-position" ||
        property_name == "scoreboard-reselect" ||
        // Which analytics layers Program bakes. Drawing only: it selects among
        // results the models already produced, and enabling a layer mid-run
        // starts no model and changes no inference schedule.
        property_name == "player-drawing-layers")) ||
      ((element_name == "program_gpu_preview_sink" || element_name == "stitched_gpu_preview_sink" ||
        element_name == "hmstitcher_preview_sink") &&
       (property_name == "show-player-tracking" || property_name == "show-play-tracking" ||
        property_name == "show-rink-mask" || property_name == "player-drawing-layers"));
}

} // namespace pipeline
} // namespace hm
