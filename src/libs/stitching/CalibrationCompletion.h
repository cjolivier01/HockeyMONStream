#pragma once

#include <sstream>
#include <string>

namespace hm::stitching {

// Binds a completion event to the exact stitched pixels, calibration owner,
// and pipeline generation that produced it.
inline std::string calibration_completion_scope(
    const std::string& output_generation,
    const std::string& invalidation_id,
    const std::string& run_generation) {
  std::ostringstream scope;
  scope << output_generation.size() << ':' << output_generation << invalidation_id.size() << ':' << invalidation_id
        << run_generation.size() << ':' << run_generation;
  return scope.str();
}

} // namespace hm::stitching
