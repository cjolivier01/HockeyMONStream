#pragma once
#include <QtWidgets/QWidget>
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"
namespace hm::ui {
bool EditHighlightItem(
    HighlightInterval* item,
    const QString& game_dir,
    const QString& game_id,
    const QString& archive,
    const QString& route,
    qint64 archive_offset,
    const ArchiveMediaInfo& media,
    QWidget* parent);
}
