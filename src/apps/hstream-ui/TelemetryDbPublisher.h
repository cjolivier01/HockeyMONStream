#pragma once
#include <optional>
#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"
namespace hm::ui_internal {
// A null suffix selects the first unused generation, independently of video encoding.
TelemetryCsvPublicationResult publish_telemetry_database(
    const QString& source,
    const QString& directory,
    const std::optional<QString>& suffix = std::nullopt);
} // namespace hm::ui_internal
