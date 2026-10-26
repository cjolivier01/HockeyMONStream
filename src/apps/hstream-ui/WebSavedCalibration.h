#pragma once
#include <functional>
#include "src/apps/hstream-ui/WebJobSpec.h"
namespace hm::web {
QString ExportSavedCalibrationPreview(
    const Request& request,
    QJsonArray& artifacts,
    const std::function<bool()>& cancelled = {});
QJsonObject ReadSavedCalibrationStill(
    const Request& request,
    QJsonArray& artifacts,
    const std::function<bool()>& cancelled);
QJsonObject RunSavedCalibration(const Request& request, QJsonArray& artifacts, const std::function<bool()>& cancelled);
} // namespace hm::web
