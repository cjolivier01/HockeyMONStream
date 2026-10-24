#pragma once

#include "hstream/src/libs/common/ConfigLayers.h"

namespace hm::program {
// The initial CLI owns the saved-calibration video Program graph. Reject known
// requests for unconnected stages rather than returning a misleading successful
// render/export. Remove each restriction when that feature acquires an owner.
absl::Status ValidateAmdProgramAdmission(const config::ProgramConfiguration& config);
} // namespace hm::program
