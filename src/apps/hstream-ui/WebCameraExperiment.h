#pragma once

#include "src/apps/hstream-ui/WebJobSpec.h"

#include <functional>

namespace hm::web {
// One supervised camera dialog. Inputs are immutable worker-staged references;
// runtime commands carry the run/session/request identity and bounded values.
QJsonObject RunCameraExperiment(
    const Request& request,
    QJsonArray& artifacts,
    unsigned preview_port,
    const std::function<bool()>& interrupted);
} // namespace hm::web
