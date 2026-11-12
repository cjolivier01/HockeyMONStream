#pragma once
#include <functional>
#include "src/apps/hstream-ui/WebJobSpec.h"
namespace hm::web {
QJsonObject DiagnoseCalibrationFailure(const Request& request, const std::function<bool()>& cancelled);
}
