#pragma once
#include <functional>
#include "src/apps/hstream-ui/WebJobSpec.h"
namespace hm::web {
QString ExportSavedCalibrationPreview(const Request& request, QJsonArray& artifacts);
QJsonObject RunSavedCalibration(const Request& request, QJsonArray& artifacts, const std::function<bool()>& cancelled);
} // namespace hm::web
