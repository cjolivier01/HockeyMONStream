#pragma once

#include <QtCore/QJsonObject>
#include <functional>

namespace hm::web {
// One-shot CPU still decoding. The caller owns child cancellation and run cleanup.
QJsonObject ScoreboardPreview(const QJsonObject& request, const std::function<void()>& before_publication = {});
} // namespace hm::web
