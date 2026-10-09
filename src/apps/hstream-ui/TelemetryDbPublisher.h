#pragma once
#include <optional>
#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"
namespace hm::ui_internal {
// nullopt selects the next available positive version, independently of video encoding.
// An explicit suffix must be "-N" with N >= 1.
TelemetryCsvPublicationResult publish_telemetry_database(
    const QString& source,
    const QString& directory,
    const std::optional<QString>& suffix = std::nullopt);
} // namespace hm::ui_internal
