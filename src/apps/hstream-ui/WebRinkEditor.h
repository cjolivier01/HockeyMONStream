#pragma once

#include <QtCore/QJsonObject>

namespace hm::web {
// CPU-only shared Qt estimator. The supervisor owns snapshots and pano_trafo.
QJsonObject RinkEditor(const QJsonObject& request);
} // namespace hm::web
