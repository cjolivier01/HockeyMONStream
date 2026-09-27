#include "src/apps/hstream-ui/AnsiLogFormat.h"

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

} // namespace

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
