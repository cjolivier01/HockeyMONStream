#pragma once

#include <functional>

#include "src/apps/hstream-ui/WebJobSpec.h"

namespace hm::web {
QJsonObject RunCalibrationControlsStage(const Request& request, const std::function<bool()>& cancelled);
} // namespace hm::web
