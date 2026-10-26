#pragma once

#include <yaml-cpp/yaml.h>
#include <functional>

#include "src/apps/hstream-ui/WebJobSpec.h"

namespace hm::web {
// Apply saved browser control edits to a private native snapshot. Hidden
// generated geometry survives unchanged controls until native cleanup owns it.
void ApplySavedCalibrationControlEdits(YAML::Node& config, const QJsonObject& before, const QJsonObject& desired);
QJsonObject RunCalibrationControlsStage(const Request& request, const std::function<bool()>& cancelled);
} // namespace hm::web
