#include "src/apps/hstream-ui/AnsiLogFormat.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

namespace hm::ui {
namespace {

struct AnsiTextStyle {
  // Empty means "whatever the log widget's palette uses", so log text stays
  // readable on a light desktop theme as well as a dark one.
  QString foreground;
  bool bold = false;
  bool dim = false;
};

// The runtime log paints on the desktop theme's own text background, so the
// ANSI palette has to come in two contrast variants.
QString ansi_color(int code, bool dark_background) {
  switch (code) {
    case 30:
      return dark_background ? "#4c566a" : "#4b5563";
    case 31:
      return dark_background ? "#bf616a" : "#b42318";
    case 32:
      return dark_background ? "#a3be8c" : "#256029";
    case 33:
      return dark_background ? "#ebcb8b" : "#8a6100";
    case 34:
      return dark_background ? "#81a1c1" : "#1d4ed8";
    case 35:
      return dark_background ? "#b48ead" : "#7e22ce";
    case 36:
      return dark_background ? "#88c0d0" : "#0e7490";
    case 37:
      return dark_background ? "#e5e9f0" : "#374151";
    case 90:
      return dark_background ? "#667085" : "#6b7280";
    case 91:
      return dark_background ? "#ff7b72" : "#c2410c";
    case 92:
      return dark_background ? "#7ee787" : "#15803d";
    case 93:
      return dark_background ? "#f2cc60" : "#a16207";
    case 94:
      return dark_background ? "#79c0ff" : "#2563eb";
    case 95:
      return dark_background ? "#d2a8ff" : "#9333ea";
    case 96:
      return dark_background ? "#a5d6ff" : "#0891b2";
    case 97:
      return dark_background ? "#ffffff" : "#111827";
    default:
      return {};
  }
}

void apply_ansi_codes(const QString& codes, AnsiTextStyle* style, bool dark_background) {
  const QStringList parts = codes.isEmpty() ? QStringList{"0"} : codes.split(';');
  for (int i = 0; i < parts.size(); ++i) {
    bool ok = false;
    const int code = parts[i].isEmpty() ? 0 : parts[i].toInt(&ok);
    if (!ok) {
      continue;
    }
    if (code == 0) {
      *style = {};
    } else if (code == 1) {
      style->bold = true;
      style->dim = false;
    } else if (code == 2) {
      style->dim = true;
      style->bold = false;
    } else if (code == 22) {
      style->bold = false;
      style->dim = false;
    } else if (code == 39) {
      style->foreground.clear();
    } else if (const QString color = ansi_color(code, dark_background); !color.isEmpty()) {
      style->foreground = color;
    } else if (code == 38 && i + 2 < parts.size() && parts[i + 1] == "5") {
      const int color_index = parts[i + 2].toInt(&ok);
      if (ok && color_index >= 0 && color_index <= 255) {
        style->foreground = QString("hsl(%1, 65%, %2%)").arg((color_index * 47) % 360).arg(dark_background ? 70 : 35);
      }
      i += 2;
    } else if (code == 38 && i + 4 < parts.size() && parts[i + 1] == "2") {
      const int red = parts[i + 2].toInt(&ok);
      const bool red_ok = ok;
      const int green = parts[i + 3].toInt(&ok);
      const bool green_ok = ok;
      const int blue = parts[i + 4].toInt(&ok);
      if (red_ok && green_ok && ok && red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 &&
          blue <= 255) {
        style->foreground = QString("#%1%2%3")
                                .arg(red, 2, 16, QLatin1Char('0'))
                                .arg(green, 2, 16, QLatin1Char('0'))
                                .arg(blue, 2, 16, QLatin1Char('0'));
      }
      i += 4;
    }
  }
}

QString style_span_open(const AnsiTextStyle& style) {
  QStringList declarations;
  if (!style.foreground.isEmpty()) {
    declarations << QString("color:%1").arg(style.foreground);
  }
  if (style.bold) {
    declarations << "font-weight:600";
  }
  if (style.dim) {
    declarations << "opacity:0.72";
  }
  if (declarations.isEmpty()) {
    return "<span>";
  }
  return QString("<span style=\"%1\">").arg(declarations.join(';'));
}

// How a runtime log entry is presented. Runner output is classified from its
// text; the UI's own entries arrive already classified.
enum class LogSeverity {
  kInfo,
  kDetail,
  kSuccess,
  kWarning,
  kError,
};

QString severity_color(LogSeverity severity, bool dark_background) {
  switch (severity) {
    case LogSeverity::kDetail:
      return dark_background ? "#98a2b3" : "#5b6472";
    case LogSeverity::kSuccess:
      return dark_background ? "#7ee787" : "#15803d";
    case LogSeverity::kWarning:
      return dark_background ? "#f2cc60" : "#a16207";
    case LogSeverity::kError:
      return dark_background ? "#ff7b72" : "#b42318";
    case LogSeverity::kInfo:
      break;
  }
  // Ordinary entries stay in the palette's own text colour.
  return {};
}

LogSeverity classify_log_message(const QString& message, bool stderr_output) {
  static const QRegularExpression error_pattern(
      "\\b(?:ERROR|FATAL|ABORTED|CANCELLED|DATA_LOSS|DEADLINE_EXCEEDED|FAILED_PRECONDITION|INTERNAL|INVALID_ARGUMENT|"
      "NOT_FOUND|OUT_OF_RANGE|PERMISSION_DENIED|RESOURCE_EXHAUSTED|UNAUTHENTICATED|UNAVAILABLE|UNIMPLEMENTED)\\b|"
      "\\b(?:failed|failure)\\b|status=error",
      QRegularExpression::CaseInsensitiveOption);
  // "could not"/"unable to" also introduce recoverable notices such as a tool
  // skipping optional metadata, so they stay a tier below outright failure.
  static const QRegularExpression warning_pattern(
      "\\b(?:WARN|WARNING|CRITICAL|deprecated|stale|skipping|retrying|unsupported|not supported|cannot|could not|"
      "unable to)\\b",
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression success_pattern(
      "\\b(?:succeeded|successfully)\\b|status=complete|\\bis complete\\b", QRegularExpression::CaseInsensitiveOption);
  if (error_pattern.match(message).hasMatch())
    return LogSeverity::kError;
  if (warning_pattern.match(message).hasMatch())
    return LogSeverity::kWarning;
  if (success_pattern.match(message).hasMatch())
    return LogSeverity::kSuccess;
  // Explicitly identified stderr can use the quieter detail style. The main
  // pipeline merges its descriptors for ordered diagnostics, so its unmarked
  // lines have no recoverable stream origin and retain the ordinary style.
  return stderr_output ? LogSeverity::kDetail : LogSeverity::kInfo;
}

} // namespace

QString log_message_color(const QString& text, bool stderr_output, bool dark_background) {
  return severity_color(classify_log_message(text, stderr_output), dark_background);
}

QString stage_log_line_html(const QString& line, bool dark_background) {
  const bool stderr_output = line.startsWith("[stderr] ");
  const QString message = stderr_output ? line.mid(9) : line;
  const auto severity = classify_log_message(message, stderr_output);
  QString color = severity_color(severity, dark_background);
  if ((severity == LogSeverity::kInfo || severity == LogSeverity::kDetail) && message.trimmed().startsWith("**PERF:"))
    color = ansi_color(36, dark_background);
  const QString body = ansi_to_html(line, dark_background);
  return "<span style=\"white-space:pre-wrap" + (color.isEmpty() ? QString() : ";color:" + color) + "\">" + body +
      "</span>";
}

QString ansi_to_html(const QString& text, bool dark_background) {
  QString html;
  AnsiTextStyle style;
  bool span_open = false;
  auto open_span = [&]() {
    if (!span_open) {
      html += style_span_open(style);
      span_open = true;
    }
  };
  auto close_span = [&]() {
    if (span_open) {
      html += "</span>";
      span_open = false;
    }
  };

  for (qsizetype i = 0; i < text.size();) {
    if (text[i] == QChar(0x1b) && i + 1 < text.size() && text[i + 1] == '[') {
      qsizetype end = i + 2;
      while (end < text.size() && !text[end].isLetter()) {
        ++end;
      }
      if (end < text.size()) {
        if (text[end] == 'm') {
          close_span();
          apply_ansi_codes(text.mid(i + 2, end - i - 2), &style, dark_background);
        }
        i = end + 1;
        continue;
      }
    }

    open_span();
    html += QString(text[i]).toHtmlEscaped();
    ++i;
  }
  close_span();
  return html;
}

} // namespace hm::ui
