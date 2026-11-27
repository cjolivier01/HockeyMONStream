#pragma once

#include <QtCore/QString>

namespace hm::ui {

struct AnsiTextStyle {
  // Empty means "whatever the log widget's palette uses", so log text stays
  // readable on a light desktop theme as well as a dark one.
  QString foreground;
  bool bold = false;
  bool dim = false;
};

QString log_message_color(const QString& text, bool stderr_output, bool dark_background);
QString stage_log_line_html(const QString& line, bool dark_background, AnsiTextStyle& style, bool visible);

// Render terminal SGR colors as escaped rich text for desktop logs.
QString ansi_to_html(const QString& text, bool dark_background);

} // namespace hm::ui
