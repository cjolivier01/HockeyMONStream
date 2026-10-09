#pragma once
#include <QtCore/QString>

namespace hm::ui {
// Finalizes only our completed qtmux output, before publication. Retains AAC
// preroll for decoder state, and removes priming/padding from the audible edit.
bool FinalizeHighlightMp4Audio(const QString& path, qint64 duration_ms, QString* error);
} // namespace hm::ui
