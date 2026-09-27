#pragma once

#include <QtCore/QString>

namespace hm::ui {

// Render terminal SGR colors as escaped rich text for desktop logs.
QString ansi_to_html(const QString& text, bool dark_background);

} // namespace hm::ui
