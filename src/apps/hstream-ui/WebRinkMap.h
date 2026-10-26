#pragma once
#include <functional>
#include "src/apps/hstream-ui/WebJobSpec.h"

namespace hm::web {
QJsonObject RunRinkMap(const Request& request, QJsonArray& artifacts, const std::function<bool()>& cancelled);
} // namespace hm::web
