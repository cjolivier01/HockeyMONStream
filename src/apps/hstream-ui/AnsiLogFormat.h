#pragma once

#include <QtCore/QString>

namespace hm::ui {

QString log_message_color(const QString& text, bool stderr_output, bool dark_background);
QString stage_log_line_html(const QString& line, bool dark_background);

// Render terminal SGR colors as escaped rich text for desktop logs.
QString ansi_to_html(const QString& text, bool dark_background);

} // namespace hm::ui
