#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/PipelineInspectorWidget.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QEvent>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/Qt>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEngine>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QStandardItemModel>
#include <QtGui/QTextDocument>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QToolButton>

#include <yaml-cpp/yaml.h>

#include <limits>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <QtCore/QUuid>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kFixedEdgeRotationMaximumX10 = 900;
constexpr int kDefaultStitchCalibrationControlPoints = 1500;
constexpr int kDefaultStitchCalibrationFrameCount = 4;
constexpr char kZeroStitchFrameTime[] = "00:00:00";
constexpr char kStitchFrameTimeFormat[] = "HH:mm:ss";
constexpr char kStitchFrameTimeFractionalFormat[] = "HH:mm:ss.zzz";
constexpr int kRuntimeControlAckTimeoutMs = 3000;
constexpr qsizetype kMaxCapturedLogCharacters = 16 * 1024 * 1024;
constexpr char kStitchedPreviewPipelineOptions[] =
    "pipeline.streammux.batch-size=2,pipeline.streammux.sync-inputs=0,"
    "pipeline.streammux.batched-push-timeout=2147483647,pipeline.streammux.frame-num-reset-on-stream-reset=0,"
    "pipeline.streammux.frame-num-reset-on-eos=0,pipeline.hmstitcher.show=0";

struct CalibrationStageSpec {
  const char* id;
  const char* label;
};

constexpr CalibrationStageSpec kCalibrationStages[] = {
    {"input", "Wait for synchronized camera frames"},
    {"orientation", "Find the ice rink and orient cameras"},
    {"features", "Look for control points"},
    {"matching", "Match control points"},
    {"optimizer", "Run panorama optimizer (autooptimiser)"},
    {"canvas", "Build stitch maps and panorama"},
    {"rink-mask", "Find the ice surface"},
};

std::optional<QTime> parse_stitch_frame_time(const QString& value) {
  QTime parsed = QTime::fromString(value, kStitchFrameTimeFormat);
  if (!parsed.isValid()) {
    parsed = QTime::fromString(value, kStitchFrameTimeFractionalFormat);
  }
  return parsed.isValid() ? std::optional<QTime>(parsed) : std::nullopt;
}

QString format_stitch_frame_time(const QTime& value) {
  return value.msec() == 0 ? value.toString(kStitchFrameTimeFormat) : value.toString(kStitchFrameTimeFractionalFormat);
}

QString normalize_backend_choice(QString value, const QString& fallback) {
  value = value.trimmed().toLower().replace('_', '-');
  return value.isEmpty() ? fallback : value;
}

std::optional<QString> canonical_control_point_matcher_choice(QString value) {
  value = value.trimmed().toLower().replace('_', '-');
  if (value.isEmpty() || value == "aliked-lightglue" || value == "raco-aliked-lightglue" ||
      value == "native-aliked-lightglue" || value == "superpoint-lightglue" || value == "superpoint" ||
      value == "lightglue") {
    return QString("superpoint-lightglue");
  }
  return std::nullopt;
}

std::optional<QString> canonical_mapping_backend_choice(QString value) {
  value = value.trimmed().toLower().replace('_', '-');
  if (value.isEmpty() || value == "nona") {
    return QString("nona");
  }
  if (value == "opencv-magsac" || value == "magsac" || value == "magsac++") {
    return QString("opencv-magsac");
  }
  if (value == "opencv-affine-ransac" || value == "affine-ransac" || value == "ransac") {
    return QString("opencv-affine-ransac");
  }
  return std::nullopt;
}

QString canonical_or_normalized_matcher_choice(QString value, const QString& fallback) {
  const QString normalized = normalize_backend_choice(value, fallback);
  return canonical_control_point_matcher_choice(normalized).value_or(normalized);
}

QString canonical_or_normalized_mapping_choice(QString value, const QString& fallback) {
  const QString normalized = normalize_backend_choice(value, fallback);
  return canonical_mapping_backend_choice(normalized).value_or(normalized);
}

bool set_combo_to_data(QComboBox* combo, const QString& value) {
  if (!combo)
    return false;
  const int index = combo->findData(value);
  if (index < 0)
    return false;
  if (combo->model() && !(combo->model()->flags(combo->model()->index(index, 0)) & Qt::ItemIsEnabled))
    return false;
  combo->setCurrentIndex(index);
  return true;
}

std::optional<size_t> calibration_stage_index(const QString& stage) {
  for (size_t index = 0; index < std::size(kCalibrationStages); ++index) {
    if (stage == QString::fromLatin1(kCalibrationStages[index].id))
      return index;
  }
  return std::nullopt;
}

absl::Status publish_yaml_config(const fs::path& config_path, const YAML::Node& config) {
  std::string contents;
  if (config.IsDefined() && !config.IsNull())
    contents = YAML::Dump(config) + "\n";
  return hm::stitching::publish_game_config(config_path.parent_path(), contents);
}

struct CameraSliderSpec {
  const char* id;
  const char* label;
  int minimum;
  int maximum;
  int default_value;
};

struct AnsiTextStyle {
  QString foreground = "#d8dee9";
  bool bold = false;
  bool dim = false;
};

QString timestamp() {
  return QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
}

QString format_video_time_ns(qint64 nanoseconds) {
  qint64 total_seconds = std::max<qint64>(0, nanoseconds) / 1000000000LL;
  const qint64 hours = total_seconds / 3600;
  total_seconds %= 3600;
  const qint64 minutes = total_seconds / 60;
  const qint64 seconds = total_seconds % 60;
  return QString("%1:%2:%3")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'));
}

QLabel* make_value_label(const QString& object_name, const QString& value) {
  auto* label = new QLabel(value);
  label->setObjectName(object_name);
  label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  label->setMinimumWidth(92);
  return label;
}

void set_control_help(QWidget* control, const QString& description) {
  if (!control)
    return;
  control->setToolTip(description);
  control->setStatusTip(description);
  control->setWhatsThis(description);
  control->setAccessibleDescription(description);
}

class WheelPassthroughSlider : public QSlider {
 public:
  explicit WheelPassthroughSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
      : QSlider(orientation, parent) {}

 protected:
  void wheelEvent(QWheelEvent* event) override {
    event->ignore();
  }
};

class StitchingCalibrationDialog : public QDialog {
 public:
  explicit StitchingCalibrationDialog(QWidget* parent = nullptr) : QDialog(parent) {}

  void setCloseAllowed(bool allowed) {
    close_allowed_ = allowed;
  }

  void reject() override {
    if (close_allowed_)
      QDialog::reject();
  }

 protected:
  void closeEvent(QCloseEvent* event) override {
    if (!close_allowed_) {
      event->ignore();
      return;
    }
    QDialog::closeEvent(event);
  }

 private:
  bool close_allowed_{false};
};

class NativeVideoTarget : public QWidget {
 public:
  explicit NativeVideoTarget(QWidget* parent = nullptr) : QWidget(parent) {
    // X11 video-overlay sinks paint this native child directly.  Keep Qt's
    // backing store from repainting the child black after the video sink has
    // presented a frame.  WA_PaintOnScreen requires paintEngine() to return
    // null; doing both avoids the QWidget::paintEngine warning while leaving
    // ownership of every pixel with the external renderer.
    if (QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0) {
      setAttribute(Qt::WA_NativeWindow);
      setAttribute(Qt::WA_PaintOnScreen);
      setAttribute(Qt::WA_NoSystemBackground);
    }
    setAutoFillBackground(false);
  }

  QPaintEngine* paintEngine() const override {
    return nullptr;
  }

  void setFocusToggleCallback(std::function<void()> callback) {
    focus_toggle_callback_ = std::move(callback);
  }

  void setFocusButton(QPushButton* button) {
    focus_button_ = button;
  }

  void setFocusAvailable(bool available) {
    focus_available_ = available;
  }

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_available_ && focus_toggle_callback_) {
      focus_toggle_callback_();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_available_ && focus_button_ && focus_button_->isEnabled() &&
        focus_button_->isVisible()) {
      const QPoint button_position = focus_button_->mapFromGlobal(event->globalPosition().toPoint());
      if (focus_button_->rect().contains(button_position)) {
        focus_button_->click();
        event->accept();
        return;
      }
    }
    QWidget::mouseReleaseEvent(event);
  }

 private:
  std::function<void()> focus_toggle_callback_;
  QPushButton* focus_button_{nullptr};
  bool focus_available_{false};
};

QIcon preview_focus_icon(bool focused) {
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
  const int outer = focused ? 3 : 2;
  const int inner = focused ? 6 : 5;
  const int far = focused ? 12 : 13;
  const int arm = inner - outer;
  painter.drawLine(outer, inner, outer, outer);
  painter.drawLine(outer, outer, inner, outer);
  painter.drawLine(far, inner, far, outer);
  painter.drawLine(far, outer, far - arm, outer);
  painter.drawLine(outer, far - arm, outer, far);
  painter.drawLine(outer, far, inner, far);
  painter.drawLine(far, far - arm, far, far);
  painter.drawLine(far, far, far - arm, far);
  return QIcon(pixmap);
}

QIcon hstream_application_icon() {
  static const QIcon icon = [] {
    QIcon result;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
      QPixmap pixmap(size, size);
      pixmap.fill(Qt::transparent);
      QPainter painter(&pixmap);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.scale(size / 512.0, size / 512.0);

      QLinearGradient background(64, 42, 448, 470);
      background.setColorAt(0.0, QColor("#071523"));
      background.setColorAt(0.55, QColor("#0a3552"));
      background.setColorAt(1.0, QColor("#08718a"));
      painter.setPen(Qt::NoPen);
      painter.setBrush(background);
      painter.drawRoundedRect(QRectF(18, 18, 476, 476), 108, 108);

      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor(66, 225, 239, 145), 17, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawRoundedRect(QRectF(72, 146, 368, 220), 108, 108);

      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor("#f7fbff"));
      painter.drawRoundedRect(QRectF(124, 112, 76, 288), 27, 27);
      painter.drawRoundedRect(QRectF(312, 112, 76, 288), 27, 27);
      painter.drawRoundedRect(QRectF(174, 218, 164, 76), 27, 27);

      QPainterPath stream;
      stream.moveTo(76, 304);
      stream.cubicTo(159, 246, 217, 329, 304, 265);
      stream.cubicTo(350, 231, 397, 215, 440, 228);
      painter.setPen(QPen(QColor("#00cedf"), 39, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stream);
      painter.setPen(QPen(QColor("#8ff8ff"), 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stream);

      painter.setPen(QPen(QColor("#f7fbff"), 10));
      painter.setBrush(QColor("#ff4f64"));
      painter.drawEllipse(QPointF(399, 105), 31, 31);
      painter.end();
      result.addPixmap(pixmap);
    }
    return result;
  }();
  return icon;
}

class LetterboxRenderHost : public QWidget {
 public:
  explicit LetterboxRenderHost(double aspect_ratio, QWidget* parent = nullptr)
      : QWidget(parent), aspect_ratio_(aspect_ratio > 0.0 ? aspect_ratio : 16.0 / 9.0) {
    setObjectName("letterboxRenderHost");
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setAutoFillBackground(true);
    render_surface_ = new QWidget(this);
    render_surface_->setAutoFillBackground(true);
    render_surface_->setPalette(pal);
    render_target_ = new NativeVideoTarget(render_surface_);
    // Keep the native overlay unmapped until a pipeline explicitly enables
    // embedded rendering. A mapped, unpainted X11 child is an opaque black
    // window and can obscure Qt siblings while the initial layout settles.
    render_target_->hide();
    focus_button_ = new QPushButton(this);
    focus_button_->setFixedSize(24, 24);
    focus_button_->setIconSize(QSize(14, 14));
    set_control_help(
        focus_button_, "Expand this video preview to fill the application while keeping video on the GPU.");
    focus_button_->setAccessibleName("Focus video");
    focus_button_->setIcon(preview_focus_icon(false));
    focus_button_->setStyleSheet(
        "QPushButton { background: rgba(15, 23, 42, 210); border: 1px solid rgba(255, 255, 255, 100); "
        "border-radius: 3px; color: white; padding: 0; }"
        "QPushButton:hover { background: rgba(30, 64, 175, 235); }");
    if (QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0) {
      // The video-overlay sink owns a native child window. Make the control a
      // native sibling too so the window system can stack it visibly above
      // the sink instead of hiding Qt backing-store pixels behind the video.
      focus_button_->setAttribute(Qt::WA_NativeWindow);
      focus_button_->winId();
    }
    render_target_->setFocusButton(focus_button_);
    setFocusAvailable(false);
    connect(focus_button_, &QPushButton::clicked, this, [this]() {
      if (focus_toggle_callback_)
        focus_toggle_callback_();
    });
  }

  QWidget* renderSurface() const {
    return render_surface_;
  }

  QWidget* renderTarget() const {
    return render_target_;
  }

  QPushButton* focusButton() const {
    return focus_button_;
  }

  void setFocusToggleCallback(std::function<void()> callback) {
    focus_toggle_callback_ = std::move(callback);
    render_target_->setFocusToggleCallback(focus_toggle_callback_);
  }

  void setFocusAvailable(bool available) {
    focus_available_ = available;
    render_target_->setFocusAvailable(available);
    focus_button_->setEnabled(available);
    focus_button_->setVisible(available);
    if (available)
      focus_button_->raise();
  }

  bool focusAvailable() const {
    return focus_available_;
  }

  void setFocused(bool focused) {
    focus_button_->setIcon(preview_focus_icon(focused));
    focus_button_->setAccessibleName(focused ? "Restore HStream controls" : "Focus video");
    set_control_help(
        focus_button_,
        focused ? "Restore the normal HStream layout and controls."
                : "Expand this video preview to fill the application while keeping video on the GPU.");
    focus_button_->raise();
  }

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_available_ && focus_toggle_callback_) {
      focus_toggle_callback_();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    if (!render_surface_) {
      return;
    }
    const QSize available = size();
    if (available.width() <= 0 || available.height() <= 0) {
      return;
    }

    int width = available.width();
    int height = static_cast<int>(std::round(width / aspect_ratio_));
    if (height > available.height()) {
      height = available.height();
      width = static_cast<int>(std::round(height * aspect_ratio_));
    }
    const int x = (available.width() - width) / 2;
    const int y = (available.height() - height) / 2;
    render_surface_->setGeometry(x, y, width, height);
    render_target_->setGeometry(0, 0, width, height);
    constexpr int kButtonMargin = 6;
    focus_button_->move(available.width() - focus_button_->width() - kButtonMargin, kButtonMargin);
    focus_button_->raise();
  }

 private:
  double aspect_ratio_;
  QWidget* render_surface_{nullptr};
  NativeVideoTarget* render_target_{nullptr};
  QPushButton* focus_button_{nullptr};
  std::function<void()> focus_toggle_callback_;
  bool focus_available_{false};
};

QString ansi_color(int code) {
  switch (code) {
    case 30:
      return "#4c566a";
    case 31:
      return "#bf616a";
    case 32:
      return "#a3be8c";
    case 33:
      return "#ebcb8b";
    case 34:
      return "#81a1c1";
    case 35:
      return "#b48ead";
    case 36:
      return "#88c0d0";
    case 37:
      return "#e5e9f0";
    case 90:
      return "#667085";
    case 91:
      return "#ff7b72";
    case 92:
      return "#7ee787";
    case 93:
      return "#f2cc60";
    case 94:
      return "#79c0ff";
    case 95:
      return "#d2a8ff";
    case 96:
      return "#a5d6ff";
    case 97:
      return "#ffffff";
    default:
      return {};
  }
}

void apply_ansi_codes(const QString& codes, AnsiTextStyle* style) {
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
      style->foreground = "#d8dee9";
    } else if (const QString color = ansi_color(code); !color.isEmpty()) {
      style->foreground = color;
    } else if (code == 38 && i + 2 < parts.size() && parts[i + 1] == "5") {
      const int color_index = parts[i + 2].toInt(&ok);
      if (ok && color_index >= 0 && color_index <= 255) {
        style->foreground = QString("hsl(%1, 65%, 70%)").arg((color_index * 47) % 360);
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
  declarations << QString("color:%1").arg(style.foreground);
  if (style.bold) {
    declarations << "font-weight:600";
  }
  if (style.dim) {
    declarations << "opacity:0.72";
  }
  return QString("<span style=\"%1\">").arg(declarations.join(';'));
}

QString ansi_to_html(const QString& text) {
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
          apply_ansi_codes(text.mid(i + 2, end - i - 2), &style);
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

bool is_video_file(const QString& path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == "mp4" || suffix == "mkv" || suffix == "m4v" || suffix == "mov" || suffix == "avi";
}

bool is_auto_chapter_file(const QString& file_name) {
  static const QRegularExpression gopro("^G[A-Z][0-9]{6}\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\\.(MP4|mp4)$");
  static const QRegularExpression left_right("(left|right)(-[0-9])?\\.mp4$");
  return gopro.match(file_name).hasMatch() || insta360.match(file_name).hasMatch() ||
      left_right.match(file_name).hasMatch();
}

bool is_root_auto_file(const QString& file_name) {
  return is_auto_chapter_file(file_name);
}

QString auto_file_family(const QString& file_name) {
  static const QRegularExpression gopro("^G[A-Z][0-9]{6}\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\\.(MP4|mp4)$");
  static const QRegularExpression left_right("(left|right)(-[0-9])?\\.mp4$");
  const QRegularExpressionMatch gopro_match = gopro.match(file_name);
  if (gopro_match.hasMatch()) {
    return "gopro-" + file_name.mid(4, 4);
  }
  const QRegularExpressionMatch insta360_match = insta360.match(file_name);
  if (insta360_match.hasMatch()) {
    return "insta360-" + file_name.mid(4, 15);
  }
  const QRegularExpressionMatch lr_match = left_right.match(file_name);
  if (lr_match.hasMatch()) {
    return "lr-" + lr_match.captured(1).toLower();
  }
  return {};
}

std::optional<QString> explicit_chapter_key(const QString& path) {
  const QString file_name = QFileInfo(path).fileName();
  static const QRegularExpression gopro("^G[A-Z]([0-9]{2})([0-9]{4})\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_([0-9]{8})_([0-9]{6})_([0-9]{3})\\.(MP4|mp4)$");
  static const QRegularExpression left_right(
      "(left|right)(?:-([0-9]+))?\\.(mp4|mkv|m4v)$", QRegularExpression::CaseInsensitiveOption);

  const QRegularExpressionMatch gopro_match = gopro.match(file_name);
  if (gopro_match.hasMatch()) {
    return QString("gopro:%1:%2").arg(gopro_match.captured(2), gopro_match.captured(1));
  }
  const QRegularExpressionMatch insta360_match = insta360.match(file_name);
  if (insta360_match.hasMatch()) {
    return QString("insta360:%1:%2:%3")
        .arg(insta360_match.captured(1), insta360_match.captured(2), insta360_match.captured(3));
  }
  const QRegularExpressionMatch lr_match = left_right.match(file_name);
  if (lr_match.hasMatch()) {
    QString part = lr_match.captured(2).isEmpty() ? "1" : lr_match.captured(2);
    int first_nonzero = 0;
    while (first_nonzero + 1 < part.size() && part[first_nonzero] == '0') {
      ++first_nonzero;
    }
    part = part.mid(first_nonzero);
    return QString("lr:%1:%2").arg(QString::number(part.size()).rightJustified(10, '0'), part);
  }
  return std::nullopt;
}

QString canonical_dir_path(const QString& path) {
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

void prepend_env_path(QProcessEnvironment& env, const QString& name, const QString& dir) {
  if (dir.isEmpty() || !QDir(dir).exists()) {
    return;
  }
  const QString current = env.value(name);
  if (current.isEmpty()) {
    env.insert(name, dir);
    return;
  }
  const QStringList parts = current.split(':', Qt::SkipEmptyParts);
  if (!parts.contains(dir)) {
    env.insert(name, dir + ":" + current);
  }
}

QString bazel_output_configuration(const QString& bazel_bin_path) {
  const QString canonical_path = canonical_dir_path(bazel_bin_path);
  const QFileInfo bin_info(canonical_path);
  if (bin_info.fileName() != "bin")
    return "installed";
  const QDir configuration_dir = bin_info.dir();
  QDir bazel_out_dir = configuration_dir;
  if (!bazel_out_dir.cdUp() || bazel_out_dir.dirName() != "bazel-out")
    return "installed";
  QString output_configuration = configuration_dir.dirName();
  output_configuration.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
  return output_configuration.isEmpty() ? QString("unknown-output") : output_configuration;
}

QString bazel_solib_directory(const QString& architecture) {
  const QString normalized = architecture.toLower();
  if (normalized == "x86_64" || normalized == "amd64")
    return "_solib_k8";
  if (normalized == "aarch64" || normalized == "arm64")
    return "_solib_aarch64";
  return {};
}

QString runtime_architecture_name() {
  const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
  if (architecture == "amd64" || architecture == "x86_64")
    return "x86_64";
  if (architecture == "arm64" || architecture == "aarch64")
    return "aarch64";
  return architecture.isEmpty() ? QString("unknown") : architecture;
}

QString runtime_launch_key() {
  return QString("launch-%1").arg(QCoreApplication::applicationPid());
}

QString writable_runtime_cache_root(const QString& working_dir) {
  QStringList candidates;
  auto add_environment_candidate = [&candidates](const char* name, const QString& suffix = {}) {
    const QString value = qEnvironmentVariable(name);
    if (!value.isEmpty())
      candidates.push_back(suffix.isEmpty() ? value : QDir(value).filePath(suffix));
  };
  add_environment_candidate("HSTREAM_RUNTIME_CACHE_DIR");
  candidates.push_back(QDir(working_dir).filePath(".cache"));
  add_environment_candidate("TEST_TMPDIR", "hstream-runtime-cache");
  add_environment_candidate("XDG_CACHE_HOME", "hstream");
  const QString standard_cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (!standard_cache.isEmpty())
    candidates.push_back(QDir(standard_cache).filePath("runtime"));

  for (const QString& candidate : candidates) {
    if (candidate.isEmpty() || !QDir().mkpath(candidate))
      continue;
    QTemporaryDir probe(QDir(candidate).filePath(".write-probe-XXXXXX"));
    if (probe.isValid())
      return QDir(candidate).absolutePath();
  }

  QTemporaryDir fallback(QDir(QDir::tempPath()).filePath("hstream-runtime-XXXXXX"));
  if (!fallback.isValid() ||
      !QFile::setPermissions(
          fallback.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
    return {};
  }
  fallback.setAutoRemove(false);
  return QDir(fallback.path()).absolutePath();
}

void stage_bazel_gst_plugins(QProcessEnvironment& env, const QString& cache_root, const QString& bazel_bin_path) {
  const QDir bazel_bin(bazel_bin_path);
  const QDir root(bazel_bin.filePath("src/gst-plugins"));
  if (!root.exists()) {
    return;
  }

  const QString arch = runtime_architecture_name();
  const QDir canonical_bazel_bin(canonical_dir_path(bazel_bin.absolutePath()));
  const QString output_configuration = bazel_output_configuration(canonical_bazel_bin.absolutePath());
  QDir runtime_dir(
      QDir(cache_root)
          .filePath(QString("gst-plugin-path/%1/%2/%3").arg(arch, output_configuration, runtime_launch_key())));
  if (!runtime_dir.exists() && !runtime_dir.mkpath(".")) {
    return;
  }
  const QFileInfoList stale_links = runtime_dir.entryInfoList(QStringList("*.so"), QDir::Files | QDir::System);
  for (const QFileInfo& stale : stale_links) {
    if (stale.isSymLink()) {
      QFile::remove(stale.absoluteFilePath());
    }
  }

  QDirIterator plugin_it(
      root.absolutePath(), QStringList({"libnvdsgst_*.so", "libgst*.so"}), QDir::Files, QDirIterator::Subdirectories);
  while (plugin_it.hasNext()) {
    const QFileInfo plugin(plugin_it.next());
    const QString path = plugin.absoluteFilePath();
    if (path.contains("/testutils/") || path.contains(".runfiles/")) {
      continue;
    }
    const QString link_path = runtime_dir.filePath(plugin.fileName());
    QFile::remove(link_path);
    QFile::link(plugin.canonicalFilePath(), link_path);
    prepend_env_path(env, "LD_LIBRARY_PATH", plugin.absolutePath());
  }

  QDirIterator lib_it(root.absolutePath(), QStringList("*.so"), QDir::Files, QDirIterator::Subdirectories);
  while (lib_it.hasNext()) {
    const QFileInfo lib(lib_it.next());
    const QString path = lib.absoluteFilePath();
    if (!path.contains(".runfiles/")) {
      prepend_env_path(env, "LD_LIBRARY_PATH", lib.absolutePath());
    }
  }

  const QDir bazel_bin_dir(canonical_bazel_bin);
  const QString solib_directory = bazel_solib_directory(arch);
  const QFileInfoList solib_roots = bazel_bin_dir.entryInfoList(
      QStringList() << (solib_directory.isEmpty() ? "_solib_*" : solib_directory), QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& solib_root : solib_roots) {
    prepend_env_path(env, "LD_LIBRARY_PATH", solib_root.absoluteFilePath());
    const QDir solib_dir(solib_root.absoluteFilePath());
    const QFileInfoList solib_children = solib_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& solib_child : solib_children) {
      if (!solib_child.fileName().contains("Sstubs")) {
        prepend_env_path(env, "LD_LIBRARY_PATH", solib_child.absoluteFilePath());
      }
    }
  }

  QDir runtime_lib_dir(
      QDir(cache_root)
          .filePath(QString("runtime-lib-path/%1/%2/%3").arg(arch, output_configuration, runtime_launch_key())));
  bool staged_runtime_library = false;
  if (runtime_lib_dir.mkpath(".")) {
    for (const QFileInfo& solib_root : solib_roots) {
      QDirIterator onnxruntime_it(
          solib_root.absoluteFilePath(), QStringList("libonnxruntime.so.1"), QDir::Files, QDirIterator::Subdirectories);
      if (!onnxruntime_it.hasNext())
        continue;
      const QFileInfo onnxruntime(onnxruntime_it.next());
      const QString link_path = runtime_lib_dir.filePath("libonnxruntime.so.1");
      QFile::remove(link_path);
      staged_runtime_library = QFile::link(onnxruntime.canonicalFilePath(), link_path) ||
          QFileInfo(link_path).isFile() || staged_runtime_library;
      break;
    }
    const QFileInfo yolo(bazel_bin.filePath("src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"));
    if (yolo.isFile()) {
      const QString link_path = runtime_lib_dir.filePath("libnvdsinfer_custom_impl_Yolo.so");
      QFile::remove(link_path);
      staged_runtime_library =
          QFile::link(yolo.canonicalFilePath(), link_path) || QFileInfo(link_path).isFile() || staged_runtime_library;
    }
  }
  if (staged_runtime_library)
    prepend_env_path(env, "LD_LIBRARY_PATH", runtime_lib_dir.absolutePath());

  prepend_env_path(env, "GST_PLUGIN_PATH", runtime_dir.absolutePath());
}

bool is_tegra_runtime() {
#ifdef IS_TEGRA
  return true;
#elif defined(Q_OS_LINUX)
  const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
  if (architecture != "arm64" && architecture != "aarch64")
    return false;
  if (QFileInfo::exists("/etc/nv_tegra_release"))
    return true;
  QFile compatible("/proc/device-tree/compatible");
  return compatible.open(QIODevice::ReadOnly) && compatible.readAll().contains("nvidia,tegra");
#else
  return false;
#endif
}

QString configure_pipeline_runtime_environment(
    QProcessEnvironment& env,
    const QString& working_dir,
    const QString& bazel_bin_path) {
  if (!bazel_bin_path.isEmpty()) {
    const QString missing = hm::ui_internal::missing_development_runtime_artifact(bazel_bin_path);
    if (!missing.isEmpty())
      return QString("matching Bazel runtime artifact is missing: %1").arg(missing);
  }
  const QString cache_root = writable_runtime_cache_root(working_dir);
  if (cache_root.isEmpty())
    return "could not find a writable hstream runtime cache directory";
  // Keep the child CLI on this exact private root, including when the UI had
  // to create an unpredictable last-resort temporary directory.
  env.insert("HSTREAM_RUNTIME_CACHE_DIR", cache_root);
  // DeepStream 9.1's legacy nvstreammux rejects the native 8K source caps used
  // by stitching. Match run.sh while preserving an explicit diagnostic
  // override from the caller.
  if (env.value("USE_NEW_NVSTREAMMUX").isEmpty()) {
    env.insert("USE_NEW_NVSTREAMMUX", "yes");
  }
  if (env.value("HM_RENDER_SINK").isEmpty()) {
    env.insert(
        "HM_RENDER_SINK",
        hm::ui_internal::supports_x11_embedding(QGuiApplication::platformName(), is_tegra_runtime()) ? "ximagesink"
                                                                                                     : "nv3dsink");
  }
  QDir registry_dir(QDir(cache_root).filePath("gstreamer-1.0"));
  if (registry_dir.mkpath(".")) {
    const QString arch = runtime_architecture_name();
    const QString selected_bazel_bin =
        bazel_bin_path.isEmpty() ? QDir(working_dir).filePath("bazel-bin") : bazel_bin_path;
    const QString output_configuration = bazel_output_configuration(selected_bazel_bin);
    env.insert(
        "GST_REGISTRY",
        registry_dir.filePath(QString("registry.hstream.native-onnx-v1.%1.%2.bin").arg(arch, output_configuration)));
  }

  prepend_env_path(env, "GST_PLUGIN_PATH", QDir(working_dir).filePath("lib/gst-plugins"));
  prepend_env_path(env, "GST_PLUGIN_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  prepend_env_path(env, "LD_LIBRARY_PATH", QDir(working_dir).filePath("lib"));
  prepend_env_path(env, "LD_LIBRARY_PATH", QDir(working_dir).filePath("lib/gst-plugins"));
  prepend_env_path(env, "LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib");
  prepend_env_path(env, "LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  if (!bazel_bin_path.isEmpty())
    stage_bazel_gst_plugins(env, cache_root, bazel_bin_path);
  return {};
}

QString archive_output_work_dir(const QProcessEnvironment& env, const QString& working_dir) {
  QString root = env.value("HM_OUTPUT_WORK_DIR").trimmed();
  const bool explicit_environment_override = !root.isEmpty();
  if (root.isEmpty()) {
    auto user_overlay = hm::user_config::load_or_create();
    if (user_overlay.ok()) {
      auto configured = hm::user_config::output_root(*user_overlay);
      if (configured.ok())
        root = QString::fromStdString(configured->string());
    }
  }
  if (root.isEmpty())
    root = QDir::home().filePath("hstream_output");
  // The backend interprets an explicitly relative environment override from
  // its working directory. Resolve it the same way here so the path shown and
  // monitored by the UI always names the file the backend is writing.
  if (explicit_environment_override && QFileInfo(root).isRelative())
    root = QDir(working_dir).absoluteFilePath(root);
  return QDir::cleanPath(root);
}

QString archive_output_path(const QString& output_work_dir, const QString& game_id) {
  return QDir(QDir(output_work_dir).filePath(game_id)).filePath("tracking_output-with-audio.mkv");
}

QString available_final_archive_path(const QString& game_dir, const QString& game_id) {
  QString safe_game_id = game_id.trimmed();
  safe_game_id.replace(QRegularExpression(R"([\\/]+)"), "_");
  const QString base = QString("%1-tracking_output-with-audio").arg(safe_game_id);
  for (int suffix = 0; suffix < 1000; ++suffix) {
    const QString filename = suffix == 0 ? base + ".mp4" : QString("%1-%2.mp4").arg(base).arg(suffix);
    const QString candidate = QDir(game_dir).filePath(filename);
#ifdef Q_OS_UNIX
    struct stat candidate_stat{};
    struct stat guard_stat{};
    const QByteArray encoded_candidate = QFile::encodeName(candidate);
    const QByteArray encoded_guard = QFile::encodeName(candidate + ".hstream-pin");
    if (::lstat(encoded_candidate.constData(), &candidate_stat) != 0 && errno == ENOENT &&
        ::lstat(encoded_guard.constData(), &guard_stat) != 0 && errno == ENOENT)
#else
    if (!QFileInfo::exists(candidate))
#endif
      return candidate;
  }
  return {};
}

QString failed_archive_candidate(const QString& source_path, int suffix) {
  const QFileInfo source(source_path);
  const QString extension = source.suffix().isEmpty() ? QString() : "." + source.suffix();
  QString source_base = source.completeBaseName();
  static const QRegularExpression unique_run_suffix(
      R"(\.hstream-run-v3-[0-9]+-[0-9]+-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
  source_base.remove(unique_run_suffix);
  const QString base = source_base + "-finalization-failed";
  const QString filename = suffix == 0 ? base + extension : QString("%1-%2%3").arg(base).arg(suffix).arg(extension);
  return QDir(source.absolutePath()).filePath(filename);
}

#ifdef Q_OS_UNIX
bool sync_open_file(QFile& file, QString* error) {
  if (!file.isOpen() || file.handle() < 0) {
    if (error)
      *error = "file is not open";
    return false;
  }
  if (!file.flush()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  if (::fsync(file.handle()) != 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  return true;
}

bool sync_parent_directory(const QString& path, QString* error) {
  const QByteArray encoded_parent = QFile::encodeName(QFileInfo(path).absolutePath());
  const int parent_fd = ::open(encoded_parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  if (::fsync(parent_fd) != 0) {
    const int saved_errno = errno;
    ::close(parent_fd);
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(saved_errno));
    return false;
  }
  if (::close(parent_fd) != 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  return true;
}

bool same_file_identity(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool path_has_file_identity(const QString& path, const struct stat& expected_stat) {
  struct stat current_stat{};
  const QByteArray encoded_path = QFile::encodeName(path);
  return ::lstat(encoded_path.constData(), &current_stat) == 0 && same_file_identity(current_stat, expected_stat);
}

bool link_open_file_no_replace(int source_fd, const QString& destination, int* saved_errno) {
#ifdef Q_OS_LINUX
  const QByteArray encoded_destination = QFile::encodeName(destination);
  if (::linkat(source_fd, "", AT_FDCWD, encoded_destination.constData(), AT_EMPTY_PATH) == 0)
    return true;
  if (saved_errno)
    *saved_errno = errno;
  return false;
#else
  Q_UNUSED(source_fd);
  Q_UNUSED(destination);
  if (saved_errno)
    *saved_errno = ENOTSUP;
  return false;
#endif
}

bool durably_publish_cleanup_fallback(
    int cleanup_fd,
    const char* cleanup_entry,
    int parent_fd,
    const QByteArray& filename,
    const struct stat& expected_stat,
    QByteArray* retained_name,
    QString* error) {
  const std::array<QByteArray, 2> candidates = {filename, filename + ".hstream-pin"};
  int saved_errno = EEXIST;
  for (const QByteArray& candidate : candidates) {
    bool published = ::linkat(cleanup_fd, cleanup_entry, parent_fd, candidate.constData(), 0) == 0;
    if (!published) {
      saved_errno = errno;
      if (saved_errno == EEXIST) {
        struct stat existing_stat{};
        published = ::fstatat(parent_fd, candidate.constData(), &existing_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
            same_file_identity(existing_stat, expected_stat);
      }
    }
    if (!published)
      continue;
    if (::fsync(parent_fd) != 0) {
      if (error)
        *error =
            QString("could not make retained pathname durable: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
      return false;
    }
    struct stat published_stat{};
    if (::fstatat(parent_fd, candidate.constData(), &published_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_file_identity(published_stat, expected_stat)) {
      if (error)
        *error = "retained pathname changed before it became durable";
      return false;
    }
    if (retained_name)
      *retained_name = candidate;
    return true;
  }
  if (error)
    *error = QString::fromLocal8Bit(std::strerror(saved_errno));
  return false;
}

struct DurableUiRemovalFallback {
  QString path;
  bool retire_on_success = false;
};

bool rename_entry_no_replace(
    int source_directory_fd,
    const char* source_name,
    int destination_directory_fd,
    const char* destination_name,
    int* saved_errno);

bool ensure_durable_ui_removal_fallback(
    int pinned_fd,
    const QString& path,
    const struct stat& expected_stat,
    DurableUiRemovalFallback* fallback,
    QString* error) {
  const QString fallback_path = path + ".hstream-cleanup-pin";
  int link_errno = 0;
  const bool created = link_open_file_no_replace(pinned_fd, fallback_path, &link_errno);
  if (!created && (link_errno != EEXIST || !path_has_file_identity(fallback_path, expected_stat))) {
    if (error) {
      *error = QString("could not establish public cleanup fallback at %1: %2")
                   .arg(fallback_path, QString::fromLocal8Bit(std::strerror(link_errno)));
    }
    return false;
  }
  QString sync_error;
  if (!sync_parent_directory(fallback_path, &sync_error) || !path_has_file_identity(fallback_path, expected_stat)) {
    if (error)
      *error = QString("could not make public cleanup fallback durable at %1: %2").arg(fallback_path, sync_error);
    return false;
  }
  if (fallback) {
    fallback->path = fallback_path;
    fallback->retire_on_success = true;
  }
  return true;
}

bool retire_durable_ui_removal_fallback(
    int cleanup_fd,
    int parent_fd,
    const DurableUiRemovalFallback& fallback,
    const struct stat& expected_stat,
    QString* error) {
  if (!fallback.retire_on_success)
    return true;
  const QByteArray fallback_name = QFile::encodeName(QFileInfo(fallback.path).fileName());
  int move_errno = 0;
  if (!rename_entry_no_replace(parent_fd, fallback_name.constData(), cleanup_fd, "fallback", &move_errno)) {
    if (move_errno == ENOENT)
      return true;
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(move_errno));
    return false;
  }
  if (qEnvironmentVariable("HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE") == fallback.path) {
    qunsetenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE");
    if (error)
      *error = "cleanup interruption requested after fallback quarantine";
    return false;
  }
  struct stat quarantined_stat{};
  if (::fstatat(cleanup_fd, "fallback", &quarantined_stat, AT_SYMLINK_NOFOLLOW) != 0) {
    const int inspect_errno = errno;
    int restore_errno = 0;
    if (rename_entry_no_replace(cleanup_fd, "fallback", parent_fd, fallback_name.constData(), &restore_errno))
      ::fsync(parent_fd);
    if (error)
      *error = QString("could not inspect quarantined public cleanup fallback: %1")
                   .arg(QString::fromLocal8Bit(std::strerror(inspect_errno)));
    return false;
  }
  if (!same_file_identity(quarantined_stat, expected_stat)) {
    QByteArray restored_name;
    QString restore_error;
    const bool restored = durably_publish_cleanup_fallback(
        cleanup_fd, "fallback", parent_fd, fallback_name, quarantined_stat, &restored_name, &restore_error);
    if (restored) {
      ::unlinkat(cleanup_fd, "fallback", 0);
      ::fsync(cleanup_fd);
    }
    if (error)
      *error = QString("public cleanup fallback was replaced at %1: %2").arg(fallback.path, restore_error);
    return false;
  }
  if (::unlinkat(cleanup_fd, "fallback", 0) != 0 || ::fsync(cleanup_fd) != 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  return true;
}

bool create_open_file_guard(int source_fd, const QString& protected_path, QString* guard_path, QString* error) {
  const QString candidate = protected_path + ".hstream-pin";
  int saved_errno = 0;
  if (link_open_file_no_replace(source_fd, candidate, &saved_errno)) {
    if (guard_path)
      *guard_path = candidate;
    return true;
  }
  if (error)
    *error = QString::fromLocal8Bit(std::strerror(saved_errno));
  return false;
}

bool remove_path_if_same_identity(
    const QString& path,
    const struct stat& expected_stat,
    QString* error,
    const QString& required_identity_path,
    const struct stat* required_identity_stat);

bool copy_open_file_no_replace(
    int source_fd,
    const QString& destination,
    int* destination_fd,
    struct stat* destination_stat,
    int* saved_errno,
    QString* rollback_error);

QString rescue_open_file_no_replace(
    int source_fd,
    const struct stat& expected_stat,
    const QString& preferred_path,
    QString* error,
    struct stat* rescued_stat = nullptr) {
  struct stat pinned_source_stat{};
  if (source_fd < 0 || ::fstat(source_fd, &pinned_source_stat) != 0 || !S_ISREG(pinned_source_stat.st_mode) ||
      !same_file_identity(pinned_source_stat, expected_stat)) {
    if (error)
      *error = "the pinned source identity is unavailable";
    return {};
  }
  for (int suffix = 0; suffix < 1000; ++suffix) {
    const QString candidate = suffix == 0 ? preferred_path + ".hstream-rescue"
                                          : QString("%1.hstream-rescue-%2").arg(preferred_path).arg(suffix);
    int saved_errno = 0;
    struct stat published_stat = expected_stat;
    bool published = link_open_file_no_replace(source_fd, candidate, &saved_errno);
    QString copy_rollback_error;
    if (!published && saved_errno != EEXIST) {
      int copied_fd = -1;
      published = copy_open_file_no_replace(
          source_fd, candidate, &copied_fd, &published_stat, &saved_errno, &copy_rollback_error);
      if (copied_fd >= 0)
        ::close(copied_fd);
    }
    if (!published) {
      if (saved_errno == EEXIST)
        continue;
      if (error) {
        *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        if (!copy_rollback_error.isEmpty())
          *error += QString("; copy rollback remains unresolved: %1").arg(copy_rollback_error);
      }
      return {};
    }
    if (!path_has_file_identity(candidate, published_stat)) {
      if (error)
        *error = QString("rescue pathname was replaced while being published: %1").arg(candidate);
      return {};
    }
    QString sync_error;
    if (sync_parent_directory(candidate, &sync_error)) {
      if (rescued_stat)
        *rescued_stat = published_stat;
      return candidate;
    }
    QString cleanup_error;
    remove_path_if_same_identity(candidate, published_stat, &cleanup_error, {}, nullptr);
    if (error) {
      *error = QString("could not make rescue pathname durable: %1").arg(sync_error);
      if (!cleanup_error.isEmpty())
        *error += QString("; rescue cleanup remains unresolved: %1").arg(cleanup_error);
    }
    return {};
  }
  if (error)
    *error = "no unused rescue pathname remained";
  return {};
}

bool copy_open_file_no_replace(
    int source_fd,
    const QString& destination,
    int* destination_fd,
    struct stat* destination_stat,
    int* saved_errno,
    QString* rollback_error) {
  const QByteArray encoded_destination = QFile::encodeName(destination);
  const int output_fd =
      ::open(encoded_destination.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (output_fd < 0) {
    if (saved_errno)
      *saved_errno = errno;
    return false;
  }

  struct stat created_stat{};
  if (::fstat(output_fd, &created_stat) != 0 || !S_ISREG(created_stat.st_mode)) {
    const int created_stat_errno = errno;
    if (rollback_error) {
      *rollback_error =
          QString("created destination identity is unavailable; partial copy retained at %1").arg(destination);
    }
    ::close(output_fd);
    if (saved_errno)
      *saved_errno = created_stat_errno;
    return false;
  }

  bool copied = true;
  if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_COPY_REPLACEMENT")) {
    qunsetenv("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_COPY_REPLACEMENT");
    ::unlink(encoded_destination.constData());
    const int replacement_fd =
        ::open(encoded_destination.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (replacement_fd >= 0) {
      constexpr char kReplacement[] = "injected foreign cross-filesystem log";
      const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
      (void)replacement_bytes;
      ::close(replacement_fd);
    }
    copied = false;
    errno = EIO;
  }
  off_t offset = 0;
  std::array<char, 64 * 1024> buffer{};
  while (copied) {
    const ssize_t bytes_read = ::pread(source_fd, buffer.data(), buffer.size(), offset);
    if (bytes_read == 0)
      break;
    if (bytes_read < 0) {
      if (errno == EINTR)
        continue;
      copied = false;
      break;
    }
    ssize_t written = 0;
    while (written < bytes_read) {
      const ssize_t result = ::write(output_fd, buffer.data() + written, bytes_read - written);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0) {
        copied = false;
        break;
      }
      written += result;
    }
    if (!copied)
      break;
    offset += bytes_read;
  }

  struct stat copied_stat{};
  if (!copied || ::fsync(output_fd) != 0 || ::fstat(output_fd, &copied_stat) != 0 || !S_ISREG(copied_stat.st_mode)) {
    const int copy_errno = errno;
    QString cleanup_error;
    if (!remove_path_if_same_identity(destination, created_stat, &cleanup_error, {}, nullptr) && rollback_error)
      *rollback_error = cleanup_error;
    ::close(output_fd);
    if (saved_errno)
      *saved_errno = copy_errno;
    return false;
  }
  if (destination_fd)
    *destination_fd = output_fd;
  else
    ::close(output_fd);
  if (destination_stat)
    *destination_stat = copied_stat;
  return true;
}

bool rename_entry_no_replace(
    int source_directory_fd,
    const char* source_name,
    int destination_directory_fd,
    const char* destination_name,
    int* saved_errno) {
#ifdef Q_OS_LINUX
  constexpr unsigned int kRenameNoReplace = 1;
  if (::syscall(
          SYS_renameat2,
          source_directory_fd,
          source_name,
          destination_directory_fd,
          destination_name,
          kRenameNoReplace) == 0) {
    return true;
  }
  const int rename_errno = errno;
  if (saved_errno)
    *saved_errno = rename_errno;
  return false;
#else
  if (::linkat(source_directory_fd, source_name, destination_directory_fd, destination_name, 0) != 0) {
    if (saved_errno)
      *saved_errno = errno;
    return false;
  }
  if (::unlinkat(source_directory_fd, source_name, 0) == 0)
    return true;
  if (saved_errno)
    *saved_errno = errno;
  return false;
#endif
}

constexpr char kUiCleanupDirectoryPrefix[] = ".hstream-cleanup-v1-";
constexpr char kUiCleanupOwnerSuffix[] = ".hstream-owner";
constexpr char kUiCleanupOwnerMagic[] = "hstream-cleanup-v1\n";

QByteArray ui_cleanup_owner_name(const QByteArray& cleanup_name) {
  return cleanup_name + kUiCleanupOwnerSuffix;
}

bool create_ui_cleanup_owner(
    int parent_fd,
    const QByteArray& cleanup_name,
    const QByteArray& target_name,
    QString* error) {
  const QByteArray owner_name = ui_cleanup_owner_name(cleanup_name);
  const int owner_fd = ::openat(
      parent_fd, owner_name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (owner_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  const QByteArray record = QByteArray(kUiCleanupOwnerMagic) + target_name.toBase64();
  qsizetype written = 0;
  int saved_errno = 0;
  while (written < record.size()) {
    const ssize_t result = ::write(owner_fd, record.constData() + written, record.size() - written);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      saved_errno = result < 0 ? errno : EIO;
      break;
    }
    written += result;
  }
  if (saved_errno == 0 && ::fsync(owner_fd) != 0)
    saved_errno = errno;
  if (::close(owner_fd) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (saved_errno == 0 && ::fsync(parent_fd) != 0)
    saved_errno = errno;
  if (saved_errno == 0)
    return true;
  ::unlinkat(parent_fd, owner_name.constData(), 0);
  ::fsync(parent_fd);
  if (error)
    *error = QString::fromLocal8Bit(std::strerror(saved_errno));
  return false;
}

bool read_ui_cleanup_owner(int parent_fd, const QByteArray& cleanup_name, QByteArray* target_name, QString* error) {
  const QByteArray owner_name = ui_cleanup_owner_name(cleanup_name);
  const int owner_fd = ::openat(parent_fd, owner_name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (owner_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  struct stat owner_stat{};
  QByteArray record;
  int saved_errno = 0;
  if (::fstat(owner_fd, &owner_stat) != 0) {
    saved_errno = errno;
  } else if (!S_ISREG(owner_stat.st_mode) || owner_stat.st_size <= 0 || owner_stat.st_size > 64 * 1024) {
    saved_errno = EINVAL;
  } else {
    record.resize(static_cast<qsizetype>(owner_stat.st_size));
    qsizetype read_bytes = 0;
    while (read_bytes < record.size()) {
      const ssize_t result = ::read(owner_fd, record.data() + read_bytes, record.size() - read_bytes);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0) {
        saved_errno = result < 0 ? errno : EIO;
        break;
      }
      read_bytes += result;
    }
  }
  if (::close(owner_fd) != 0 && saved_errno == 0)
    saved_errno = errno;
  const QByteArray magic(kUiCleanupOwnerMagic);
  if (saved_errno == 0 && record.startsWith(magic)) {
    const QByteArray encoded_target = record.mid(magic.size());
    const QByteArray decoded_target = QByteArray::fromBase64(encoded_target);
    if (!encoded_target.isEmpty() && decoded_target.toBase64() == encoded_target && !decoded_target.isEmpty() &&
        !decoded_target.contains('/') && !decoded_target.contains('\0') && decoded_target != "." &&
        decoded_target != "..") {
      if (target_name)
        *target_name = decoded_target;
      return true;
    }
    saved_errno = EINVAL;
  } else if (saved_errno == 0) {
    saved_errno = EINVAL;
  }
  if (error)
    *error = QString::fromLocal8Bit(std::strerror(saved_errno));
  return false;
}

bool retire_ui_cleanup_directory(int parent_fd, const QByteArray& cleanup_name) {
  if (::unlinkat(parent_fd, cleanup_name.constData(), AT_REMOVEDIR) != 0)
    return false;
  const QByteArray owner_name = ui_cleanup_owner_name(cleanup_name);
  if (::unlinkat(parent_fd, owner_name.constData(), 0) != 0 && errno != ENOENT)
    return false;
  return true;
}

QString publish_unique_ui_reconciliation_guard(
    int source_fd,
    const struct stat& expected_stat,
    const QString& public_path,
    QString* error) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    const QString candidate = public_path + ".hstream-reconcile-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    int link_errno = 0;
    if (!link_open_file_no_replace(source_fd, candidate, &link_errno)) {
      if (link_errno == EEXIST)
        continue;
      if (error)
        *error = QString::fromLocal8Bit(std::strerror(link_errno));
      return {};
    }
    QString sync_error;
    if (!sync_parent_directory(candidate, &sync_error) || !path_has_file_identity(candidate, expected_stat)) {
      if (error)
        *error = QString("could not make reconciliation guard durable at %1: %2").arg(candidate, sync_error);
      return {};
    }
    return candidate;
  }
  if (error)
    *error = "no unused reconciliation guard name remained";
  return {};
}

bool reconcile_scoped_ui_cleanup_directory(const QString& directory_path, QString* error) {
  struct ReconciledEntry {
    QString public_path;
    QString guard_path;
    struct stat identity{};
  };
  const auto finish_reconciled_entry = [error](const ReconciledEntry& entry) {
    constexpr const char* kFallbackSuffix = ".hstream-cleanup-pin";
    QString required_path = entry.public_path;
    if (entry.public_path.endsWith(kFallbackSuffix)) {
      const QString original_path =
          entry.public_path.left(entry.public_path.size() - static_cast<qsizetype>(std::strlen(kFallbackSuffix)));
      bool retire_interrupted_guard = false;
      if (original_path.endsWith(".hstream-pin")) {
        const QString guarded_path = original_path.left(original_path.size() - std::strlen(".hstream-pin"));
        retire_interrupted_guard = path_has_file_identity(guarded_path, entry.identity);
        if (retire_interrupted_guard)
          required_path = guarded_path;
      }
      if (!retire_interrupted_guard) {
        struct stat original_stat{};
        const QByteArray encoded_original = QFile::encodeName(original_path);
        if (::lstat(encoded_original.constData(), &original_stat) != 0) {
          if (errno != ENOENT) {
            if (error)
              *error = QString::fromLocal8Bit(std::strerror(errno));
            return false;
          }
          const QByteArray encoded_guard = QFile::encodeName(entry.guard_path);
          const int guard_fd = ::open(encoded_guard.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
          int publish_errno = 0;
          const bool published = guard_fd >= 0 && link_open_file_no_replace(guard_fd, original_path, &publish_errno);
          if (guard_fd >= 0)
            ::close(guard_fd);
          QString sync_error;
          if (!published || !sync_parent_directory(original_path, &sync_error) ||
              !path_has_file_identity(original_path, entry.identity)) {
            if (error) {
              *error = QString("could not restore interrupted cleanup path %1: %2; trusted inode retained at %3")
                           .arg(
                               original_path,
                               published ? sync_error : QString::fromLocal8Bit(std::strerror(publish_errno)),
                               entry.guard_path);
            }
            return false;
          }
        } else if (!same_file_identity(original_stat, entry.identity)) {
          if (error)
            *error = QString("cleanup restoration conflicts with %1; trusted inode retained at %2")
                         .arg(original_path, entry.guard_path);
          return false;
        }
        required_path = original_path;
      }
      struct stat fallback_stat{};
      const QByteArray encoded_fallback = QFile::encodeName(entry.public_path);
      if (::lstat(encoded_fallback.constData(), &fallback_stat) == 0) {
        if (!same_file_identity(fallback_stat, entry.identity)) {
          if (error)
            *error = QString("cleanup fallback was replaced at %1; trusted inode retained at %2")
                         .arg(entry.public_path, entry.guard_path);
          return false;
        }
        QString fallback_error;
        if (!remove_path_if_same_identity(
                entry.public_path, entry.identity, &fallback_error, required_path, &entry.identity)) {
          if (error)
            *error = fallback_error;
          return false;
        }
      } else if (errno != ENOENT) {
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
      }
    } else if (!path_has_file_identity(required_path, entry.identity)) {
      if (error)
        *error = QString("public cleanup identity changed at %1; trusted inode retained at %2")
                     .arg(required_path, entry.guard_path);
      return false;
    }
    QString guard_error;
    if (!remove_path_if_same_identity(entry.guard_path, entry.identity, &guard_error, required_path, &entry.identity)) {
      if (error)
        *error = guard_error;
      return false;
    }
    return true;
  };
  const QByteArray encoded_directory = QFile::encodeName(QFileInfo(directory_path).absoluteFilePath());
  const int parent_fd = ::open(encoded_directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  static const QRegularExpression cleanup_name_pattern(
      R"(^\.hstream-cleanup-v1-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)",
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression reconciliation_guard_name_pattern(
      R"(^.+\.hstream-reconcile-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)",
      QRegularExpression::CaseInsensitiveOption);
  const QStringList names =
      QDir(directory_path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
  for (const QString& cleanup_name : names) {
    if (!cleanup_name_pattern.match(cleanup_name).hasMatch())
      continue;
    const QByteArray encoded_cleanup = QFile::encodeName(cleanup_name);
    const int cleanup_fd =
        ::openat(parent_fd, encoded_cleanup.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (cleanup_fd < 0) {
      const int saved_errno = errno;
      if (saved_errno == ENOENT)
        continue;
      ::close(parent_fd);
      if (error)
        *error = QString::fromLocal8Bit(std::strerror(saved_errno));
      return false;
    }
    if (::flock(cleanup_fd, LOCK_EX | LOCK_NB) != 0) {
      const int saved_errno = errno;
      ::close(cleanup_fd);
      if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
        continue;
      ::close(parent_fd);
      if (error)
        *error = QString::fromLocal8Bit(std::strerror(saved_errno));
      return false;
    }
    QByteArray owned_target_name;
    QString owner_error;
    if (!read_ui_cleanup_owner(parent_fd, encoded_cleanup, &owned_target_name, &owner_error)) {
      ::close(cleanup_fd);
      continue;
    }
    std::vector<ReconciledEntry> reconciled;
    struct PrivateEntry {
      const char* name;
      struct stat identity{};
    };
    std::vector<PrivateEntry> private_entries;
    for (const char* private_name : {"entry", "guard", "fallback"}) {
      struct stat private_stat{};
      if (::fstatat(cleanup_fd, private_name, &private_stat, AT_SYMLINK_NOFOLLOW) == 0) {
        private_entries.push_back({private_name, private_stat});
      } else if (errno != ENOENT) {
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
    }
    if (private_entries.empty()) {
      const QString original_path = QDir(directory_path).filePath(QFile::decodeName(owned_target_name));
      const QString fallback_path = original_path + ".hstream-cleanup-pin";
      struct stat fallback_stat{};
      const QByteArray encoded_fallback = QFile::encodeName(fallback_path);
      if (::lstat(encoded_fallback.constData(), &fallback_stat) == 0) {
        struct stat original_stat{};
        const QByteArray encoded_original = QFile::encodeName(original_path);
        if (::lstat(encoded_original.constData(), &original_stat) == 0) {
          if (!same_file_identity(original_stat, fallback_stat)) {
            ::close(cleanup_fd);
            ::close(parent_fd);
            if (error)
              *error = QString("cleanup fallback conflicts with %1").arg(original_path);
            return false;
          }
        } else if (errno == ENOENT) {
          const int fallback_fd = ::open(encoded_fallback.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
          struct stat pinned_fallback_stat{};
          int publish_errno = 0;
          const bool fallback_pinned = fallback_fd >= 0 && ::fstat(fallback_fd, &pinned_fallback_stat) == 0 &&
              same_file_identity(pinned_fallback_stat, fallback_stat);
          const bool restored =
              fallback_pinned && link_open_file_no_replace(fallback_fd, original_path, &publish_errno);
          if (!fallback_pinned)
            publish_errno = fallback_fd < 0 ? errno : ESTALE;
          if (fallback_fd >= 0)
            ::close(fallback_fd);
          QString sync_error;
          if (!restored || !sync_parent_directory(original_path, &sync_error) ||
              !path_has_file_identity(original_path, fallback_stat)) {
            ::close(cleanup_fd);
            ::close(parent_fd);
            if (error) {
              *error =
                  QString("could not restore empty cleanup transaction for %1: %2")
                      .arg(original_path, restored ? sync_error : QString::fromLocal8Bit(std::strerror(publish_errno)));
            }
            return false;
          }
        } else {
          const int saved_errno = errno;
          ::close(cleanup_fd);
          ::close(parent_fd);
          if (error)
            *error = QString::fromLocal8Bit(std::strerror(saved_errno));
          return false;
        }
        QString fallback_error;
        if (!remove_path_if_same_identity(
                fallback_path, fallback_stat, &fallback_error, original_path, &fallback_stat)) {
          ::close(cleanup_fd);
          ::close(parent_fd);
          if (error)
            *error = fallback_error;
          return false;
        }
      } else if (errno != ENOENT) {
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
      if (!retire_ui_cleanup_directory(parent_fd, encoded_cleanup) || ::fsync(parent_fd) != 0) {
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
      ::close(cleanup_fd);
      continue;
    }
    const bool committed_delete = std::none_of(private_entries.begin(), private_entries.end(), [](const auto& entry) {
      return std::strcmp(entry.name, "entry") == 0;
    });
    for (const PrivateEntry& private_entry : private_entries) {
      QString public_path;
      for (const QString& public_name : names) {
        if (cleanup_name_pattern.match(public_name).hasMatch() ||
            reconciliation_guard_name_pattern.match(public_name).hasMatch())
          continue;
        const QString candidate = QDir(directory_path).filePath(public_name);
        if (path_has_file_identity(candidate, private_entry.identity)) {
          public_path = candidate;
          if (public_name.endsWith(".hstream-cleanup-pin"))
            break;
        }
      }
      QString guard_path;
      if (!public_path.isEmpty()) {
        const int private_fd = ::openat(cleanup_fd, private_entry.name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat pinned_stat{};
        if (private_fd < 0 || ::fstat(private_fd, &pinned_stat) != 0 ||
            !same_file_identity(pinned_stat, private_entry.identity)) {
          const int saved_errno = private_fd < 0 ? errno : ESTALE;
          if (private_fd >= 0)
            ::close(private_fd);
          ::close(cleanup_fd);
          ::close(parent_fd);
          if (error)
            *error = QString::fromLocal8Bit(std::strerror(saved_errno));
          return false;
        }
        QString guard_error;
        guard_path = publish_unique_ui_reconciliation_guard(private_fd, pinned_stat, public_path, &guard_error);
        ::close(private_fd);
        if (guard_path.isEmpty()) {
          ::close(cleanup_fd);
          ::close(parent_fd);
          if (error)
            *error = guard_error;
          return false;
        }
        if (qEnvironmentVariable("HSTREAM_UI_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION") == public_path) {
          qunsetenv("HSTREAM_UI_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION");
          const QByteArray encoded_public = QFile::encodeName(public_path);
          ::unlink(encoded_public.constData());
          const int replacement_fd =
              ::open(encoded_public.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
          if (replacement_fd >= 0) {
            constexpr char kReplacement[] = "foreign public cleanup identity";
            const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
            (void)replacement_bytes;
            ::close(replacement_fd);
          }
        }
      } else if (!committed_delete) {
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString("HStream cleanup entry has no public identity at %1/%2")
                       .arg(QDir(directory_path).filePath(cleanup_name), private_entry.name);
        return false;
      }
      struct stat current_stat{};
      if (::fstatat(cleanup_fd, private_entry.name, &current_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
          !same_file_identity(current_stat, private_entry.identity)) {
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
      if (!guard_path.isEmpty())
        reconciled.push_back({public_path, guard_path, private_entry.identity});
    }
    for (const ReconciledEntry& entry : reconciled) {
      if (!finish_reconciled_entry(entry)) {
        ::close(cleanup_fd);
        ::close(parent_fd);
        return false;
      }
    }
    for (const QString& candidate_name : names) {
      if (!reconciliation_guard_name_pattern.match(candidate_name).hasMatch())
        continue;
      const QString candidate_path = QDir(directory_path).filePath(candidate_name);
      struct stat candidate_stat{};
      const QByteArray encoded_candidate = QFile::encodeName(candidate_path);
      if (::lstat(encoded_candidate.constData(), &candidate_stat) != 0) {
        if (errno == ENOENT)
          continue;
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
      const auto reconciled_identity = std::find_if(reconciled.begin(), reconciled.end(), [&](const auto& entry) {
        return same_file_identity(candidate_stat, entry.identity);
      });
      if (reconciled_identity == reconciled.end())
        continue;
      QString required_path;
      const QStringList current_names =
          QDir(directory_path)
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
      for (const QString& current_name : current_names) {
        if (cleanup_name_pattern.match(current_name).hasMatch() ||
            reconciliation_guard_name_pattern.match(current_name).hasMatch())
          continue;
        const QString current_path = QDir(directory_path).filePath(current_name);
        if (path_has_file_identity(current_path, candidate_stat)) {
          required_path = current_path;
          break;
        }
      }
      if (required_path.isEmpty()) {
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString("reconciliation guard has no durable public identity at %1").arg(candidate_path);
        return false;
      }
      QString stale_guard_error;
      if (!remove_path_if_same_identity(
              candidate_path, candidate_stat, &stale_guard_error, required_path, &candidate_stat)) {
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = stale_guard_error;
        return false;
      }
    }
    for (const PrivateEntry& private_entry : private_entries) {
      struct stat current_stat{};
      if (::fstatat(cleanup_fd, private_entry.name, &current_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
          !same_file_identity(current_stat, private_entry.identity) ||
          ::unlinkat(cleanup_fd, private_entry.name, 0) != 0) {
        const int saved_errno = errno;
        ::close(cleanup_fd);
        ::close(parent_fd);
        if (error)
          *error = QString::fromLocal8Bit(std::strerror(saved_errno));
        return false;
      }
    }
    if (::fsync(cleanup_fd) != 0) {
      const int saved_errno = errno;
      ::close(cleanup_fd);
      ::close(parent_fd);
      if (error)
        *error = QString::fromLocal8Bit(std::strerror(saved_errno));
      return false;
    }
    if (!retire_ui_cleanup_directory(parent_fd, encoded_cleanup) || ::fsync(parent_fd) != 0) {
      const int saved_errno = errno;
      ::close(cleanup_fd);
      ::close(parent_fd);
      if (error)
        *error = QString::fromLocal8Bit(std::strerror(saved_errno));
      return false;
    }
    ::close(cleanup_fd);
  }
  ::close(parent_fd);
  return true;
}

bool remove_path_if_same_identity(
    const QString& path,
    const struct stat& expected_stat,
    QString* error,
    const QString& required_identity_path = {},
    const struct stat* required_identity_stat = nullptr) {
  const QByteArray encoded_path = QFile::encodeName(path);
  if (!path_has_file_identity(path, expected_stat)) {
    if (error)
      *error = QString("refusing to remove a replaced path: %1").arg(path);
    return false;
  }

  const int pinned_fd = ::open(encoded_path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct stat pinned_stat{};
  if (pinned_fd < 0 || ::fstat(pinned_fd, &pinned_stat) != 0 || !S_ISREG(pinned_stat.st_mode) ||
      !same_file_identity(pinned_stat, expected_stat)) {
    const int saved_errno = pinned_fd < 0 ? errno : ESTALE;
    if (pinned_fd >= 0)
      ::close(pinned_fd);
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(saved_errno));
    return false;
  }

  const QFileInfo path_info(path);
  const QByteArray encoded_parent = QFile::encodeName(path_info.absolutePath());
  const QByteArray encoded_filename = QFile::encodeName(path_info.fileName());
  const int parent_fd = ::open(encoded_parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    ::close(pinned_fd);
    return false;
  }
  const QByteArray cleanup_name =
      QFile::encodeName(QString(kUiCleanupDirectoryPrefix) + QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (::mkdirat(parent_fd, cleanup_name.constData(), S_IRWXU) != 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    ::close(pinned_fd);
    ::close(parent_fd);
    return false;
  }
  const int cleanup_fd = ::openat(parent_fd, cleanup_name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (cleanup_fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    ::unlinkat(parent_fd, cleanup_name.constData(), AT_REMOVEDIR);
    ::close(pinned_fd);
    ::close(parent_fd);
    return false;
  }
  QString cleanup_owner_error;
  if (::flock(cleanup_fd, LOCK_EX | LOCK_NB) != 0 ||
      !create_ui_cleanup_owner(parent_fd, cleanup_name, encoded_filename, &cleanup_owner_error)) {
    const int saved_errno = cleanup_owner_error.isEmpty() ? errno : 0;
    ::close(cleanup_fd);
    ::unlinkat(parent_fd, cleanup_name.constData(), AT_REMOVEDIR);
    ::fsync(parent_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (error) {
      *error = cleanup_owner_error.isEmpty() ? QString::fromLocal8Bit(std::strerror(saved_errno)) : cleanup_owner_error;
    }
    return false;
  }

  DurableUiRemovalFallback durable_removal_fallback;
  QString durable_fallback_error;
  if (!ensure_durable_ui_removal_fallback(
          pinned_fd, path, pinned_stat, &durable_removal_fallback, &durable_fallback_error)) {
    retire_ui_cleanup_directory(parent_fd, cleanup_name);
    ::close(cleanup_fd);
    ::fsync(parent_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (error)
      *error = durable_fallback_error;
    return false;
  }

  int move_errno = 0;
  if (!rename_entry_no_replace(parent_fd, encoded_filename.constData(), cleanup_fd, "entry", &move_errno)) {
    QString fallback_retirement_error;
    const bool fallback_retired = retire_durable_ui_removal_fallback(
        cleanup_fd, parent_fd, durable_removal_fallback, pinned_stat, &fallback_retirement_error);
    if (fallback_retired) {
      retire_ui_cleanup_directory(parent_fd, cleanup_name);
      ::fsync(parent_fd);
    }
    ::close(cleanup_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (move_errno == ENOENT && fallback_retired)
      return true;
    if (error)
      *error = fallback_retired ? QString::fromLocal8Bit(std::strerror(move_errno)) : fallback_retirement_error;
    return false;
  }
  if (qEnvironmentVariable("HSTREAM_UI_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE") == path) {
    qunsetenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
    ::close(cleanup_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (error)
      *error = "archive cleanup interruption requested after quarantine";
    return false;
  }

  struct stat quarantined_stat{};
  const bool quarantined_was_inspected = ::fstatat(cleanup_fd, "entry", &quarantined_stat, AT_SYMLINK_NOFOLLOW) == 0;
  const bool quarantined_is_ours = quarantined_was_inspected && same_file_identity(quarantined_stat, expected_stat);
  bool required_identity_is_ours = true;
  if (!required_identity_path.isEmpty()) {
    const struct stat& expected_required_stat = required_identity_stat ? *required_identity_stat : expected_stat;
    required_identity_is_ours = path_has_file_identity(required_identity_path, expected_required_stat);
  }
  if (!quarantined_is_ours || !required_identity_is_ours) {
    QByteArray retained_name;
    QString retention_error;
    const bool retained =
        quarantined_was_inspected &&
        durably_publish_cleanup_fallback(
            cleanup_fd, "entry", parent_fd, encoded_filename, quarantined_stat, &retained_name, &retention_error);
    int cleanup_result = -1;
    int cleanup_errno = 0;
    if (retained) {
      cleanup_result = ::unlinkat(cleanup_fd, "entry", 0);
      cleanup_errno = errno;
      if (cleanup_result == 0) {
        cleanup_result = ::fsync(cleanup_fd);
        cleanup_errno = errno;
      }
    }
    if (cleanup_result == 0) {
      cleanup_result = retire_ui_cleanup_directory(parent_fd, cleanup_name) ? 0 : -1;
      cleanup_errno = errno;
      if (cleanup_result == 0) {
        cleanup_result = ::fsync(parent_fd);
        cleanup_errno = errno;
      }
    }
    ::close(cleanup_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (error) {
      *error = retained ? QString("refusing to remove a replaced path: %1; retained at %2")
                              .arg(path, QString::fromLocal8Bit(retained_name))
                        : QString("could not retain quarantined path %1: %2").arg(path, retention_error);
      if (!retained)
        *error += "; cleanup fallback retained";
      else if (cleanup_result != 0)
        *error += QString("; cleanup fallback remains: %1").arg(QString::fromLocal8Bit(std::strerror(cleanup_errno)));
    }
    return false;
  }

  // Retain a private guard link until the publication has survived a second
  // identity check after the quarantined pathname is removed.
  if (::linkat(cleanup_fd, "entry", cleanup_fd, "guard", 0) != 0) {
    const int saved_errno = errno;
    QByteArray retained_name;
    QString retention_error;
    const bool retained = durably_publish_cleanup_fallback(
        cleanup_fd, "entry", parent_fd, encoded_filename, pinned_stat, &retained_name, &retention_error);
    int cleanup_result = -1;
    if (retained) {
      cleanup_result = ::unlinkat(cleanup_fd, "entry", 0);
      if (cleanup_result == 0)
        cleanup_result = ::fsync(cleanup_fd);
    }
    if (cleanup_result == 0) {
      cleanup_result = retire_ui_cleanup_directory(parent_fd, cleanup_name) ? 0 : -1;
      if (cleanup_result == 0)
        cleanup_result = ::fsync(parent_fd);
    }
    ::close(cleanup_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (error) {
      *error = QString::fromLocal8Bit(std::strerror(saved_errno));
      if (!retained)
        *error += QString("; source retention failed: %1").arg(retention_error);
    }
    return false;
  }

  const int unlink_result = ::unlinkat(cleanup_fd, "entry", 0);
  const int unlink_errno = errno;
  if (unlink_result == 0 && !required_identity_path.isEmpty()) {
    const QString replacement_trigger = qEnvironmentVariable("HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
    if (!replacement_trigger.isEmpty() && replacement_trigger == path) {
      qunsetenv("HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
      const QByteArray encoded_required = QFile::encodeName(required_identity_path);
      ::unlink(encoded_required.constData());
      const int replacement_fd =
          ::open(encoded_required.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign publication after quarantine";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    const struct stat& expected_required_stat = required_identity_stat ? *required_identity_stat : expected_stat;
    if (!path_has_file_identity(required_identity_path, expected_required_stat)) {
      QByteArray retained_name;
      QString retention_error;
      const bool rescued = durably_publish_cleanup_fallback(
          cleanup_fd, "guard", parent_fd, encoded_filename, pinned_stat, &retained_name, &retention_error);
      int cleanup_result = -1;
      int cleanup_errno = 0;
      if (rescued) {
        cleanup_result = ::unlinkat(cleanup_fd, "guard", 0);
        cleanup_errno = errno;
        if (cleanup_result == 0) {
          cleanup_result = ::fsync(cleanup_fd);
          cleanup_errno = errno;
        }
      }
      if (cleanup_result == 0) {
        cleanup_result = retire_ui_cleanup_directory(parent_fd, cleanup_name) ? 0 : -1;
        cleanup_errno = errno;
        if (cleanup_result == 0) {
          cleanup_result = ::fsync(parent_fd);
          cleanup_errno = errno;
        }
      }
      ::close(cleanup_fd);
      ::close(pinned_fd);
      ::close(parent_fd);
      if (error) {
        *error = rescued
            ? QString("published path was replaced; retained source at %1")
                  .arg(QDir(QFileInfo(path).absolutePath()).filePath(QString::fromLocal8Bit(retained_name)))
            : QString("published path was replaced and source could not be retained: %1").arg(retention_error);
        if (!rescued)
          *error += "; cleanup fallback retained";
        else if (cleanup_result != 0)
          *error += QString("; cleanup fallback remains: %1").arg(QString::fromLocal8Bit(std::strerror(cleanup_errno)));
      }
      return false;
    }
  }
  QString fallback_retirement_error;
  const bool fallback_retired = retire_durable_ui_removal_fallback(
      cleanup_fd, parent_fd, durable_removal_fallback, pinned_stat, &fallback_retirement_error);
  const int guard_unlink_result = fallback_retired ? ::unlinkat(cleanup_fd, "guard", 0) : -1;
  const int guard_unlink_errno = errno;
  const int cleanup_result = fallback_retired && retire_ui_cleanup_directory(parent_fd, cleanup_name) ? 0 : -1;
  const int cleanup_errno = errno;
  ::close(cleanup_fd);
  int directory_sync_result = 0;
  int directory_sync_errno = 0;
  if (unlink_result == 0 && guard_unlink_result == 0 && fallback_retired && cleanup_result == 0) {
    const QString forced_sync_failure = qEnvironmentVariable("HSTREAM_UI_TEST_ARCHIVE_CLEANUP_PARENT_SYNC_FAILURE");
    const bool force_directory_sync_failure =
        forced_sync_failure == path || (forced_sync_failure == "mp4-guard" && path.endsWith(".mp4.hstream-pin"));
    if (force_directory_sync_failure) {
      qunsetenv("HSTREAM_UI_TEST_ARCHIVE_CLEANUP_PARENT_SYNC_FAILURE");
      directory_sync_result = -1;
      directory_sync_errno = EIO;
    } else {
      directory_sync_result = ::fsync(parent_fd);
      directory_sync_errno = errno;
    }
    if (directory_sync_result != 0) {
      int restore_errno = 0;
      const bool restored = link_open_file_no_replace(pinned_fd, path, &restore_errno);
      int rescue_errno = 0;
      const bool rescued = restored || link_open_file_no_replace(pinned_fd, path + ".hstream-pin", &rescue_errno);
      if (rescued)
        ::fsync(parent_fd);
      if (error) {
        *error = restored
            ? QString("directory sync failed after cleanup; restored path at %1: %2")
                  .arg(path, QString::fromLocal8Bit(std::strerror(directory_sync_errno)))
            : (rescued
                   ? QString("directory sync failed after cleanup; retained identity at %1.hstream-pin: %2")
                         .arg(path, QString::fromLocal8Bit(std::strerror(directory_sync_errno)))
                   : QString("directory sync failed after cleanup and the identity could not be retained: %1; %2 / %3")
                         .arg(
                             QString::fromLocal8Bit(std::strerror(directory_sync_errno)),
                             QString::fromLocal8Bit(std::strerror(restore_errno)),
                             QString::fromLocal8Bit(std::strerror(rescue_errno))));
      }
    }
  }
  ::close(pinned_fd);
  ::close(parent_fd);
  if (unlink_result == 0 && guard_unlink_result == 0 && fallback_retired && cleanup_result == 0 &&
      directory_sync_result == 0) {
    return true;
  }
  if (error && directory_sync_result == 0) {
    *error = !fallback_retired && guard_unlink_result == 0
        ? fallback_retirement_error
        : QString::fromLocal8Bit(
              std::strerror(
                  unlink_result != 0 ? unlink_errno : (guard_unlink_result != 0 ? guard_unlink_errno : cleanup_errno)));
  }
  return false;
}

bool remove_path_if_same_file(
    const QString& path,
    const QString& identity_path,
    QString* error,
    const QString& required_identity_path = {}) {
  struct stat identity_stat{};
  const QByteArray encoded_identity = QFile::encodeName(identity_path);
  if (::lstat(encoded_identity.constData(), &identity_stat) != 0) {
    if (error)
      *error = QString("could not inspect identity path: %1").arg(identity_path);
    return false;
  }
  return remove_path_if_same_identity(path, identity_stat, error, required_identity_path);
}

bool rename_path_no_replace(const QString& source, const QString& destination, int* saved_errno) {
  const QByteArray encoded_source = QFile::encodeName(source);
  const QByteArray encoded_destination = QFile::encodeName(destination);
#ifdef Q_OS_LINUX
  int rename_errno = 0;
  if (rename_entry_no_replace(
          AT_FDCWD, encoded_source.constData(), AT_FDCWD, encoded_destination.constData(), &rename_errno)) {
    return true;
  }
  if (rename_errno != ENOSYS && rename_errno != EINVAL) {
    if (saved_errno)
      *saved_errno = rename_errno;
    return false;
  }
#endif
  if (::link(encoded_source.constData(), encoded_destination.constData()) != 0) {
    if (saved_errno)
      *saved_errno = errno;
    return false;
  }
  if (::unlink(encoded_source.constData()) == 0)
    return true;
  const int unlink_errno = errno;
  QString cleanup_error;
  remove_path_if_same_file(destination, source, &cleanup_error);
  if (saved_errno)
    *saved_errno = unlink_errno;
  return false;
}
#endif

qint64 media_clock_us(const QString& value) {
  const QStringList fields = value.trimmed().split(':');
  if (fields.size() != 3)
    return -1;
  bool hours_ok = false;
  bool minutes_ok = false;
  bool seconds_ok = false;
  const qint64 hours = fields[0].toLongLong(&hours_ok);
  const qint64 minutes = fields[1].toLongLong(&minutes_ok);
  const double seconds = fields[2].toDouble(&seconds_ok);
  if (!hours_ok || !minutes_ok || !seconds_ok || hours < 0 || minutes < 0 || seconds < 0.0)
    return -1;
  return qRound64((hours * 3600.0 + minutes * 60.0 + seconds) * 1000000.0);
}

QString existing_auto_cam_dir_for_source(const QDir& game_dir, const QFileInfo& source) {
  const QString family = auto_file_family(source.fileName());
  const QString source_parent = canonical_dir_path(source.absolutePath());
  if (family.isEmpty() || source_parent.isEmpty()) {
    return {};
  }

  std::map<QString, std::pair<QString, QString>> copied_auto_sources;
  const fs::path config_path = fs::path(game_dir.absolutePath().toStdString()) / "config.yaml";
  if (fs::is_regular_file(config_path)) {
    try {
      YAML::Node config = YAML::LoadFile(config_path.string());
      YAML::Node entries = config["hstream_ui"]["auto_import_sources"];
      if (entries && entries.IsSequence()) {
        for (const auto& entry : entries) {
          if (!entry["path"] || !entry["family"] || !entry["source_parent"]) {
            continue;
          }
          copied_auto_sources[QString::fromStdString(entry["path"].as<std::string>())] = {
              QString::fromStdString(entry["family"].as<std::string>()),
              QString::fromStdString(entry["source_parent"].as<std::string>())};
        }
      }
    } catch (const std::exception&) {
      copied_auto_sources.clear();
    }
  }

  std::vector<QFileInfo> cam_dirs;
  const QFileInfoList dirs = game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
  for (const QFileInfo& dir : dirs) {
    if (cam_pattern.match(dir.fileName()).hasMatch() && !dir.isSymLink()) {
      cam_dirs.push_back(dir);
    }
  }
  std::sort(cam_dirs.begin(), cam_dirs.end(), [&cam_pattern](const QFileInfo& a, const QFileInfo& b) {
    return cam_pattern.match(a.fileName()).captured(1).toInt() < cam_pattern.match(b.fileName()).captured(1).toInt();
  });

  for (const QFileInfo& cam_dir : cam_dirs) {
    const QFileInfoList files =
        QDir(cam_dir.filePath()).entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      if (auto_file_family(file.fileName()) != family) {
        continue;
      }
      QString target_parent;
      if (file.isSymLink()) {
        target_parent = canonical_dir_path(QFileInfo(file.symLinkTarget()).absolutePath());
      } else {
        const QString relative_path = game_dir.relativeFilePath(file.filePath());
        const auto metadata = copied_auto_sources.find(relative_path);
        if (metadata == copied_auto_sources.end() || metadata->second.first != family) {
          continue;
        }
        target_parent = metadata->second.second;
      }
      if (target_parent == source_parent) {
        return cam_dir.fileName();
      }
    }
  }
  return {};
}

bool copied_auto_import_matches(
    const QDir& game_dir,
    const QString& relative_path,
    const QString& family,
    const QString& source_parent) {
  const fs::path config_path = fs::path(game_dir.absolutePath().toStdString()) / "config.yaml";
  if (!fs::is_regular_file(config_path)) {
    return false;
  }
  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    YAML::Node entries = config["hstream_ui"]["auto_import_sources"];
    if (!entries || !entries.IsSequence()) {
      return false;
    }
    for (const auto& entry : entries) {
      if (!entry["path"] || !entry["family"] || !entry["source_parent"]) {
        continue;
      }
      if (QString::fromStdString(entry["path"].as<std::string>()) == relative_path &&
          QString::fromStdString(entry["family"].as<std::string>()) == family &&
          QString::fromStdString(entry["source_parent"].as<std::string>()) == source_parent) {
        return true;
      }
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

QString sanitized_game_id(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "-");
  value.replace(QRegularExpression("-+"), "-");
  if (value.contains(QRegularExpression("^\\.+$"))) {
    value.clear();
  }
  return value;
}

QString role_label(const QString& role) {
  if (role == "left") {
    return "Left";
  }
  if (role == "center") {
    return "Center";
  }
  if (role == "right") {
    return "Right";
  }
  return "Auto";
}

bool is_explicit_role(const QString& role) {
  return role == "left" || role == "center" || role == "right";
}

bool is_cam_relative_path(const QString& path) {
  const QString clean = QDir::cleanPath(path);
  if (clean.startsWith("../") || QFileInfo(clean).isAbsolute()) {
    return false;
  }
  const int slash = clean.indexOf('/');
  if (slash < 0) {
    return false;
  }
  static const QRegularExpression cam_pattern("^cam[0-9]+$", QRegularExpression::CaseInsensitiveOption);
  return cam_pattern.match(clean.left(slash)).hasMatch();
}

QString normalized_config_video_path(const QDir& game_dir, const QString& path) {
  QFileInfo file(path);
  if (file.isAbsolute()) {
    const QString relative = game_dir.relativeFilePath(file.absoluteFilePath());
    if (!relative.startsWith("../") && relative != "..") {
      return relative;
    }
  }
  return path;
}

bool is_copied_import_in_config(const YAML::Node& config, const QDir& game_dir, const QString& relative_path) {
  YAML::Node list = config["hstream_ui"]["copied_imports"];
  if (!list || !list.IsSequence()) {
    return false;
  }
  for (const auto& item : list) {
    const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
    if (path == relative_path) {
      return true;
    }
  }
  return false;
}

bool config_references_video_path(const YAML::Node& config, const QDir& game_dir, const QString& relative_path) {
  const QString normalized_target = normalized_config_video_path(game_dir, relative_path);
  auto child = [](const YAML::Node& parent, const char* key) {
    if (parent.IsMap()) {
      for (const auto& entry : parent) {
        if (entry.first.IsScalar() && entry.first.as<std::string>() == key)
          return entry.second;
      }
    }
    return YAML::Node(YAML::NodeType::Undefined);
  };
  auto list_references_target = [&](const YAML::Node& list) {
    if (!list || !list.IsSequence())
      return false;
    for (const auto& item : list) {
      if (item.IsScalar() &&
          normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>())) == normalized_target) {
        return true;
      }
    }
    return false;
  };

  const YAML::Node roles = child(child(config, "hstream_ui"), "video_roles");
  for (const char* role : {"left", "center", "right"}) {
    if (list_references_target(child(roles, role)))
      return true;
  }
  const YAML::Node runtime_videos = child(child(config, "game"), "videos");
  return list_references_target(child(runtime_videos, "left")) ||
      list_references_target(child(runtime_videos, "right"));
}

bool clear_stitching_frame_offsets(YAML::Node& config) {
  bool changed = false;
  YAML::Node game_stitching = config["game"]["stitching"];
  if (game_stitching && game_stitching["frame_offsets"]) {
    game_stitching.remove("frame_offsets");
    changed = true;
  }
  YAML::Node stitching = config["stitching"];
  if (stitching && stitching["frame_offsets"]) {
    stitching.remove("frame_offsets");
    changed = true;
  }
  return changed;
}

bool invalidate_stitching_calibration(YAML::Node& config, const char* stale_from) {
  YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  if (!calibration || !calibration.IsMap()) {
    return false;
  }
  const std::string previous_status =
      calibration["status"] && calibration["status"].IsScalar() ? calibration["status"].as<std::string>() : "";
  const std::string previous_stale = calibration["stale_from"] && calibration["stale_from"].IsScalar()
      ? calibration["stale_from"].as<std::string>()
      : "";
  const bool previous_invalidated = calibration["artifacts_invalidated"] &&
      calibration["artifacts_invalidated"].IsScalar() && calibration["artifacts_invalidated"].as<bool>();
  const std::string previous_rink_status = calibration["rink_mask_status"] && calibration["rink_mask_status"].IsScalar()
      ? calibration["rink_mask_status"].as<std::string>()
      : "";
  const bool had_invalidation_id = calibration["invalidation_id"] && calibration["invalidation_id"].IsScalar();
  calibration["status"] = "pending";
  calibration["rink_mask_status"] = "pending";
  calibration["stale_from"] = stale_from;
  calibration["artifacts_invalidated"] = false;
  calibration.remove("invalidation_id");
  return previous_status != "pending" || previous_stale != stale_from || previous_invalidated ||
      previous_rink_status != "pending" || had_invalidation_id;
}

bool yaml_defined(YAML::Node node) {
  return node.IsDefined();
}

bool remove_yaml_key(YAML::Node parent, const char* key) {
  if (!yaml_defined(parent) || !parent.IsMap()) {
    return false;
  }
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.as<std::string>() == key) {
      parent.remove(key);
      return true;
    }
  }
  return false;
}

YAML::Node map_value(YAML::Node parent, const char* key) {
  if (!parent.IsMap()) {
    return YAML::Node();
  }
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.as<std::string>() == key) {
      return entry.second;
    }
  }
  return YAML::Node();
}

YAML::Node merge_yaml_maps(const YAML::Node& base, const YAML::Node& overlay) {
  if (!overlay.IsMap())
    return YAML::Clone(overlay);
  YAML::Node result = base.IsMap() ? YAML::Clone(base) : YAML::Node(YAML::NodeType::Map);
  for (const auto& entry : overlay) {
    const std::string key = entry.first.as<std::string>();
    YAML::Node base_child = result[key];
    if (entry.second.IsMap() && base_child.IsMap())
      result[key] = merge_yaml_maps(base_child, entry.second);
    else
      result[key] = YAML::Clone(entry.second);
  }
  return result;
}

bool lookup_yaml_key(YAML::Node parent, const char* key, YAML::Node* value) {
  if (!parent.IsMap()) {
    return false;
  }
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.as<std::string>() == key) {
      if (value) {
        *value = entry.second;
      }
      return true;
    }
  }
  return false;
}

bool has_yaml_key(YAML::Node parent, const char* key) {
  return lookup_yaml_key(parent, key, nullptr);
}

bool has_control(YAML::Node controls, const char* key) {
  return has_yaml_key(controls, key);
}

bool remove_yaml_key_mutating(YAML::Node parent, const char* key) {
  if (!parent.IsMap() || !has_yaml_key(parent, key)) {
    return false;
  }
  parent.remove(key);
  return true;
}

bool remove_yaml_path_at(YAML::Node node, const std::vector<const char*>& path, size_t index) {
  if (index + 1 == path.size()) {
    return remove_yaml_key_mutating(node, path[index]);
  }
  YAML::Node next;
  if (!lookup_yaml_key(node, path[index], &next)) {
    return false;
  }
  return remove_yaml_path_at(next, path, index + 1);
}

bool remove_yaml_path(YAML::Node root, std::initializer_list<const char*> path) {
  if (path.size() == 0) {
    return false;
  }
  return remove_yaml_path_at(root, std::vector<const char*>(path), 0);
}

bool remove_yaml_path(YAML::Node root, const QString& dotted_path) {
  const QStringList parts = dotted_path.split('.', Qt::SkipEmptyParts);
  if (parts.isEmpty()) {
    return false;
  }
  std::vector<std::string> storage;
  std::vector<const char*> path;
  storage.reserve(parts.size());
  path.reserve(parts.size());
  for (const QString& part : parts) {
    storage.push_back(part.toStdString());
    path.push_back(storage.back().c_str());
  }
  return remove_yaml_path_at(root, path, 0);
}

bool lookup_yaml_path_at(const YAML::Node& node, const QStringList& parts, int index, YAML::Node* value) {
  if (index >= parts.size()) {
    if (value) {
      *value = node;
    }
    return true;
  }
  const std::string key = parts[index].toStdString();
  YAML::Node next;
  if (!lookup_yaml_key(node, key.c_str(), &next)) {
    return false;
  }
  return lookup_yaml_path_at(next, parts, index + 1, value);
}

bool lookup_yaml_path(YAML::Node root, const QString& dotted_path, YAML::Node* value) {
  const QStringList parts = dotted_path.split('.', Qt::SkipEmptyParts);
  return lookup_yaml_path_at(root, parts, 0, value);
}

void remove_stitch_max_output_width_native_aliases(YAML::Node config) {
  for (const char* path : {
           "pipeline.hmstitcher.properties.max-output-width",
           "pipeline.hmstitcher.properties.max_output_width",
           "pipeline.hmstitcher.properties.stitch-max-output-width",
           "pipeline.hmstitcher.properties.stitch_max_output_width",
           "pipeline.hmstitcher.private-properties.max-output-width",
           "pipeline.hmstitcher.private-properties.max_output_width",
           "pipeline.hmstitcher.private-properties.stitch-max-output-width",
           "pipeline.hmstitcher.private-properties.stitch_max_output_width",
       }) {
    remove_yaml_path(config, QString::fromLatin1(path));
  }
}

int read_stitch_max_output_width_from_config(
    YAML::Node config,
    int default_value,
    int maximum_value,
    bool native_fallback_for_null_canonical = false) {
  auto read_node = [&](const QString& path, const YAML::Node& node) -> int {
    if (node.IsNull())
      return 0;
    if (!node.IsScalar())
      throw std::invalid_argument((path + " must be null or a non-negative integer").toStdString());
    const int value = node.as<int>();
    if (value < 0 || value > maximum_value)
      throw std::out_of_range((path + " is outside the supported range").toStdString());
    return value;
  };
  YAML::Node value;
  if (lookup_yaml_path(config, "stitching.max_output_width", &value)) {
    if (!value.IsNull() || !native_fallback_for_null_canonical)
      return read_node("stitching.max_output_width", value);
  }
  for (const char* path : {
           "pipeline.hmstitcher.properties.max-output-width",
           "pipeline.hmstitcher.properties.max_output_width",
           "pipeline.hmstitcher.properties.stitch-max-output-width",
           "pipeline.hmstitcher.properties.stitch_max_output_width",
           "pipeline.hmstitcher.private-properties.max-output-width",
           "pipeline.hmstitcher.private-properties.max_output_width",
           "pipeline.hmstitcher.private-properties.stitch-max-output-width",
           "pipeline.hmstitcher.private-properties.stitch_max_output_width",
       }) {
    if (lookup_yaml_path(config, QString::fromLatin1(path), &value)) {
      return read_node(QString::fromLatin1(path), value);
    }
  }
  return default_value;
}

bool has_conflicting_stitch_max_output_width_native_alias(YAML::Node config, int canonical_value, int maximum_value) {
  YAML::Node value;
  if (!lookup_yaml_path(config, "stitching.max_output_width", &value)) {
    return false;
  }
  for (const char* path : {
           "pipeline.hmstitcher.properties.max-output-width",
           "pipeline.hmstitcher.properties.max_output_width",
           "pipeline.hmstitcher.properties.stitch-max-output-width",
           "pipeline.hmstitcher.properties.stitch_max_output_width",
           "pipeline.hmstitcher.private-properties.max-output-width",
           "pipeline.hmstitcher.private-properties.max_output_width",
           "pipeline.hmstitcher.private-properties.stitch-max-output-width",
           "pipeline.hmstitcher.private-properties.stitch_max_output_width",
       }) {
    YAML::Node alias;
    if (!lookup_yaml_path(config, QString::fromLatin1(path), &alias)) {
      continue;
    }
    try {
      if (alias.IsNull()) {
        if (canonical_value != 0)
          return true;
        continue;
      }
      if (!alias.IsScalar())
        return true;
      const int alias_value = alias.as<int>();
      if (alias_value < 0 || alias_value > maximum_value || alias_value != canonical_value)
        return true;
    } catch (const std::exception&) {
      return true;
    }
  }
  return false;
}

bool generated_stitching_backend_choices_match_private(
    const YAML::Node& config,
    QString* previous_matcher = nullptr,
    QString* previous_backend = nullptr) {
  YAML::Node generated_matcher;
  YAML::Node generated_backend;
  YAML::Node private_matcher;
  YAML::Node private_backend;
  const bool matches =
      lookup_yaml_path(
          config, "hstream_ui.generated_stitching_backend_choices.control_point_matcher", &generated_matcher) &&
      generated_matcher.IsScalar() &&
      lookup_yaml_path(config, "hstream_ui.generated_stitching_backend_choices.mapping_backend", &generated_backend) &&
      generated_backend.IsScalar() && lookup_yaml_path(config, "stitching.control_point_matcher", &private_matcher) &&
      private_matcher.IsScalar() && lookup_yaml_path(config, "stitching.mapping_backend", &private_backend) &&
      private_backend.IsScalar() && private_matcher.as<std::string>() == generated_matcher.as<std::string>() &&
      private_backend.as<std::string>() == generated_backend.as<std::string>();
  if (!matches) {
    return false;
  }

  YAML::Node previous_matcher_node;
  YAML::Node previous_backend_node;
  if (previous_matcher &&
      lookup_yaml_path(
          config,
          "hstream_ui.generated_stitching_backend_choices.previous_control_point_matcher",
          &previous_matcher_node) &&
      previous_matcher_node.IsScalar()) {
    const auto canonical =
        canonical_control_point_matcher_choice(QString::fromStdString(previous_matcher_node.as<std::string>()));
    if (canonical.has_value()) {
      *previous_matcher = *canonical;
    }
  }
  if (previous_backend &&
      lookup_yaml_path(
          config, "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend", &previous_backend_node) &&
      previous_backend_node.IsScalar()) {
    const auto canonical =
        canonical_mapping_backend_choice(QString::fromStdString(previous_backend_node.as<std::string>()));
    if (canonical.has_value()) {
      *previous_backend = *canonical;
    }
  }
  return true;
}

bool read_stitch_frame_time(
    YAML::Node config,
    QString* normalized,
    bool* present = nullptr,
    const QString& fallback = QString::fromLatin1(kZeroStitchFrameTime)) {
  if (normalized) {
    *normalized = fallback;
  }
  YAML::Node node;
  const bool found = lookup_yaml_path(config, "stitching.stitch_frame_time", &node);
  if (present) {
    *present = found;
  }
  if (!found) {
    return true;
  }
  if (!node.IsScalar()) {
    return false;
  }
  try {
    const auto parsed = parse_stitch_frame_time(QString::fromStdString(node.as<std::string>()));
    if (!parsed.has_value()) {
      return false;
    }
    if (normalized) {
      *normalized = format_stitch_frame_time(*parsed);
    }
    return true;
  } catch (const YAML::Exception&) {
    return false;
  }
}

QStringList pipeline_config_files_from_args(const QStringList& pipeline_args) {
  QStringList config_files;
  for (int i = 0; i < pipeline_args.size(); ++i) {
    const QString arg = pipeline_args[i];
    if ((arg == "-c" || arg == "--config") && i + 1 < pipeline_args.size()) {
      config_files.push_back(pipeline_args[++i]);
    } else if (arg.startsWith("-c=") || arg.startsWith("--config=")) {
      config_files.push_back(arg.mid(arg.indexOf('=') + 1));
    }
  }
  return config_files;
}

QString resolve_config_path(const QString& path, const QString& base_dir) {
  const QFileInfo info(path);
  if (info.isAbsolute()) {
    return info.absoluteFilePath();
  }
  return QFileInfo(QDir(base_dir).filePath(path)).absoluteFilePath();
}

QStringList playtracker_config_candidates(
    const QString& configured,
    const QString& game_dir,
    const QString& working_dir) {
  const QString app_config_dir =
      QFileInfo(QDir(working_dir).filePath("configs/ds_hockey_app_config.yaml")).absolutePath();
  return {
      resolve_config_path(configured, app_config_dir),
      resolve_config_path(configured, working_dir),
      QDir(game_dir).filePath(configured),
      configured,
  };
}

bool same_file_path(const QString& lhs, const QString& rhs) {
  return QFileInfo(lhs).absoluteFilePath() == QFileInfo(rhs).absoluteFilePath();
}

bool is_ui_persistent_playtracker_config(const QString& path, const QString& game_dir) {
  const QFileInfo info(path);
  const QString runtime_dir = QFileInfo(QDir(game_dir).filePath(".hstream-ui")).absoluteFilePath();
  const QString filename = info.fileName();
  const bool owned_name = filename == "play_tracker_config.yaml" ||
      (filename.startsWith("play_tracker_config_") && filename.endsWith(".yaml"));
  return owned_name && info.absolutePath() == runtime_dir;
}

QString resolve_ui_persistent_playtracker_config(
    const YAML::Node& config,
    const QString& game_dir,
    const QString& working_dir) {
  YAML::Node configured_sidecar;
  if (!lookup_yaml_path(config, "pipeline.ds-playtracker.config-file", &configured_sidecar) ||
      !configured_sidecar.IsScalar()) {
    return {};
  }
  const QString configured = QString::fromStdString(configured_sidecar.as<std::string>());
  for (const QString& candidate : playtracker_config_candidates(configured, game_dir, working_dir)) {
    if (is_ui_persistent_playtracker_config(candidate, game_dir)) {
      return QFileInfo(candidate).absoluteFilePath();
    }
  }
  return {};
}

QString playtracker_sidecar_retirement_marker(const QString& sidecar) {
  const QFileInfo info(sidecar);
  return QDir(info.absolutePath()).filePath(".retired-" + info.fileName());
}

bool yaml_scalar_bool(YAML::Node node, bool default_value) {
  if (!node || !node.IsScalar()) {
    return default_value;
  }
  try {
    return node.as<bool>();
  } catch (const std::exception&) {
    const QString value = QString::fromStdString(node.as<std::string>()).trimmed().toLower();
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
      return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
      return false;
    }
  }
  return default_value;
}

struct ArtifactInvalidationResult {
  int invalidated = 0;
};

ArtifactInvalidationResult invalidate_rotation_dependent_artifacts(YAML::Node& config) {
  ArtifactInvalidationResult result;
  result.invalidated += remove_yaml_path(config, {"rink", "scoreboard", "perspective_polygon"}) ? 1 : 0;
  result.invalidated += remove_yaml_path(config, {"rink", "ice_contours_mask_count"}) ? 1 : 0;
  result.invalidated += remove_yaml_path(config, {"rink", "ice_contours_mask_centroid"}) ? 1 : 0;
  result.invalidated += remove_yaml_path(config, {"rink", "ice_contours_combined_bbox"}) ? 1 : 0;
  return result;
}

double ratio_x100(int value) {
  return static_cast<double>(std::max(1, value)) / 100.0;
}

} // namespace

void hm::ui_internal::restore_auto_selection_paths(YAML::Node& current, const YAML::Node& previous) {
  auto restore_child = [](YAML::Node current_parent, YAML::Node previous_parent, const char* key) {
    YAML::Node previous_value;
    if (lookup_yaml_key(previous_parent, key, &previous_value)) {
      current_parent[key] = YAML::Clone(previous_value);
    } else {
      remove_yaml_key(current_parent, key);
    }
  };

  restore_child(
      current["hstream_ui"]["video_roles"], map_value(map_value(previous, "hstream_ui"), "video_roles"), "left");
  restore_child(
      current["hstream_ui"]["video_roles"], map_value(map_value(previous, "hstream_ui"), "video_roles"), "center");
  restore_child(
      current["hstream_ui"]["video_roles"], map_value(map_value(previous, "hstream_ui"), "video_roles"), "right");
  restore_child(current["game"]["videos"], map_value(map_value(previous, "game"), "videos"), "left");
  restore_child(current["game"]["videos"], map_value(map_value(previous, "game"), "videos"), "right");
  restore_child(current["game"]["stitching"], map_value(map_value(previous, "game"), "stitching"), "frame_offsets");
  restore_child(current["stitching"], map_value(previous, "stitching"), "frame_offsets");
}

void hm::ui_internal::configure_application_identity() {
  QCoreApplication::setApplicationName("hstream-ui");
  QGuiApplication::setApplicationDisplayName("HStream");
  QGuiApplication::setDesktopFileName("hstream-ui");
}

QIcon hm::ui_internal::application_icon() {
  return hstream_application_icon();
}

bool hm::ui_internal::supports_x11_embedding(const QString& platform_name, bool tegra_runtime) {
#if defined(__x86_64__)
  return !tegra_runtime && platform_name.compare("xcb", Qt::CaseInsensitive) == 0;
#else
  (void)platform_name;
  (void)tegra_runtime;
  return false;
#endif
}

QString hm::ui_internal::preview_channel_for_tab(int tab_index, int camera_count) {
  if (tab_index == 0)
    return "program";
  if (tab_index == 1)
    return "stitched";
  const int source_index = tab_index - 2;
  return source_index >= 0 && source_index < camera_count ? QString("source%1").arg(source_index) : QString();
}

QString hm::ui_internal::matching_development_pipeline_runner(const QString& application_path) {
  const QString bazel_bin = matching_development_bazel_bin(application_path);
  if (bazel_bin.isEmpty())
    return {};
  const QString runner = QDir(bazel_bin).filePath("src/apps/pipeline-app/hstream-cli");
  return QFileInfo(runner).isExecutable() ? QFileInfo(runner).absoluteFilePath() : QString();
}

QString hm::ui_internal::matching_development_bazel_bin(const QString& application_path) {
  QFileInfo application_info(application_path);
  QString resolved_application = application_info.canonicalFilePath();
  if (resolved_application.isEmpty())
    resolved_application = application_info.absoluteFilePath();
  application_info.setFile(resolved_application);
  if (application_info.fileName() != "hstream-ui")
    return {};

  QDir application_dir = application_info.absoluteDir();
  if (application_dir.dirName() != "hstream-ui" || !application_dir.cdUp() || application_dir.dirName() != "apps" ||
      !application_dir.cdUp() || application_dir.dirName() != "src" || !application_dir.cdUp() ||
      application_dir.dirName() != "bin")
    return {};
  QDir configuration_dir = application_dir;
  if (!configuration_dir.cdUp() || configuration_dir.dirName().isEmpty() || !configuration_dir.cdUp() ||
      configuration_dir.dirName() != "bazel-out")
    return {};
  return application_dir.absolutePath();
}

QString hm::ui_internal::missing_development_runtime_artifact(const QString& bazel_bin_path) {
  const QDir bazel_bin(canonical_dir_path(bazel_bin_path));
  const QStringList required = {
      "src/gst-plugins/gst-dsxvideoconvert/libgstdsxvideoconvert.so",
      "src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so",
      "src/gst-plugins/gst-playtracker/libgstplaytracker.so",
      "src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so",
      "src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so",
  };
  for (const QString& relative_path : required) {
    const QString artifact = bazel_bin.filePath(relative_path);
    if (!QFileInfo(artifact).isFile())
      return artifact;
  }
  return {};
}

QString hm::ui_internal::development_runtime_root_for_application(const QString& application_path) {
  const QString bazel_bin_path = matching_development_bazel_bin(application_path);
  if (bazel_bin_path.isEmpty())
    return {};

  QFileInfo application_info(application_path);
  QString resolved_application = application_info.canonicalFilePath();
  if (resolved_application.isEmpty())
    resolved_application = application_info.absoluteFilePath();
  QDir candidate = QFileInfo(resolved_application).absoluteDir();
  while (true) {
    const QFileInfo workspace_marker(candidate.filePath("WORKSPACE.bazel"));
    const QFileInfo configs(candidate.filePath("configs"));
    if (workspace_marker.isFile() && configs.isDir()) {
      const QString canonical_marker = workspace_marker.canonicalFilePath();
      const QString source_root =
          canonical_marker.isEmpty() ? candidate.absolutePath() : QFileInfo(canonical_marker).absolutePath();
      if (QFileInfo(QDir(source_root).filePath("configs")).isDir())
        return source_root;
    }
    if (!candidate.cdUp())
      break;
  }
  QDir execroot(bazel_bin_path);
  if (!execroot.cdUp() || !execroot.cdUp() || !execroot.cdUp())
    return {};
  const QString canonical_source_directory = QFileInfo(execroot.filePath("src")).canonicalFilePath();
  if (canonical_source_directory.isEmpty())
    return {};
  const QDir source_root = QFileInfo(canonical_source_directory).dir();
  const QFileInfo workspace_marker(source_root.filePath("WORKSPACE.bazel"));
  if (workspace_marker.isFile() && QFileInfo(source_root.filePath("configs")).isDir())
    return source_root.absolutePath();
  return {};
}

HStreamWindow::HStreamWindow(QWidget* parent) : QMainWindow(parent) {
  loadBaselineDefaults();
  hm::ui_internal::configure_application_identity();
  const QString application_path = QCoreApplication::applicationFilePath();
  development_runtime_root_ = hm::ui_internal::development_runtime_root_for_application(application_path);
  development_pipeline_runner_ = hm::ui_internal::matching_development_pipeline_runner(application_path);
  development_bazel_bin_ = hm::ui_internal::matching_development_bazel_bin(application_path);
  setWindowIcon(hm::ui_internal::application_icon());
  capture_complete_log_ = qEnvironmentVariableIsSet("HSTREAM_UI_E2E_GAME_ID");
  pipeline_process_ = new QProcess(this);
  pipeline_process_->setProcessChannelMode(QProcess::MergedChannels);
  connect(pipeline_process_, &QProcess::started, this, [this]() { handlePipelineStarted(); });
  connect(
      pipeline_process_,
      QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
      this,
      [this](int exit_code, QProcess::ExitStatus exit_status) { handlePipelineFinished(exit_code, exit_status); });
  connect(pipeline_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    handlePipelineError(error);
  });
  connect(pipeline_process_, &QProcess::readyReadStandardOutput, this, [this]() { readPipelineOutput(); });
  connect(pipeline_process_, &QProcess::readyReadStandardError, this, [this]() { readPipelineOutput(); });
  archive_finalize_process_ = new QProcess(this);
  archive_finalize_process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(archive_finalize_process_, &QProcess::readyReadStandardOutput, this, [this]() {
    readArchiveFinalizationProgress();
  });
  connect(archive_finalize_process_, &QProcess::readyReadStandardError, this, [this]() {
    archive_finalize_error_output_ += QString::fromLocal8Bit(archive_finalize_process_->readAllStandardError());
    constexpr qsizetype kMaximumFinalizeErrorCharacters = 64 * 1024;
    if (archive_finalize_error_output_.size() > kMaximumFinalizeErrorCharacters)
      archive_finalize_error_output_ = archive_finalize_error_output_.right(kMaximumFinalizeErrorCharacters);
  });
  connect(
      archive_finalize_process_,
      QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
      this,
      [this](int exit_code, QProcess::ExitStatus exit_status) { finishArchiveFinalization(exit_code, exit_status); });
  connect(archive_finalize_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart)
      return;
    const QString process_error = archive_finalize_process_->errorString();
    if (archive_finalize_stage_ == ArchiveFinalizeStage::kSyncRecovery) {
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
#ifdef Q_OS_UNIX
      if (archive_finalize_recovery_log_fd_ >= 0)
        ::close(archive_finalize_recovery_log_fd_);
#endif
      archive_finalize_recovery_log_fd_ = -1;
      archive_finalize_recovery_log_device_ = 0;
      archive_finalize_recovery_log_inode_ = 0;
      releaseArchiveFinalizeSource(false);
      showArchiveFinalizationFailure(
          archive_finalize_pending_failure_detail_ +
          QString(
              "\n\nThe recovery file was renamed, but the durability helper could not start: %1 Do not start "
              "another archive run until this file has been copied to safety.")
              .arg(process_error));
    } else if (archive_finalize_stage_ == ArchiveFinalizeStage::kSyncCompleted) {
      failArchiveFinalization(QString("Could not start the archive durability helper: %1").arg(process_error));
    } else {
      failArchiveFinalization(QString("Could not start ffmpeg: %1").arg(process_error));
    }
  });
  buildUi();
  refreshGames();
  updateRunControls();
  appendLog(QString("hstream-ui started with hstream-cli runner backend=%1").arg(pipelineRunnerPath()));
}

void HStreamWindow::loadBaselineDefaults() {
  const auto loaded = hm::baseline_config::load();
  if (!loaded.ok())
    throw std::runtime_error(loaded.status().ToString());
  const auto user_overlay = hm::user_config::load_or_create();
  if (!user_overlay.ok())
    throw std::runtime_error(user_overlay.status().ToString());
  baseline_config_ = merge_yaml_maps(loaded->values, *user_overlay);
  baseline_config_root_ = QString::fromStdString(loaded->root.string());
  YAML::Node user_stitch_max_output_width;
  const bool user_clears_stitch_max_output_width =
      lookup_yaml_path(*user_overlay, "stitching.max_output_width", &user_stitch_max_output_width) &&
      user_stitch_max_output_width.IsNull();

  auto require = [this](const QString& path) {
    YAML::Node value;
    if (!lookup_yaml_path(baseline_config_, path, &value))
      throw std::runtime_error(QString("Bundled baseline is missing required UI default %1").arg(path).toStdString());
    return value;
  };
  auto integer = [&require](const QString& path) {
    const YAML::Node value = require(path);
    if (!value.IsScalar())
      throw std::runtime_error(QString("Bundled baseline UI default %1 must be a scalar").arg(path).toStdString());
    try {
      return value.as<int>();
    } catch (const YAML::Exception& error) {
      throw std::runtime_error(
          QString("Invalid integer UI default %1 in bundled baseline: %2").arg(path, error.what()).toStdString());
    }
  };
  auto boolean = [&require](const QString& path) {
    const YAML::Node value = require(path);
    if (!value.IsScalar())
      throw std::runtime_error(QString("Bundled baseline UI default %1 must be a scalar").arg(path).toStdString());
    try {
      return value.as<bool>() ? 1 : 0;
    } catch (const YAML::Exception& error) {
      throw std::runtime_error(
          QString("Invalid boolean UI default %1 in bundled baseline: %2").arg(path, error.what()).toStdString());
    }
  };
  auto round_to_int = [](const QString& path, double value) {
    if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
      throw std::runtime_error(
          QString("UI value %1 is non-finite or outside the integer range: %2").arg(path).arg(value).toStdString());
    }
    return static_cast<int>(std::lround(value));
  };
  auto scaled = [&require, &round_to_int](const QString& path, double scale) {
    const YAML::Node value = require(path);
    if (!value.IsScalar())
      throw std::runtime_error(QString("Bundled baseline UI default %1 must be a scalar").arg(path).toStdString());
    try {
      return round_to_int(path, value.as<double>() * scale);
    } catch (const YAML::Exception& error) {
      throw std::runtime_error(
          QString("Invalid numeric UI default %1 in bundled baseline: %2").arg(path, error.what()).toStdString());
    }
  };
  auto checked = [this](const QString& id, int value, int, int) { camera_defaults_[id] = value; };

  const YAML::Node stitch_frame_time = require("stitching.stitch_frame_time");
  if (!stitch_frame_time.IsScalar())
    throw std::runtime_error("Effective baseline stitching.stitch_frame_time must be a scalar");
  const auto parsed_stitch_frame_time =
      parse_stitch_frame_time(QString::fromStdString(stitch_frame_time.as<std::string>()));
  if (!parsed_stitch_frame_time.has_value())
    throw std::runtime_error("Effective baseline stitching.stitch_frame_time must be HH:MM:SS or HH:MM:SS.mmm");
  default_stitch_frame_time_ = format_stitch_frame_time(*parsed_stitch_frame_time);
  default_stitch_max_output_width_ = read_stitch_max_output_width_from_config(
      baseline_config_, 0, std::numeric_limits<int>::max(), !user_clears_stitch_max_output_width);
  YAML::Node control_point_matcher;
  if (lookup_yaml_path(baseline_config_, "stitching.control_point_matcher", &control_point_matcher) &&
      control_point_matcher.IsScalar()) {
    const QString configured = QString::fromStdString(control_point_matcher.as<std::string>());
    const auto canonical = canonical_control_point_matcher_choice(configured);
    if (canonical.has_value()) {
      default_control_point_matcher_ = *canonical;
    }
  }
  YAML::Node mapping_backend;
  if (lookup_yaml_path(baseline_config_, "stitching.mapping_backend", &mapping_backend) && mapping_backend.IsScalar()) {
    const QString configured = QString::fromStdString(mapping_backend.as<std::string>());
    const auto canonical = canonical_mapping_backend_choice(configured);
    if (!canonical.has_value())
      throw std::runtime_error("Effective baseline stitching.mapping_backend is not supported");
    default_mapping_backend_ = *canonical;
  }

  checked("Stop_Direction_Change_Delay_Frames", integer("rink.camera.stop_on_dir_change_delay"), 0, 60);
  checked("Cancel_Stop_On_Opposite_Direction", boolean("rink.camera.cancel_stop_on_opposite_dir"), 0, 1);
  checked("Stop_Cancel_Hysteresis_Frames", integer("rink.camera.stop_cancel_hysteresis_frames"), 0, 10);
  checked("Stop_Delay_Cooldown_Frames", integer("rink.camera.stop_delay_cooldown_frames"), 0, 30);
  checked("Time_To_Dest_Speed_Limit_Frames", integer("rink.camera.time_to_dest_speed_limit_frames"), 0, 120);
  checked("Zoom_In_Aggressiveness", integer("rink.camera.zoom_in_aggressiveness"), 0, 100);
  checked("Overshoot_Stop_Delay_Frames", integer("rink.camera.breakaway_detection.overshoot_stop_delay_count"), 0, 60);
  checked(
      "Post_Nonstop_Stop_Delay_Frames",
      integer("rink.camera.breakaway_detection.post_nonstop_stop_delay_count"),
      0,
      60);
  checked(
      "Overshoot_Speed_Ratio_x100",
      scaled("rink.camera.breakaway_detection.overshoot_scale_speed_ratio", 100.0),
      0,
      200);

  const YAML::Node rotation = require("stitching.post_stitch_rotate_degrees");
  int rotation_slider = 90;
  if (!rotation.IsNull()) {
    if (!rotation.IsScalar())
      throw std::runtime_error("Bundled baseline stitching.post_stitch_rotate_degrees must be null or numeric");
    try {
      rotation_slider = round_to_int("stitching.post_stitch_rotate_degrees", 90.0 - rotation.as<double>());
    } catch (const YAML::Exception& error) {
      throw std::runtime_error(
          std::string("Invalid stitching.post_stitch_rotate_degrees in bundled baseline: ") + error.what());
    }
  }
  checked("Stitch_Rotate_Degrees", rotation_slider, 0, 180);

  const YAML::Node fixed_rotation = require("rink.camera.fixed_edge_rotation_angle");
  try {
    if (fixed_rotation.IsNull()) {
      // An explicit null suppresses the canonical native-property mapping.
      // Show a neutral value without turning it into an explicit 0-degree
      // override when an unrelated preset field is saved.
      checked("Link_Fixed_Edge_Rotation_Left_Right", 1, 0, 1);
      checked("Left_Fixed_Edge_Rotation_Angle_x10", 0, 0, kFixedEdgeRotationMaximumX10);
      checked("Right_Fixed_Edge_Rotation_Angle_x10", 0, 0, kFixedEdgeRotationMaximumX10);
    } else if (fixed_rotation.IsSequence() && fixed_rotation.size() == 2) {
      checked("Link_Fixed_Edge_Rotation_Left_Right", 0, 0, 1);
      checked(
          "Left_Fixed_Edge_Rotation_Angle_x10",
          round_to_int("rink.camera.fixed_edge_rotation_angle[0]", fixed_rotation[0].as<double>() * 10.0),
          0,
          kFixedEdgeRotationMaximumX10);
      checked(
          "Right_Fixed_Edge_Rotation_Angle_x10",
          round_to_int("rink.camera.fixed_edge_rotation_angle[1]", fixed_rotation[1].as<double>() * 10.0),
          0,
          kFixedEdgeRotationMaximumX10);
    } else if (fixed_rotation.IsScalar()) {
      const int value = round_to_int("rink.camera.fixed_edge_rotation_angle", fixed_rotation.as<double>() * 10.0);
      checked("Link_Fixed_Edge_Rotation_Left_Right", 1, 0, 1);
      checked("Left_Fixed_Edge_Rotation_Angle_x10", value, 0, kFixedEdgeRotationMaximumX10);
      checked("Right_Fixed_Edge_Rotation_Angle_x10", value, 0, kFixedEdgeRotationMaximumX10);
    } else {
      throw std::runtime_error(
          "Effective baseline rink.camera.fixed_edge_rotation_angle must be null, numeric, or [left, right]");
    }
  } catch (const YAML::Exception& error) {
    throw std::runtime_error(
        std::string("Invalid rink.camera.fixed_edge_rotation_angle in bundled baseline: ") + error.what());
  }

  // These selectors describe where a UI edit is applied, rather than a
  // HockeyMOM config value. A zero speed/acceleration means no UI override, so
  // the effective values continue to come from the baseline-backed pipeline.
  camera_defaults_["Apply_To_Fast_Box"] = 0;
  camera_defaults_["Apply_To_Follower_Box"] = 1;
  camera_defaults_["Max_Speed_X_x10"] = 0;
  camera_defaults_["Max_Speed_Y_x10"] = 0;
  camera_defaults_["Max_Accel_X_x10"] = 0;
  camera_defaults_["Max_Accel_Y_x10"] = 0;
  camera_defaults_["Lift_Shadow_Black_Point"] = 0;
  camera_defaults_["Exposure_x100"] = 0;
  camera_defaults_["Use_10_Bit_Grading"] = 0;
}

HStreamWindow::~HStreamWindow() {
  finishArchiveJobLog();
  releaseArchiveFinalizeSource(false);
  releaseArchiveFinalizerOwnership(false);
}

bool HStreamWindow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == stitch_frame_time_edit_ && event) {
    if (event->type() == QEvent::FocusIn) {
      stitch_frame_time_edit_->setDisplayFormat(kStitchFrameTimeFractionalFormat);
    } else if (event->type() == QEvent::FocusOut) {
      QTimer::singleShot(0, this, [this] {
        if (stitch_frame_time_edit_ && !stitch_frame_time_edit_->hasFocus() &&
            stitch_frame_time_edit_->time().msec() == 0) {
          stitch_frame_time_edit_->setDisplayFormat(kStitchFrameTimeFormat);
        }
      });
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void HStreamWindow::closeEvent(QCloseEvent* event) {
  if (!event) {
    return;
  }
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("window close requested; stopping pipeline before exit");
    stopPipeline();
    if (pipeline_process_->state() != QProcess::NotRunning) {
      appendLog("window close deferred because the pipeline is still running");
      event->ignore();
      return;
    }
  }
  if (archive_finalize_process_ && archive_finalize_process_->state() != QProcess::NotRunning) {
    appendLog("window close deferred while the completed archive is being finalized");
    if (archive_finalize_dialog_) {
      archive_finalize_dialog_->show();
      archive_finalize_dialog_->raise();
    }
    event->ignore();
    return;
  }
  QMainWindow::closeEvent(event);
}

QString HStreamWindow::pipelineStateText() const {
  return pipeline_state_ ? pipeline_state_->text() : QString();
}

QString HStreamWindow::outputStateText(const QString& id) const {
  const auto it = output_states_.find(id);
  return it == output_states_.end() ? QString() : it->second->text();
}

QString HStreamWindow::logText() const {
  return log_ ? log_->toPlainText() : QString();
}

QString HStreamWindow::completeLogText() const {
  return complete_log_;
}

QString HStreamWindow::scoreboardSelectorUrl() const {
  return scoreboard_selector_url_;
}

QString HStreamWindow::gameIdText() const {
  return game_id_edit_ ? game_id_edit_->text() : QString();
}

QString HStreamWindow::gameDirectoryText() const {
  return game_path_label_ ? game_path_label_->text() : QString();
}

int HStreamWindow::videoSetCount() const {
  return video_set_list_ ? video_set_list_->count() : 0;
}

int HStreamWindow::cameraControlValue(const QString& id) const {
  const auto slider = camera_sliders_.find(id);
  if (slider != camera_sliders_.end() && slider->second) {
    return slider->second->value();
  }
  const auto checkbox = camera_checkboxes_.find(id);
  return checkbox != camera_checkboxes_.end() && checkbox->second && checkbox->second->isChecked() ? 1 : 0;
}

int HStreamWindow::cameraTabCount() const {
  return (program_control_tabs_ ? program_control_tabs_->count() : 0) +
      (stitched_control_tabs_ ? stitched_control_tabs_->count() : 0);
}

void HStreamWindow::buildUi() {
  setObjectName("hstreamUi");
  setWindowTitle("HStream UI");
  resize(1440, 900);

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(10);

  top_bar_ = new QWidget(central);
  top_bar_->setObjectName("topBarPanel");
  auto* top_bar_layout = new QVBoxLayout(top_bar_);
  top_bar_layout->setContentsMargins(0, 0, 0, 0);
  buildTopBar(top_bar_layout);
  root->addWidget(top_bar_);

  playback_progress_ = new QProgressBar(central);
  playback_progress_->setObjectName("playbackProgress");
  playback_progress_->setAccessibleName("Playback progress");
  playback_progress_->setRange(0, 0);
  playback_progress_->setTextVisible(true);
  playback_progress_->setFixedHeight(18);
  playback_progress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  playback_progress_->setStyleSheet(
      "QProgressBar { background: #20252d; border: 1px solid #596273; border-radius: 4px; color: white; "
      "font-weight: 600; text-align: center; }"
      "QProgressBar::chunk { background: #238636; border-radius: 3px; }"
      "QProgressBar[playbackState=\"completed\"] { background: #162b48; border-color: #58a6ff; }"
      "QProgressBar[playbackState=\"completed\"]::chunk { background: #1f6feb; }"
      "QProgressBar[playbackState=\"error\"] { background: #5a1a1a; border-color: #ff7b72; }"
      "QProgressBar[playbackState=\"error\"]::chunk { background: #da3633; }");
  playback_progress_->hide();
  resetPlaybackProgress(false);

  auto* playback_transport = new QWidget(central);
  playback_transport->setObjectName("playbackTransport");
  auto* playback_transport_layout = new QHBoxLayout(playback_transport);
  playback_transport_layout->setContentsMargins(0, 0, 0, 0);
  playback_transport_layout->setSpacing(10);
  playback_transport_layout->addWidget(playback_progress_, 1);

  playback_seek_controls_ = new QWidget(playback_transport);
  playback_seek_controls_->setObjectName("playbackSeekControls");
  auto* seek_layout = new QHBoxLayout(playback_seek_controls_);
  seek_layout->setContentsMargins(0, 0, 0, 0);
  seek_layout->setSpacing(8);
  playback_seek_back_button_ = new QPushButton("−10s", playback_seek_controls_);
  playback_seek_back_button_->setObjectName("playbackSeekBack10Button");
  playback_seek_slider_ = new WheelPassthroughSlider(Qt::Horizontal, playback_seek_controls_);
  playback_seek_slider_->setObjectName("playbackSeekSlider");
  playback_seek_slider_->setAccessibleName("Video playback position");
  playback_seek_slider_->setRange(0, 100000);
  playback_seek_forward_button_ = new QPushButton("+10s", playback_seek_controls_);
  playback_seek_forward_button_->setObjectName("playbackSeekForward10Button");
  playback_seek_position_ = new QLabel("00:00:00 / --:--:--", playback_seek_controls_);
  playback_seek_position_->setObjectName("playbackSeekPosition");
  playback_seek_position_->setMinimumWidth(132);
  playback_seek_position_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  seek_layout->addWidget(playback_seek_back_button_);
  seek_layout->addWidget(playback_seek_slider_, 1);
  seek_layout->addWidget(playback_seek_forward_button_);
  seek_layout->addWidget(playback_seek_position_);
  connect(playback_seek_back_button_, &QPushButton::clicked, this, [this]() {
    requestPlaybackSeekRelative(-10LL * 1000000000LL);
  });
  connect(playback_seek_forward_button_, &QPushButton::clicked, this, [this]() {
    requestPlaybackSeekRelative(10LL * 1000000000LL);
  });
  connect(playback_seek_slider_, &QSlider::sliderReleased, this, [this]() {
    if (!playback_seek_slider_ || playback_duration_ns_ <= 0)
      return;
    const long double fraction = static_cast<long double>(playback_seek_slider_->value()) /
        static_cast<long double>(playback_seek_slider_->maximum());
    requestPlaybackSeek(static_cast<qint64>(fraction * static_cast<long double>(playback_duration_ns_)));
  });
  playback_transport_layout->addWidget(playback_seek_controls_, 2);
  root->addWidget(playback_transport);
  updatePlaybackSeekControls();

  main_log_splitter_ = new QSplitter(Qt::Vertical);
  main_log_splitter_->setObjectName("mainLogSplitter");
  main_log_splitter_->setChildrenCollapsible(false);

  setup_panel_ = new QWidget();
  setup_panel_->setObjectName("setupPanel");
  auto* main_layout = new QVBoxLayout(setup_panel_);
  main_layout->setContentsMargins(0, 0, 0, 0);
  buildMainArea(main_layout);

  log_panel_ = new QWidget();
  log_panel_->setObjectName("logPanel");
  auto* log_layout = new QVBoxLayout(log_panel_);
  log_layout->setContentsMargins(0, 0, 0, 0);
  buildLog(log_layout);

  main_log_splitter_->addWidget(setup_panel_);
  main_log_splitter_->addWidget(log_panel_);
  main_log_splitter_->setStretchFactor(0, 4);
  main_log_splitter_->setStretchFactor(1, 1);
  main_log_splitter_->setSizes({680, 170});
  root->addWidget(main_log_splitter_, 1);

  setCentralWidget(central);
  configureControlHelp();
  captureSavedControlState();
}

void HStreamWindow::buildTopBar(QVBoxLayout* root) {
  auto* status_bar = new QHBoxLayout();
  status_bar->setSpacing(4);

  auto* title = new QLabel("HStream Runtime Control");
  title->setObjectName("titleLabel");
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 2);
  title->setFont(title_font);

  pipeline_state_ = make_value_label("pipelineStateLabel", "STOPPED");
  backend_mode_ = new QLabel("Backend: hstream-cli");
  backend_mode_->setObjectName("backendModeLabel");

  run_mode_selector_ = new QComboBox();
  run_mode_selector_->setObjectName("runModeCombo");
  run_mode_selector_->addItem("Program", "program");
  run_mode_selector_->addItem("Stitching Calibration", "stitch-calibration");
  connect(run_mode_selector_, &QComboBox::currentIndexChanged, this, [this]() {
    if (control_points_spin_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      control_points_spin_->setEnabled(!running);
    }
    if (calibration_frame_count_spin_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      calibration_frame_count_spin_->setEnabled(!running);
    }
    if (control_point_matcher_combo_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      control_point_matcher_combo_->setEnabled(!running);
    }
    if (mapping_backend_combo_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      mapping_backend_combo_->setEnabled(!running);
    }
    if (stitch_max_output_width_spin_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      stitch_max_output_width_spin_->setEnabled(!running);
    }
    if (stitch_frame_time_edit_) {
      const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
      stitch_frame_time_edit_->setEnabled(!running);
    }
    if (stitched_status_ && isCalibrationRun()) {
      stitched_status_->setText("Stitching calibration preview");
    }
    if (preview_tabs_) {
      const bool calibration = isCalibrationRun();
      preview_tabs_->setTabEnabled(0, !calibration);
      preview_tabs_->setTabToolTip(
          0,
          calibration ? "Program processing is omitted from the stitching-only calibration pipeline."
                      : "Final Program output after detection, tracking, cropping, and overlays.");
      if (calibration)
        preview_tabs_->setCurrentIndex(1);
    }
    const bool overlays_available = !isCalibrationRun() && render_video_toggle_ && render_video_toggle_->isChecked();
    for (QCheckBox* toggle : {show_player_tracking_toggle_, show_play_tracking_toggle_, show_rink_mask_toggle_}) {
      if (toggle)
        toggle->setEnabled(overlays_available);
    }
  });

  control_points_spin_ = new QSpinBox();
  control_points_spin_->setObjectName("controlPointsSpin");
  control_points_spin_->setRange(20, 5000);
  control_points_spin_->setSingleStep(25);
  control_points_spin_->setValue(kDefaultStitchCalibrationControlPoints);
  control_points_spin_->setEnabled(true);
  control_points_spin_->setPrefix("CP ");
  control_points_spin_->setFixedWidth(64);
  control_points_spin_->setToolTip(
      "Control-point limit for stitching calibration. Changing this in Program mode recalibrates stitching before "
      "the full pipeline continues.");
  connect(control_points_spin_, &QSpinBox::valueChanged, this, [this]() { updatePresetDirtyState(); });

  calibration_frame_count_spin_ = new QSpinBox();
  calibration_frame_count_spin_->setObjectName("calibrationFrameCountSpin");
  calibration_frame_count_spin_->setRange(1, 16);
  calibration_frame_count_spin_->setSingleStep(1);
  calibration_frame_count_spin_->setValue(kDefaultStitchCalibrationFrameCount);
  calibration_frame_count_spin_->setEnabled(true);
  calibration_frame_count_spin_->setPrefix("Frames ");
  calibration_frame_count_spin_->setFixedWidth(86);
  calibration_frame_count_spin_->setToolTip(
      "Synchronized frame pairs to use for stitching calibration. Changing this captures new calibration inputs.");
  connect(calibration_frame_count_spin_, &QSpinBox::valueChanged, this, [this]() { updatePresetDirtyState(); });

  stitch_max_output_width_spin_ = new QSpinBox();
  stitch_max_output_width_spin_->setObjectName("stitchMaxOutputWidthSpin");
  stitch_max_output_width_spin_->setRange(0, std::numeric_limits<int>::max());
  stitch_max_output_width_spin_->setSingleStep(256);
  stitch_max_output_width_spin_->setSpecialValueText("Auto");
  stitch_max_output_width_spin_->setValue(default_stitch_max_output_width_);
  set_control_help(
      stitch_max_output_width_spin_,
      "Maximum stitched canvas width. Auto keeps the native mapping size; lower values reduce stitching memory.");
  connect(stitch_max_output_width_spin_, &QSpinBox::valueChanged, this, [this]() { updatePresetDirtyState(); });

  control_point_matcher_combo_ = new QComboBox();
  control_point_matcher_combo_->setObjectName("controlPointMatcherCombo");
  control_point_matcher_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  control_point_matcher_combo_->setMinimumContentsLength(20);
  control_point_matcher_combo_->setMinimumWidth(240);
  control_point_matcher_combo_->addItem("SuperPoint + LightGlue", "superpoint-lightglue");
  control_point_matcher_combo_->addItem("DeDoDe + LightGlue", "dedode-lightglue");
  control_point_matcher_combo_->addItem("LoFTR", "loftr");
  if (auto* matcher_model = qobject_cast<QStandardItemModel*>(control_point_matcher_combo_->model())) {
    for (int index = 1; index < control_point_matcher_combo_->count(); ++index) {
      if (auto* item = matcher_model->item(index)) {
        item->setEnabled(false);
      }
    }
  }
  int matcher_index = control_point_matcher_combo_->findData(default_control_point_matcher_);
  control_point_matcher_combo_->setCurrentIndex(matcher_index < 0 ? 0 : matcher_index);
  control_point_matcher_combo_->setToolTip("Native feature matcher used to find stitching control points.");
  connect(control_point_matcher_combo_, &QComboBox::currentIndexChanged, this, [this]() { updatePresetDirtyState(); });

  mapping_backend_combo_ = new QComboBox();
  mapping_backend_combo_->setObjectName("mappingBackendCombo");
  mapping_backend_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  mapping_backend_combo_->setMinimumContentsLength(12);
  mapping_backend_combo_->setMinimumWidth(220);
  mapping_backend_combo_->addItem("NONA", "nona");
  mapping_backend_combo_->addItem("MAGSAC++", "opencv-magsac");
  mapping_backend_combo_->addItem("RANSAC", "opencv-affine-ransac");
  int mapping_index = mapping_backend_combo_->findData(default_mapping_backend_);
  mapping_backend_combo_->setCurrentIndex(mapping_index < 0 ? 0 : mapping_index);
  mapping_backend_combo_->setToolTip("Mapping TIFF generator used after control points are selected.");
  connect(mapping_backend_combo_, &QComboBox::currentIndexChanged, this, [this]() { updatePresetDirtyState(); });

  stitch_frame_time_edit_ = new QTimeEdit();
  stitch_frame_time_edit_->setObjectName("stitchFrameTimeEdit");
  stitch_frame_time_edit_->setFixedWidth(70);
  stitch_frame_time_edit_->setDisplayFormat(kStitchFrameTimeFormat);
  stitch_frame_time_edit_->setTime(*parse_stitch_frame_time(default_stitch_frame_time_));
  stitch_frame_time_edit_->setWrapping(false);
  stitch_frame_time_edit_->installEventFilter(this);
  stitch_frame_time_edit_->setToolTip(
      "Frame timestamp used to calibrate stitching. Playback returns to the beginning after one-pass calibration.");
  connect(stitch_frame_time_edit_, &QTimeEdit::timeChanged, this, [this](const QTime& value) {
    stitch_frame_time_edit_->setDisplayFormat(
        value.msec() == 0 && !stitch_frame_time_edit_->hasFocus() ? kStitchFrameTimeFormat
                                                                  : kStitchFrameTimeFractionalFormat);
    updatePresetDirtyState();
  });

  render_video_toggle_ = new QCheckBox("Render video");
  render_video_toggle_->setObjectName("renderVideoCheck");
  render_video_toggle_->setChecked(true);
  render_video_toggle_->setToolTip(
      "Show the active GPU preview and play local monitor audio; this can be changed while the pipeline is running");
  connect(render_video_toggle_, &QCheckBox::toggled, this, [this](bool enabled) {
    for (QCheckBox* toggle : {show_player_tracking_toggle_, show_play_tracking_toggle_, show_rink_mask_toggle_}) {
      if (toggle)
        toggle->setEnabled(enabled && !isCalibrationRun());
    }
    setRuntimeVideoRendering(enabled);
  });

  show_player_tracking_toggle_ = new QCheckBox("Player boxes");
  show_player_tracking_toggle_->setObjectName("showPlayerTrackingCheck");
  show_player_tracking_toggle_->setToolTip(
      "Draw tracked-player boxes on the Program and Stitched GPU previews without altering encoded output");
  show_play_tracking_toggle_ = new QCheckBox("Play tracking");
  show_play_tracking_toggle_->setObjectName("showPlayTrackingCheck");
  show_play_tracking_toggle_->setToolTip(
      "Draw play-tracker camera boxes, thresholds, and state geometry on both GPU previews");
  show_rink_mask_toggle_ = new QCheckBox("Rink mask");
  show_rink_mask_toggle_->setObjectName("showRinkMaskCheck");
  show_rink_mask_toggle_->setToolTip("Composite the saved ice-rink mask as translucent green on both GPU previews");
  for (QCheckBox* toggle : {show_player_tracking_toggle_, show_play_tracking_toggle_, show_rink_mask_toggle_}) {
    toggle->setChecked(false);
    connect(toggle, &QCheckBox::toggled, this, [this]() { setRuntimePreviewOverlays(); });
  }

  drivegpt_csv_toggle_ = new QCheckBox("DriveGPT CSV");
  drivegpt_csv_toggle_->setObjectName("drivegptCsvCheck");
  drivegpt_csv_toggle_->setChecked(false);
  drivegpt_csv_toggle_->setToolTip(
      "Save HM-compatible tracking.csv, camera.csv, and camera_fast.csv policy metadata for this Program run");

  start_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), "Play");
  start_button_->setObjectName("startPipelineButton");
  pause_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), "Pause");
  pause_button_->setObjectName("pausePipelineButton");
  auto* restart = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), "Restart Stage");
  restart->setObjectName("restartStageButton");
  save_preset_button_ = new QPushButton(style()->standardIcon(QStyle::SP_DialogSaveButton), "Save Preset");
  save_preset_button_->setObjectName("savePresetButton");
  auto* reset = new QPushButton("Reset Camera");
  reset->setObjectName("resetCameraButton");
  stop_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), "Stop");
  stop_button_->setObjectName("stopPipelineButton");

  connect(start_button_, &QPushButton::clicked, this, [this]() { startPipeline(); });
  connect(pause_button_, &QPushButton::clicked, this, [this]() { pauseOrResumePipeline(); });
  connect(stop_button_, &QPushButton::clicked, this, [this]() { stopPipeline(); });
  connect(restart, &QPushButton::clicked, this, [this]() { restartStage(); });
  connect(save_preset_button_, &QPushButton::clicked, this, [this]() { savePreset(); });
  connect(reset, &QPushButton::clicked, this, [this]() { resetCameraControls(); });

  status_bar->addWidget(title);
  status_bar->addSpacing(16);
  status_bar->addWidget(new QLabel("Pipeline:"));
  status_bar->addWidget(pipeline_state_);
  status_bar->addWidget(backend_mode_);
  status_bar->addStretch(1);
  status_bar->addWidget(run_mode_selector_);
  status_bar->addWidget(control_points_spin_);
  status_bar->addWidget(calibration_frame_count_spin_);
  status_bar->addWidget(stitch_frame_time_edit_);
  status_bar->addWidget(render_video_toggle_);
  status_bar->addWidget(show_player_tracking_toggle_);
  status_bar->addWidget(show_play_tracking_toggle_);
  status_bar->addWidget(show_rink_mask_toggle_);
  status_bar->addWidget(drivegpt_csv_toggle_);

  auto* action_bar = new QHBoxLayout();
  action_bar->setSpacing(8);
  action_bar->addStretch(1);
  action_bar->addWidget(start_button_);
  action_bar->addWidget(pause_button_);
  action_bar->addWidget(restart);
  action_bar->addWidget(save_preset_button_);
  action_bar->addWidget(reset);
  action_bar->addWidget(stop_button_);
  root->addLayout(status_bar);
  root->addLayout(action_bar);
}

void HStreamWindow::buildMainArea(QVBoxLayout* root) {
  auto* setup_row = new QWidget();
  setup_row->setObjectName("setupControlsRow");
  auto* setup_layout = new QHBoxLayout(setup_row);
  setup_layout->setContentsMargins(0, 0, 0, 0);
  auto* game_column = new QWidget();
  auto* game_layout = new QVBoxLayout(game_column);
  game_layout->setContentsMargins(0, 0, 0, 0);
  buildGameControls(game_layout);
  auto* output_column = new QWidget();
  output_column->setMaximumWidth(360);
  auto* output_layout = new QVBoxLayout(output_column);
  output_layout->setContentsMargins(0, 0, 0, 0);
  buildOutputControls(output_layout);
  setup_layout->addWidget(game_column, 1);
  setup_layout->addWidget(output_column);

  setup_preview_splitter_ = new QSplitter(Qt::Vertical);
  setup_preview_splitter_->setObjectName("setupPreviewSplitter");
  setup_preview_splitter_->setChildrenCollapsible(true);
  setup_preview_splitter_->addWidget(setup_row);
  auto* preview_container = new QWidget();
  auto* preview_layout = new QVBoxLayout(preview_container);
  preview_layout->setContentsMargins(0, 0, 0, 0);
  buildPreviewPane(preview_layout);
  setup_preview_splitter_->addWidget(preview_container);
  setup_preview_splitter_->setStretchFactor(0, 0);
  setup_preview_splitter_->setStretchFactor(1, 1);
  setup_preview_splitter_->setCollapsible(0, true);
  setup_preview_splitter_->setCollapsible(1, false);
  setup_preview_splitter_->setSizes({240, 440});
  root->addWidget(setup_preview_splitter_, 1);
}

void HStreamWindow::buildGameControls(QVBoxLayout* root) {
  auto* group = new QGroupBox("Game");
  game_controls_ = group;
  group->setObjectName("gameSetupGroup");
  auto* layout = new QGridLayout(group);
  layout->setColumnStretch(1, 1);

  game_selector_ = new QComboBox();
  game_selector_->setObjectName("gameSelector");
  connect(game_selector_, &QComboBox::currentTextChanged, this, [this](const QString& game_id) {
    if (!game_id.isEmpty() && game_id != game_id_edit_->text()) {
      selectGame(game_id);
    }
  });

  game_id_edit_ = new QLineEdit();
  game_id_edit_->setObjectName("gameIdEdit");
  game_id_edit_->setPlaceholderText("game-id");
  connect(game_id_edit_, &QLineEdit::textChanged, this, [this]() {
    updateArchiveOutputPathLabel();
    updatePresetDirtyState();
  });
  connect(game_id_edit_, &QLineEdit::editingFinished, this, [this]() {
    game_id_edit_->setText(sanitized_game_id(game_id_edit_->text()));
    refreshVideoSets();
  });

  auto* create = new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton), "Create / Load");
  create->setObjectName("createGameButton");
  connect(create, &QPushButton::clicked, this, [this]() { createOrLoadGame(); });

  auto* refresh = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), "Refresh");
  refresh->setObjectName("refreshGamesButton");
  connect(refresh, &QPushButton::clicked, this, [this]() { refreshGames(); });

  game_path_label_ = new QLabel(gameRoot());
  game_path_label_->setObjectName("gamePathLabel");
  game_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  layout->addWidget(new QLabel("Existing"), 0, 0);
  layout->addWidget(game_selector_, 0, 1);
  layout->addWidget(refresh, 0, 2);
  layout->addWidget(new QLabel("Game ID"), 1, 0);
  layout->addWidget(game_id_edit_, 1, 1);
  layout->addWidget(create, 1, 2);
  layout->addWidget(new QLabel("Path"), 2, 0);
  layout->addWidget(game_path_label_, 2, 1, 1, 2);

  auto* video_group = new QGroupBox("Video Sets");
  video_controls_ = video_group;
  video_group->setObjectName("videoSetsGroup");
  auto* video_layout = new QGridLayout(video_group);
  video_layout->setColumnStretch(0, 1);

  video_path_edit_ = new QLineEdit();
  video_path_edit_->setObjectName("videoPathEdit");
  video_path_edit_->setPlaceholderText("/path/to/video.mp4");

  auto* browse = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), "Browse");
  browse->setObjectName("browseVideoButton");
  connect(browse, &QPushButton::clicked, this, [this]() { browseVideoPath(); });

  auto* add = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogNewFolder), "Add");
  add->setObjectName("addVideoButton");
  connect(add, &QPushButton::clicked, this, [this]() { addVideoPath(); });

  auto* remove = new QPushButton(style()->standardIcon(QStyle::SP_TrashIcon), "Remove");
  remove->setObjectName("removeVideoButton");
  connect(remove, &QPushButton::clicked, this, [this]() { removeSelectedVideoSet(); });

  role_auto_ = new QRadioButton("Auto");
  role_auto_->setObjectName("videoRole_auto");
  role_auto_->setChecked(true);
  role_left_ = new QRadioButton("Left");
  role_left_->setObjectName("videoRole_left");
  role_center_ = new QRadioButton("Center");
  role_center_->setObjectName("videoRole_center");
  role_right_ = new QRadioButton("Right");
  role_right_->setObjectName("videoRole_right");

  auto* role_group = new QButtonGroup(video_group);
  role_group->addButton(role_auto_);
  role_group->addButton(role_left_);
  role_group->addButton(role_center_);
  role_group->addButton(role_right_);

  auto* roles = new QHBoxLayout();
  roles->setSpacing(12);
  roles->addWidget(role_auto_);
  roles->addWidget(role_left_);
  roles->addWidget(role_center_);
  roles->addWidget(role_right_);
  roles->addStretch(1);

  video_set_list_ = new QListWidget();
  video_set_list_->setObjectName("videoSetList");
  video_set_list_->setMinimumHeight(48);

  video_sets_path_label_ = new QLabel(gameRoot());
  video_sets_path_label_->setObjectName("videoSetsPathLabel");
  video_sets_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  video_sets_path_label_->setEnabled(false);

  video_layout->addWidget(video_path_edit_, 0, 0);
  video_layout->addWidget(browse, 0, 1);
  video_layout->addWidget(add, 0, 2);
  video_layout->addLayout(roles, 1, 0, 1, 3);
  video_layout->addWidget(new QLabel("Game path"), 2, 0, 1, 1);
  video_layout->addWidget(video_sets_path_label_, 2, 1, 1, 2);
  video_layout->addWidget(video_set_list_, 3, 0, 1, 2);
  video_layout->addWidget(remove, 3, 2);

  layout->addWidget(video_group, 3, 0, 1, 3);
  root->addWidget(group);
}

void HStreamWindow::buildPreviewPane(QVBoxLayout* root) {
  preview_tabs_ = new QTabWidget();
  preview_tabs_->setObjectName("previewTabs");

  auto configure_host = [this](LetterboxRenderHost* host, int tab_index, const QString& button_name) {
    host->focusButton()->setObjectName(button_name);
    host->setFocusToggleCallback([this, tab_index]() { togglePreviewFocus(tab_index); });
    preview_hosts_.push_back(host);
  };
  auto add_controls_drawer =
      [this](QVBoxLayout* page_layout, QWidget* controls, const QString& label, const QString& object_name) {
        auto* toggle = new QToolButton();
        toggle->setObjectName(object_name);
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setArrowType(Qt::DownArrow);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setText(label);
        toggle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        set_control_help(
            toggle,
            "Show or hide the controls associated with this video stage to trade control space for a larger preview.");
        connect(toggle, &QToolButton::toggled, controls, [toggle, controls](bool expanded) {
          controls->setVisible(expanded);
          toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
          set_control_help(
              toggle,
              expanded ? "Hide the controls associated with this video stage to give the preview more space."
                       : "Show the controls associated with this video stage so their changes can be viewed live.");
        });
        associated_control_toggles_.push_back(toggle);
        associated_control_panels_.push_back(controls);
        page_layout->addWidget(toggle);
        page_layout->addWidget(controls);
      };

  auto* program = new QWidget();
  auto* layout = new QVBoxLayout(program);
  auto* preview_host = new LetterboxRenderHost(16.0 / 9.0);
  preview_host->setObjectName("programLetterboxHost");
  configure_host(preview_host, 0, "programFocusButton");
  preview_surface_ = preview_host->renderSurface();
  preview_surface_->setObjectName("previewSurface");
  preview_render_target_ = preview_host->renderTarget();
  preview_render_target_->setObjectName("previewRenderTarget");
  preview_external_notice_ = new QLabel("Video is displayed in a separate DeepStream window", preview_host);
  preview_external_notice_->setObjectName("programExternalRenderNotice");
  preview_external_notice_->setAlignment(Qt::AlignCenter);
  preview_external_notice_->setWordWrap(true);
  preview_external_notice_->setStyleSheet("color: #c9d1d9; padding: 24px;");
  preview_external_notice_->hide();
  auto* preview_notice_layout = new QVBoxLayout(preview_host);
  preview_notice_layout->setContentsMargins(0, 0, 0, 0);
  preview_notice_layout->addWidget(preview_external_notice_);

  preview_status_ = new QLabel("Pipeline stopped");
  preview_status_->setObjectName("previewStatusLabel");
  auto* program_footer = new QHBoxLayout();
  program_footer->addWidget(preview_status_, 1);
  layout->addWidget(preview_host, 1);
  layout->addLayout(program_footer);
  auto* program_controls = new QWidget();
  program_controls->setObjectName("programAssociatedControls");
  auto* program_controls_layout = new QVBoxLayout(program_controls);
  program_controls_layout->setContentsMargins(0, 0, 0, 0);
  buildCameraControls(program_controls_layout, true);
  add_controls_drawer(layout, program_controls, "Program Controls", "programControlsToggle");

  auto* stitched = new QWidget();
  auto* stitched_layout = new QVBoxLayout(stitched);
  auto* stitched_host = new LetterboxRenderHost(16.0 / 9.0);
  stitched_host->setObjectName("stitchedLetterboxHost");
  configure_host(stitched_host, 1, "stitchedFocusButton");
  stitched_surface_ = stitched_host->renderSurface();
  stitched_surface_->setObjectName("stitchedPreviewSurface");
  stitched_render_target_ = stitched_host->renderTarget();
  stitched_render_target_->setObjectName("stitchedPreviewRenderTarget");
  stitched_external_notice_ = new QLabel("Video is displayed in a separate DeepStream window", stitched_host);
  stitched_external_notice_->setObjectName("stitchedExternalRenderNotice");
  stitched_external_notice_->setAlignment(Qt::AlignCenter);
  stitched_external_notice_->setWordWrap(true);
  stitched_external_notice_->setStyleSheet("color: #c9d1d9; padding: 24px;");
  stitched_external_notice_->hide();
  auto* stitched_notice_layout = new QVBoxLayout(stitched_host);
  stitched_notice_layout->setContentsMargins(0, 0, 0, 0);
  stitched_notice_layout->addWidget(stitched_external_notice_);
  stitched_status_ = new QLabel("Stitched canvas preview");
  stitched_status_->setObjectName("stitchedPreviewStatusLabel");
  auto* stitched_footer = new QHBoxLayout();
  stitched_footer->addWidget(stitched_status_, 1);
  stitched_layout->addWidget(stitched_host, 1);
  stitched_layout->addLayout(stitched_footer);
  auto* stitched_controls = new QWidget();
  stitched_controls->setObjectName("stitchedAssociatedControls");
  auto* stitched_controls_layout = new QVBoxLayout(stitched_controls);
  stitched_controls_layout->setContentsMargins(0, 0, 0, 0);
  buildCameraControls(stitched_controls_layout, false);
  add_controls_drawer(stitched_layout, stitched_controls, "Stitched Controls", "stitchedControlsToggle");

  preview_tabs_->addTab(program, "Program");
  preview_tabs_->addTab(stitched, "Stitched");
  for (int camera_index = 0; camera_index < 3; ++camera_index) {
    auto* camera = new QWidget();
    auto* camera_layout = new QVBoxLayout(camera);
    auto* camera_host = new LetterboxRenderHost(16.0 / 9.0);
    camera_host->setObjectName(QString("camera%1LetterboxHost").arg(camera_index + 1));
    configure_host(camera_host, camera_index + 2, QString("camera%1FocusButton").arg(camera_index + 1));
    QWidget* camera_surface = camera_host->renderSurface();
    camera_surface->setObjectName(QString("camera%1PreviewSurface").arg(camera_index + 1));
    camera_preview_surfaces_.push_back(camera_surface);
    QWidget* camera_render_target = camera_host->renderTarget();
    camera_render_target->setObjectName(QString("camera%1PreviewRenderTarget").arg(camera_index + 1));
    camera_preview_render_targets_.push_back(camera_render_target);

    auto* camera_notice = new QLabel("Camera preview requires embedded X11 rendering", camera_host);
    camera_notice->setObjectName(QString("camera%1ExternalRenderNotice").arg(camera_index + 1));
    camera_notice->setAlignment(Qt::AlignCenter);
    camera_notice->setWordWrap(true);
    camera_notice->setStyleSheet("color: #c9d1d9; padding: 24px;");
    camera_notice->hide();
    camera_preview_notices_.push_back(camera_notice);
    auto* camera_notice_layout = new QVBoxLayout(camera_host);
    camera_notice_layout->setContentsMargins(0, 0, 0, 0);
    camera_notice_layout->addWidget(camera_notice);

    camera_layout->addWidget(camera_host, 1);
    auto* camera_status = new QLabel(
        QString("Camera %1 decoded source preview — downstream controls do not alter this tab").arg(camera_index + 1));
    camera_status->setObjectName(QString("camera%1PreviewStatusLabel").arg(camera_index + 1));
    camera_layout->addWidget(camera_status);
    preview_tabs_->addTab(camera, QString("Camera %1").arg(camera_index + 1));
  }
  pipeline_inspector_ = new PipelineInspectorWidget();
  pipeline_inspector_->setCommandWriter([this](const QByteArray& command) {
    return pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning &&
        pipeline_process_->write(command) == command.size();
  });
  pipeline_inspector_tab_index_ = preview_tabs_->addTab(pipeline_inspector_, "Pipeline");
  preview_tabs_->setTabToolTip(
      pipeline_inspector_tab_index_,
      "Inspect the structured live GStreamer graph, select elements, and edit explicitly live-mutable properties.");
  connect(preview_tabs_, &QTabWidget::currentChanged, this, [this](int tab_index) {
    if (preview_focus_mode_) {
      if (!canFocusPreview(tab_index)) {
        setPreviewFocusMode(false, tab_index);
      } else {
        focused_preview_tab_ = tab_index;
        for (size_t index = 0; index < preview_hosts_.size(); ++index) {
          auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[index]);
          if (host)
            host->setFocused(static_cast<int>(index) == tab_index);
        }
      }
    }
    if (tab_index == pipeline_inspector_tab_index_) {
      // The inspector has no video surface. Quiesce the previously selected
      // GPU branch while it is hidden; returning to a video tab issues a new
      // generation and therefore re-arms first-frame recovery.
      if (!requestPipelinePreviewChannel("none", PreviewRequestReason::kTabChange)) {
        appendLog("could not request Pipeline inspector GPU idle state; retrying");
        scheduleInspectorPreviewIdleRetry(previewDisableTimeoutMs());
      }
      if (pipeline_inspector_ && pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
        pipeline_inspector_->requestRefresh();
      }
      return;
    }
    switchPipelineRenderTarget(tab_index);
  });
  root->addWidget(preview_tabs_, 1);
}

void HStreamWindow::buildOutputControls(QVBoxLayout* parent) {
  auto* group = new QGroupBox("Output Routing");
  group->setObjectName("outputRoutingGroup");
  group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto* layout = new QVBoxLayout(group);
  layout->setSpacing(group->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
  output_list_ = layout;

  const std::vector<std::pair<QString, QString>> outputs = {
      {"youtube-primary", "YouTube Primary"},
      {"rtsp-local", "RTSP Local"},
      {"archive-file", "Archive File"},
      {"spare-rtmp", "Spare RTMP"},
  };

  for (const auto& [id, label] : outputs) {
    auto* row = new QHBoxLayout();
    auto* toggle = new QCheckBox(label);
    toggle->setObjectName("outputToggle_" + id);
    toggle->setChecked(false);
    auto* state = make_value_label("outputState_" + id, "STOPPED");
    output_toggles_[id] = toggle;
    output_states_[id] = state;
    connect(toggle, &QCheckBox::toggled, this, [this, id](bool enabled) { toggleOutput(id, enabled); });
    row->addWidget(toggle, 1);
    row->addWidget(state);
    layout->addLayout(row);
    if (id == "archive-file") {
      archive_output_path_label_ = new QLabel("Archive path will be shown when enabled", group);
      archive_output_path_label_->setObjectName("archiveOutputPath");
      archive_output_path_label_->setWordWrap(true);
      archive_output_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
      archive_output_path_label_->setStyleSheet("color: #98a2b3; font-size: 11px; padding-left: 20px;");
      layout->addWidget(archive_output_path_label_);
    }
  }

  auto* redirect = new QPushButton("Redirect YouTube");
  redirect->setObjectName("redirectYoutubeButton");
  connect(redirect, &QPushButton::clicked, this, [this]() { redirectYoutube(); });

  auto* add_rtsp = new QPushButton("Add RTSP Mount");
  add_rtsp->setObjectName("addRtspButton");
  connect(add_rtsp, &QPushButton::clicked, this, [this]() { addRtspOutput(); });

  layout->addWidget(redirect);
  layout->addWidget(add_rtsp);
  parent->addWidget(group, 0, Qt::AlignTop);
  parent->addStretch(1);
}

void HStreamWindow::configureControlHelp() {
  auto help = [this](const QString& object_name, const QString& description) {
    set_control_help(findChild<QWidget*>(object_name), description);
  };

  help(
      "runModeCombo",
      "Choose Program for the full output pipeline, or Stitching Calibration for a stitching-only graph without "
      "detection, tracking, rink-mask filtering, cropping, or Program output. Output archives are created only by "
      "Program runs.");
  help(
      "controlPointsSpin",
      "Set the maximum number of feature control points used during stitching calibration. Changing it makes Program recalibrate stale stitching before continuing.");
  help(
      "stitchFrameTimeEdit",
      "Choose the video timestamp used as the stitching calibration reference frame. Playback returns to its normal start after one-pass calibration completes.");
  help(
      "renderVideoCheck",
      "Show GPU video previews and local monitor audio. This can be toggled while running without stopping the processing pipeline.");
  help(
      "drivegptCsvCheck",
      "For the next Program run, save HM-compatible tracking.csv, camera.csv, and camera_fast.csv in the game "
      "directory, plus timestamp/config sidecars. Only tracker and policy metadata is copied; video pixels remain on the GPU.");
  help(
      "startPipelineButton",
      "Validate the selected game and start the chosen run mode. Output routes are captured when Play is pressed; route changes during playback apply to the next run.");
  help(
      "pausePipelineButton",
      "Pause or resume the running pipeline without discarding its current processing position.");
  help(
      "playbackSeekSlider",
      "Seek relative to the configured run start for rapid play-tracking tests. Seeking is enabled only while a "
      "Program run has local rendering as its sole output; archive, RTMP/YouTube, RTSP, and other outputs disable "
      "it.");
  help("playbackSeekBack10Button", "Seek ten seconds earlier during local-render-only Program playback.");
  help("playbackSeekForward10Button", "Seek ten seconds later during local-render-only Program playback.");
  help(
      "restartStageButton",
      "Stop the current pipeline and start the selected mode again, preserving the current game, controls, and output-route selections.");
  help(
      "resetCameraButton",
      "Restore all Program and Stitched camera controls to their built-in defaults. Use Save Preset afterward if those defaults should persist for this game.");
  help(
      "stopPipelineButton",
      "Request a graceful stop of the running pipeline. Partial archive work is retained rather than presented as a completed video.");

  help(
      "gameSelector",
      "Select an existing game directory and load its videos, stitching configuration, and saved controls.");
  help("gameIdEdit", "Enter the game directory name to create or load under the configured game root.");
  help("createGameButton", "Create the entered game directory if needed, then load its videos and saved controls.");
  help("refreshGamesButton", "Rescan the configured game root and refresh the existing-game list.");
  help("videoPathEdit", "Enter a video file or directory to import into the selected game.");
  help("browseVideoButton", "Choose a video file or directory with the native file browser.");
  help(
      "addVideoButton",
      "Import the entered video using the selected Auto, Left, Center, or Right role and update the game configuration.");
  help("removeVideoButton", "Remove the selected imported video assignment from this game.");
  help(
      "videoRole_auto",
      "Let HStream discover the camera role and chapter ordering from supported filenames and directories.");
  help("videoRole_left", "Assign the imported video explicitly to the left camera timeline.");
  help(
      "videoRole_center",
      "Assign the imported video explicitly to the center camera timeline when that layout is supported.");
  help("videoRole_right", "Assign the imported video explicitly to the right camera timeline.");
  help("videoSetList", "Shows the video files and camera-role assignments currently configured for this game.");

  help(
      "outputToggle_youtube-primary",
      "Enable the primary YouTube/RTMP output for the next Program run. Changes made while playing apply on the next run.");
  help(
      "outputToggle_rtsp-local",
      "Enable the local RTSP server output for the next Program run. Changes made while playing apply on the next run.");
  help(
      "outputToggle_archive-file",
      "Encode an archive during the next Program run. Work is written below the configured output root, then a successful run is losslessly finalized as a fast-start MP4 in the game directory. Stitching Calibration does not archive.");
  help(
      "outputToggle_spare-rtmp",
      "Enable the spare RTMP destination for the next Program run. Changes made while playing apply on the next run.");
  help(
      "redirectYoutubeButton", "Enable and redirect the primary YouTube output using the configured RTMP destination.");
  help("addRtspButton", "Add another local RTSP mount and enable it for the next Program run.");
  help("clearLogButton", "Clear the visible runtime log. This does not stop or otherwise change the running pipeline.");

  help(
      "programFocusButton",
      "Expand the Program preview to fill the application; click again to restore the normal layout.");
  help(
      "stitchedFocusButton",
      "Expand the Stitched preview to fill the application; click again to restore the normal layout.");
  help("camera1FocusButton", "Expand Camera 1 to fill the application; click again to restore the normal layout.");
  help("camera2FocusButton", "Expand Camera 2 to fill the application; click again to restore the normal layout.");
  help("camera3FocusButton", "Expand Camera 3 to fill the application; click again to restore the normal layout.");
  help(
      "programControlsToggle",
      "Show or hide controls that affect Program frames after stitching, allowing the video preview to use more space.");
  help(
      "stitchedControlsToggle",
      "Show or hide controls that affect the stitched canvas before Program tracking, allowing the video preview to use more space.");

  const std::map<QString, QString> camera_help = {
      {"Stop_Direction_Change_Delay_Frames",
       "Frames to wait before stopping tracked motion after its direction changes."},
      {"Cancel_Stop_On_Opposite_Direction", "When enabled, cancel a pending stop if motion reverses direction again."},
      {"Stop_Cancel_Hysteresis_Frames",
       "Frames of opposite-direction motion required before a pending stop is cancelled."},
      {"Stop_Delay_Cooldown_Frames", "Cooldown frames before another direction-change stop delay may begin."},
      {"Time_To_Dest_Speed_Limit_Frames",
       "Limit tracking speed when the estimated time to the destination falls below this frame count."},
      {"Zoom_In_Aggressiveness",
       "How readily play tracking zooms in. 25 exactly preserves the established behavior; higher values lower "
       "only the shrink threshold so the camera zooms in sooner and more often."},
      {"Apply_To_Fast_Box", "Apply saved tracking and motion tuning to the fast/current-ROI tracking box."},
      {"Apply_To_Follower_Box", "Apply saved tracking and motion tuning to the follower/aspect tracking box."},
      {"Bring_Up_Shadows",
       "Reveal shadow and midtone detail with a hue-preserving luma gamma lift matched to the supplied reference. "
       "The effect rolls off smoothly toward white; overlays and alpha are protected. Zero bypasses the grade, and "
       "100 matches the reference strength."},
      {"Lift_Shadow_Black_Point",
       "Allow Bring up shadows to raise exact black with a neutral toe. At 100%, black rises to 15% video level and "
       "the added toe fades out by 60% luma. Disabled by default."},
      {"Exposure_x100",
       "Apply uniform exposure gain. 30, 60, 100, and 130 select settings 0.3, 0.6, 1.0, and 1.3; 100 is +0.5 "
       "stop (sqrt(2) gain). It preserves exact black, raises the whole signal, and clips at white. When Bring up "
       "shadows is also enabled, the luma lift runs first and exposure runs second."},
      {"Use_10_Bit_Grading",
       "Keep 10-bit decoded video in P010 through the lossless camera mux, convert it to RGB10A2, and stitch and "
       "grade in FP16 before the one final RGBA8 conversion. This uses more GPU memory and applies on the next run."},
      {"Overshoot_Stop_Delay_Frames", "Frames to delay stopping when tracking motion overshoots its destination."},
      {"Post_Nonstop_Stop_Delay_Frames", "Frames to delay stopping after a continuous non-stop movement segment."},
      {"Overshoot_Speed_Ratio_x100",
       "Overshoot speed multiplier in hundredths; 70 means 0.70 times the configured speed."},
      {"Max_Speed_X_x10", "Horizontal tracking speed override in tenths; zero keeps the underlying configured value."},
      {"Max_Speed_Y_x10", "Vertical tracking speed override in tenths; zero keeps the underlying configured value."},
      {"Max_Accel_X_x10",
       "Horizontal tracking acceleration override in tenths; zero keeps the underlying configured value."},
      {"Max_Accel_Y_x10",
       "Vertical tracking acceleration override in tenths; zero keeps the underlying configured value."},
      {"Stitch_Rotate_Degrees",
       "Rotate the stitched canvas before play tracking. The default display value of 90 corresponds to no additional configured rotation."},
      {"Link_Fixed_Edge_Rotation_Left_Right",
       "Keep the left and right fixed-edge crop rotations equal when enabled; disable it to tune each side independently."},
      {"Left_Fixed_Edge_Rotation_Angle_x10",
       "Left fixed-edge crop rotation in tenths of a degree; 250 means 25.0 degrees."},
      {"Right_Fixed_Edge_Rotation_Angle_x10",
       "Right fixed-edge crop rotation in tenths of a degree; 250 means 25.0 degrees."},
  };
  for (const auto& [id, description] : camera_help) {
    const QString object_name =
        id == "Lift_Shadow_Black_Point" || id == "Use_10_Bit_Grading" ? "cameraCheck_" + id : "cameraSlider_" + id;
    help(object_name, description + " Changes apply live where supported; Save Preset stores the value for this game.");
  }
  updatePresetDirtyState();
}

void HStreamWindow::buildCameraControls(QVBoxLayout* parent, bool program_stage) {
  auto default_value = [this](const QString& id) {
    const auto found = camera_defaults_.find(id);
    if (found == camera_defaults_.end())
      throw std::logic_error(QString("No initialized camera-control default for %1").arg(id).toStdString());
    return found->second;
  };
  auto* group = new QGroupBox(program_stage ? "Program Controls" : "Stitched Controls");
  group->setObjectName(program_stage ? "programControlsGroup" : "stitchedControlsGroup");
  group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* layout = new QVBoxLayout(group);
  auto* association = new QLabel(
      program_stage
          ? "These controls affect Program frames after stitching. Changes are applied live while the pipeline is "
            "running; Save Preset keeps them for the next run."
          : "Stitch rotation affects the stitched canvas before play tracking. It applies live while the pipeline "
            "is running; Save Preset keeps it for the next run.");
  association->setObjectName(program_stage ? "programControlAssociation" : "stitchedControlAssociation");
  association->setWordWrap(true);
  association->setStyleSheet("color: #667085;");
  layout->addWidget(association);

  auto* control_tabs = new QTabWidget();
  control_tabs->setObjectName(program_stage ? "programControlTabs" : "stitchedControlTabs");
  control_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  control_tabs->setMinimumHeight(92);
  control_tabs->setMaximumHeight(180);
  if (program_stage)
    program_control_tabs_ = control_tabs;
  else
    stitched_control_tabs_ = control_tabs;

  auto add_slider_tab = [this, &default_value](const std::vector<CameraSliderSpec>& specs, bool include_color_toggles) {
    auto* page = new QWidget();
    page->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea();
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget();
    content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* content_layout = new QVBoxLayout(content);
    for (const CameraSliderSpec& spec : specs) {
      addSlider(content_layout, spec.id, spec.label, spec.minimum, spec.maximum, spec.default_value);
    }
    if (include_color_toggles) {
      addCameraCheckBox(
          content_layout,
          "Lift_Shadow_Black_Point",
          "Lift black point too (stronger)",
          default_value("Lift_Shadow_Black_Point") != 0);
      addCameraCheckBox(
          content_layout,
          "Use_10_Bit_Grading",
          "Use 10-bit / FP16 stitch and grade (next run)",
          default_value("Use_10_Bit_Grading") != 0);
    }
    content_layout->addStretch(1);
    scroll->setWidget(content);
    page_layout->addWidget(scroll, 1);
    return page;
  };

  const std::vector<CameraSliderSpec> tracking_controls = {
      {"Zoom_In_Aggressiveness", "Zoom-in aggressiveness", 0, 100, default_value("Zoom_In_Aggressiveness")},
      {"Stop_Direction_Change_Delay_Frames",
       "Stop direction-change delay frames",
       0,
       60,
       default_value("Stop_Direction_Change_Delay_Frames")},
      {"Cancel_Stop_On_Opposite_Direction",
       "Cancel stop on opposite direction",
       0,
       1,
       default_value("Cancel_Stop_On_Opposite_Direction")},
      {"Stop_Cancel_Hysteresis_Frames",
       "Stop cancel hysteresis frames",
       0,
       10,
       default_value("Stop_Cancel_Hysteresis_Frames")},
      {"Stop_Delay_Cooldown_Frames", "Stop-delay cooldown frames", 0, 30, default_value("Stop_Delay_Cooldown_Frames")},
      {"Time_To_Dest_Speed_Limit_Frames",
       "Time-to-destination speed limit frames",
       0,
       120,
       default_value("Time_To_Dest_Speed_Limit_Frames")},
      {"Apply_To_Fast_Box", "Apply to fast box", 0, 1, default_value("Apply_To_Fast_Box")},
      {"Apply_To_Follower_Box", "Apply to follower box", 0, 1, default_value("Apply_To_Follower_Box")},
  };
  const std::vector<CameraSliderSpec> motion_controls = {
      {"Overshoot_Stop_Delay_Frames",
       "Overshoot stop-delay frames",
       0,
       60,
       default_value("Overshoot_Stop_Delay_Frames")},
      {"Post_Nonstop_Stop_Delay_Frames",
       "Post-nonstop stop-delay frames",
       0,
       60,
       default_value("Post_Nonstop_Stop_Delay_Frames")},
      {"Overshoot_Speed_Ratio_x100", "Overshoot speed ratio x100", 0, 200, default_value("Overshoot_Speed_Ratio_x100")},
      {"Max_Speed_X_x10", "Max speed X override x10 (0 = configured)", 0, 2000, default_value("Max_Speed_X_x10")},
      {"Max_Speed_Y_x10", "Max speed Y override x10 (0 = configured)", 0, 2000, default_value("Max_Speed_Y_x10")},
      {"Max_Accel_X_x10", "Max accel X override x10 (0 = configured)", 0, 1000, default_value("Max_Accel_X_x10")},
      {"Max_Accel_Y_x10", "Max accel Y override x10 (0 = configured)", 0, 1000, default_value("Max_Accel_Y_x10")},
  };
  const std::vector<CameraSliderSpec> color_controls = {
      {"Bring_Up_Shadows", "Bring up shadows (%)", 0, 100, 0},
      {"Exposure_x100", "Exposure x100", 0, 130, 0},
  };
  const std::vector<CameraSliderSpec> stitch_controls = {
      {"Stitch_Rotate_Degrees", "Stitch rotate degrees", 0, 180, default_value("Stitch_Rotate_Degrees")},
      {"Link_Fixed_Edge_Rotation_Left_Right",
       "Link left/right fixed-edge rotation",
       0,
       1,
       default_value("Link_Fixed_Edge_Rotation_Left_Right")},
      {"Left_Fixed_Edge_Rotation_Angle_x10",
       "Left fixed-edge rotation angle x10",
       0,
       kFixedEdgeRotationMaximumX10,
       default_value("Left_Fixed_Edge_Rotation_Angle_x10")},
      {"Right_Fixed_Edge_Rotation_Angle_x10",
       "Right fixed-edge rotation angle x10",
       0,
       kFixedEdgeRotationMaximumX10,
       default_value("Right_Fixed_Edge_Rotation_Angle_x10")},
  };

  if (program_stage) {
    control_tabs->addTab(add_slider_tab(tracking_controls, false), "Tracking");
    control_tabs->addTab(add_slider_tab(motion_controls, false), "Motion");
    control_tabs->addTab(add_slider_tab(color_controls, true), "Color");
    const std::vector<CameraSliderSpec> crop_controls(stitch_controls.begin() + 1, stitch_controls.end());
    control_tabs->addTab(add_slider_tab(crop_controls, false), "Crop Rotation");
  } else {
    const std::vector<CameraSliderSpec> rotation_controls = {stitch_controls.front()};
    control_tabs->addTab(add_slider_tab(rotation_controls, false), "Rotation");
    auto* algorithms_page = new QWidget();
    algorithms_page->setObjectName("stitchingAlgorithmsTab");
    auto* algorithms_layout = new QGridLayout(algorithms_page);
    algorithms_layout->setContentsMargins(8, 8, 8, 8);
    algorithms_layout->setColumnStretch(1, 1);
    auto* matcher_label = new QLabel("Control-point matcher");
    matcher_label->setObjectName("controlPointMatcherLabel");
    matcher_label->setBuddy(control_point_matcher_combo_);
    auto* mapping_label = new QLabel("Mapping backend");
    mapping_label->setObjectName("mappingBackendLabel");
    mapping_label->setBuddy(mapping_backend_combo_);
    auto* max_width_label = new QLabel("Max stitched width");
    max_width_label->setObjectName("stitchMaxOutputWidthLabel");
    max_width_label->setBuddy(stitch_max_output_width_spin_);
    control_point_matcher_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mapping_backend_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    stitch_max_output_width_spin_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    algorithms_layout->addWidget(matcher_label, 0, 0);
    algorithms_layout->addWidget(control_point_matcher_combo_, 0, 1);
    algorithms_layout->addWidget(mapping_label, 1, 0);
    algorithms_layout->addWidget(mapping_backend_combo_, 1, 1);
    algorithms_layout->addWidget(max_width_label, 2, 0);
    algorithms_layout->addWidget(stitch_max_output_width_spin_, 2, 1);
    algorithms_layout->setRowStretch(3, 1);
    control_tabs->addTab(algorithms_page, "Algorithms");
  }
  layout->addWidget(control_tabs);
  parent->addWidget(group);
}

void HStreamWindow::buildLog(QVBoxLayout* root) {
  auto* header = new QHBoxLayout();
  auto* title = new QLabel("Runtime log");
  QFont title_font = title->font();
  title_font.setBold(true);
  title->setFont(title_font);
  auto* clear = new QPushButton("Clear Log");
  clear->setObjectName("clearLogButton");
  clear->setToolTip("Clear the visible runtime log");
  header->addWidget(title);
  header->addStretch(1);
  header->addWidget(clear);
  root->addLayout(header);

  log_ = new QTextEdit();
  log_->setObjectName("runtimeLog");
  log_->setReadOnly(true);
  log_->setAcceptRichText(true);
  log_->setLineWrapMode(QTextEdit::NoWrap);
  // Calibration now reports every native stage, and operators need the lead-up
  // to a failure rather than only the final few hundred lines.
  log_->document()->setMaximumBlockCount(2000);
  log_->setMinimumHeight(60);
  log_->setStyleSheet(
      "QTextEdit#runtimeLog {"
      " background: #05070a;"
      " color: #d8dee9;"
      " font-family: \"JetBrains Mono\", \"SFMono-Regular\", Consolas, monospace;"
      " font-size: 12px;"
      " border: 1px solid #252a31;"
      " selection-background-color: #264f78;"
      "}");
  connect(clear, &QPushButton::clicked, log_, &QTextEdit::clear);
  root->addWidget(log_);
}

QString HStreamWindow::pipelineRunnerPath() const {
  const QByteArray test_runner = qgetenv("HSTREAM_UI_TEST_RUNNER");
  if (!test_runner.isEmpty()) {
    return QString::fromLocal8Bit(test_runner);
  }
  if (!development_pipeline_runner_.isEmpty()) {
    return development_pipeline_runner_;
  }
  if (!development_bazel_bin_.isEmpty()) {
    // A Bazel-built UI must never fall back to an installed or differently
    // configured CLI. Returning the expected sibling path makes startup fail
    // closed with a useful missing-runner diagnostic.
    return QDir(development_bazel_bin_).filePath("src/apps/pipeline-app/hstream-cli");
  }
  const QString installed_runner = "/opt/hstream/bin/hstream-cli";
  if (QFileInfo::exists(installed_runner)) {
    return installed_runner;
  }
  const QString bazel_runner = QDir::current().filePath("bazel-bin/src/apps/pipeline-app/hstream-cli");
  if (QFileInfo::exists(bazel_runner)) {
    return bazel_runner;
  }
  const QString legacy_bazel_runner = QDir::current().filePath("bazel-bin/src/apps/pipeline-app/pipeline-app");
  if (QFileInfo::exists(legacy_bazel_runner)) {
    return legacy_bazel_runner;
  }
  return "hstream-cli";
}

QString HStreamWindow::pipelineConfigPath(const QString& config_name) const {
  if (!development_runtime_root_.isEmpty()) {
    const QString development_config = QDir(QDir(development_runtime_root_).filePath("configs")).filePath(config_name);
    if (QFileInfo::exists(development_config)) {
      return development_config;
    }
  }
  const QString installed_config = QDir("/opt/hstream/configs").filePath(config_name);
  if (QFileInfo::exists(installed_config)) {
    return installed_config;
  }
  return QDir("configs").filePath(config_name);
}

QString HStreamWindow::pipelineWorkingDirectory() const {
  if (!qgetenv("HSTREAM_UI_TEST_RUNNER").isEmpty()) {
    return QDir::currentPath();
  }
  if (!development_runtime_root_.isEmpty()) {
    return development_runtime_root_;
  }
  if (QFileInfo::exists("/opt/hstream/bin/hstream-cli")) {
    return "/opt/hstream";
  }
  return QDir::currentPath();
}

bool HStreamWindow::setupPretrainedAssets(const QStringList& pipeline_args) {
  Q_UNUSED(pipeline_args);
  appendLog("pretrained assets will be verified by hstream-cli");
  return true;
}

void HStreamWindow::logMissingTensorRtEngineCaches(const QStringList& pipeline_args) {
  const QString working_dir = pipelineWorkingDirectory();
  const QStringList config_files = pipeline_config_files_from_args(pipeline_args);
  std::set<std::string> logged_engines;
  for (const QString& config_file_arg : config_files) {
    const QString config_file = resolve_config_path(config_file_arg, working_dir);
    if (!QFileInfo::exists(config_file)) {
      continue;
    }

    try {
      const YAML::Node pipeline = YAML::LoadFile(config_file.toStdString());
      YAML::Node primary_gie;
      if (!lookup_yaml_key(pipeline, "primary-gie", &primary_gie) || !primary_gie.IsMap()) {
        continue;
      }
      if (!yaml_scalar_bool(map_value(primary_gie, "enable"), true)) {
        continue;
      }
      YAML::Node infer_config_node;
      if (!lookup_yaml_key(primary_gie, "config-file", &infer_config_node) || !infer_config_node.IsScalar()) {
        continue;
      }

      const QString infer_config = resolve_config_path(
          QString::fromStdString(infer_config_node.as<std::string>()), QFileInfo(config_file).absolutePath());
      if (!QFileInfo::exists(infer_config)) {
        continue;
      }
      const YAML::Node infer = YAML::LoadFile(infer_config.toStdString());
      YAML::Node engine_node;
      if (!lookup_yaml_path(infer, "property.model-engine-file", &engine_node) || !engine_node.IsScalar()) {
        continue;
      }

      const QString engine_file = resolve_config_path(
          QString::fromStdString(engine_node.as<std::string>()), QFileInfo(infer_config).absolutePath());
      if (QFileInfo::exists(engine_file)) {
        continue;
      }
      if (!logged_engines.insert(engine_file.toStdString()).second) {
        continue;
      }

      appendLog(QString("configured TensorRT engine seed missing: %1").arg(engine_file));
      appendLog(
          "first run will build the primary-gie engine in HStream's writable per-user cache before video appears; the render window may stay black during this step");
      appendLog("DeepStream may also log a model-engine-file open/deserialize warning while it builds the engine");
    } catch (const std::exception& e) {
      appendLog(QString("could not inspect TensorRT engine cache from %1: %2").arg(config_file, e.what()));
    }
  }
}

QStringList HStreamWindow::enabledSinkNames() const {
  QStringList sinks;
  for (const auto& [id, toggle] : output_toggles_) {
    if (!toggle || !toggle->isChecked()) {
      continue;
    }
    if (id.contains("youtube") || id.contains("rtmp")) {
      sinks.push_back("RTMP");
    } else if (id.contains("rtsp")) {
      sinks.push_back("RTSP");
    } else if (id.contains("archive")) {
      sinks.push_back("ENCODE_FILE");
    }
  }
  if (render_video_toggle_ && render_video_toggle_->isChecked() && !sinks.contains("RENDER")) {
    sinks.push_front("RENDER");
  }
  if (sinks.isEmpty()) {
    sinks.push_back("FAKE");
  }
  sinks.removeDuplicates();
  return sinks;
}

bool HStreamWindow::isCalibrationRun() const {
  return run_mode_selector_ && run_mode_selector_->currentData().toString() == "stitch-calibration";
}

int HStreamWindow::stitchingCalibrationControlPoints() const {
  return control_points_spin_ ? control_points_spin_->value() : kDefaultStitchCalibrationControlPoints;
}

int HStreamWindow::stitchingCalibrationFrameCount() const {
  return calibration_frame_count_spin_ ? calibration_frame_count_spin_->value() : kDefaultStitchCalibrationFrameCount;
}

int HStreamWindow::stitchingMaxOutputWidth() const {
  return stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->value() : default_stitch_max_output_width_;
}

QString HStreamWindow::stitchFrameTime() const {
  return stitch_frame_time_edit_ ? format_stitch_frame_time(stitch_frame_time_edit_->time())
                                 : default_stitch_frame_time_;
}

QString HStreamWindow::controlPointMatcher() const {
  return control_point_matcher_combo_ ? control_point_matcher_combo_->currentData().toString()
                                      : default_control_point_matcher_;
}

QString HStreamWindow::mappingBackend() const {
  return mapping_backend_combo_ ? mapping_backend_combo_->currentData().toString() : default_mapping_backend_;
}

bool HStreamWindow::runStitchingClean(
    const QString& runner,
    const QString& working_dir,
    const QProcessEnvironment& env,
    bool from_control_points,
    const QString& expected_invalidation_id) {
  const QString game_id = game_id_edit_ ? game_id_edit_->text().trimmed() : QString();
  QStringList clean_args;
  clean_args << "-g" << game_id << "--enable-sources=URI-MULTIPLE";
  clean_args << "-c" << pipelineConfigPath("ds_hockey_configure_stitching.yaml");
  clean_args << (from_control_points ? "--clean-from-control-points" : "--clean");
  if (!expected_invalidation_id.isEmpty())
    clean_args << QString("--clean-expected-invalidation-id=%1").arg(expected_invalidation_id);

  appendLog(QString("stitching calibration clean command %1 %2").arg(runner, clean_args.join(' ')));
  QProcess clean;
  clean.setProcessChannelMode(QProcess::MergedChannels);
  clean.setProcessEnvironment(env);
  clean.setWorkingDirectory(working_dir);
  clean.start(runner, clean_args);
  if (!clean.waitForStarted(5000)) {
    appendLog(QString("failed to start stitching clean: %1").arg(clean.errorString()));
    return false;
  }
  QByteArray output;
  while (clean.state() != QProcess::NotRunning) {
    if (!clean.waitForReadyRead(250) && clean.error() != QProcess::Timedout) {
      break;
    }
    output += clean.readAll();
  }
  clean.waitForFinished(0);
  output += clean.readAll();
  const QString output_text = QString::fromLocal8Bit(output).trimmed();
  if (!output_text.isEmpty()) {
    for (const QString& line : output_text.split('\n')) {
      appendLog(line.trimmed());
    }
  }
  if (clean.exitStatus() != QProcess::NormalExit || clean.exitCode() != 0) {
    appendLog(QString("stitching clean failed exit=%1").arg(clean.exitCode()));
    return false;
  }
  return true;
}

bool HStreamWindow::saveStitchingCalibrationState(
    const QString& game_id,
    int control_points,
    const QString& status,
    const QString& stale_from,
    const QString& expected_invalidation_id,
    bool artifacts_invalidated,
    bool require_matching_pending,
    bool* applied) {
  if (applied)
    *applied = false;
  const fs::path config_path = fs::path(gameDirectory(game_id).toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(
        QString("could not lock stitching calibration settings: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }
  YAML::Node config(YAML::NodeType::Map);
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not save stitching calibration settings: %1").arg(exc.what()));
      return false;
    }
    if (!yaml_defined(config) || config.IsNull()) {
      config = YAML::Node(YAML::NodeType::Map);
    }
  }
  YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  if (require_matching_pending) {
    QString current_stitch_frame_time = default_stitch_frame_time_;
    const bool current_stitch_frame_time_valid =
        read_stitch_frame_time(config, &current_stitch_frame_time, nullptr, default_stitch_frame_time_);
    const int current_control_points = calibration["control_points"] && calibration["control_points"].IsScalar()
        ? calibration["control_points"].as<int>()
        : -1;
    const int current_frame_count =
        calibration["frame_count"] && calibration["frame_count"].IsScalar() ? calibration["frame_count"].as<int>() : -1;
    const QString current_control_point_matcher = canonical_or_normalized_matcher_choice(
        config["stitching"]["control_point_matcher"] && config["stitching"]["control_point_matcher"].IsScalar()
            ? QString::fromStdString(config["stitching"]["control_point_matcher"].as<std::string>())
            : QString(),
        default_control_point_matcher_);
    const QString current_mapping_backend = canonical_or_normalized_mapping_choice(
        config["stitching"]["mapping_backend"] && config["stitching"]["mapping_backend"].IsScalar()
            ? QString::fromStdString(config["stitching"]["mapping_backend"].as<std::string>())
            : QString(),
        default_mapping_backend_);
    const int current_max_output_width = read_stitch_max_output_width_from_config(
        config,
        default_stitch_max_output_width_,
        stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->maximum() : std::numeric_limits<int>::max());
    const QString current_status = calibration["status"] && calibration["status"].IsScalar()
        ? QString::fromStdString(calibration["status"].as<std::string>())
        : QString();
    const QString current_stale = calibration["stale_from"] && calibration["stale_from"].IsScalar()
        ? QString::fromStdString(calibration["stale_from"].as<std::string>())
        : QString();
    const QString current_invalidation_id = calibration["invalidation_id"] && calibration["invalidation_id"].IsScalar()
        ? QString::fromStdString(calibration["invalidation_id"].as<std::string>())
        : QString();
    const bool current_invalidated = calibration["artifacts_invalidated"] &&
        calibration["artifacts_invalidated"].IsScalar() && calibration["artifacts_invalidated"].as<bool>();
    const bool already_completed = status == "complete" && current_status == "complete" && current_stale.isEmpty() &&
        current_invalidation_id == expected_invalidation_id && !current_invalidated &&
        current_stitch_frame_time_valid && current_stitch_frame_time == active_stitch_frame_time_ &&
        current_control_points == control_points && current_frame_count == active_calibration_frame_count_ &&
        current_control_point_matcher == active_control_point_matcher_ &&
        current_mapping_backend == active_mapping_backend_ &&
        current_max_output_width == active_stitch_max_output_width_;
    if (already_completed) {
      if (applied)
        *applied = true;
      return true;
    }
    const bool expected_invalidated = status != "pending";
    if (!current_stitch_frame_time_valid || current_stitch_frame_time != active_stitch_frame_time_ ||
        current_control_points != control_points || current_frame_count != active_calibration_frame_count_ ||
        current_control_point_matcher != active_control_point_matcher_ ||
        current_mapping_backend != active_mapping_backend_ ||
        current_max_output_width != active_stitch_max_output_width_ || current_status != "pending" ||
        current_stale != stale_from || current_invalidation_id != expected_invalidation_id ||
        current_invalidated != expected_invalidated) {
      appendLog(
          QString("stitching calibration state transition to %1 skipped because dependency state changed concurrently")
              .arg(status));
      return true;
    }
  }

  calibration["control_points"] = control_points;
  calibration["frame_count"] = active_calibration_frame_count_;
  remove_yaml_path(config, {"hstream_ui", "generated_stitching_backend_choices"});
  remove_yaml_path(config, {"stitching", "calibration_frame_count"});
  remove_stitch_max_output_width_native_aliases(config);
  config["stitching"]["control_point_matcher"] = active_control_point_matcher_.toStdString();
  config["stitching"]["mapping_backend"] = active_mapping_backend_.toStdString();
  if (active_calibration_frame_count_ != kDefaultStitchCalibrationFrameCount) {
    config["stitching"]["calibration_frame_count"] = active_calibration_frame_count_;
  }
  config["stitching"]["max_output_width"] = active_stitch_max_output_width_ > 0
      ? YAML::Node(active_stitch_max_output_width_)
      : YAML::Node(YAML::NodeType::Null);
  calibration["status"] = status.toStdString();
  if (status == "pending")
    calibration["rink_mask_status"] = "pending";
  if (!expected_invalidation_id.isEmpty())
    calibration["invalidation_id"] = expected_invalidation_id.toStdString();
  if (status == "complete") {
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    calibration["rink_mask_status"] = active_run_is_calibration_ ? "omitted" : "complete";
  } else if (!stale_from.isEmpty()) {
    calibration["stale_from"] = stale_from.toStdString();
    calibration["artifacts_invalidated"] = artifacts_invalidated;
  }
  const auto publish = publish_yaml_config(config_path, config);
  if (!publish.ok()) {
    appendLog(QString("failed to write stitching calibration settings %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
    return false;
  }
  if (applied)
    *applied = true;
  appendLog(QString("stitching calibration control points saved %1 status=%2").arg(control_points).arg(status));
  return true;
}

bool HStreamWindow::prepareStitchingCalibrationRun(
    const QString& runner,
    const QString& working_dir,
    const QProcessEnvironment& env,
    bool* calibration_required) {
  if (!calibration_required) {
    appendLog("stitching calibration setup did not provide a result destination");
    return false;
  }
  *calibration_required = false;
  const int control_points = active_calibration_control_points_;
  const int frame_count = active_calibration_frame_count_;
  const fs::path config_path = fs::path(gameDirectory(active_run_game_id_).toStdString()) / "config.yaml";
  bool saved_found = false;
  int saved_control_points = 0;
  int saved_frame_count = kDefaultStitchCalibrationFrameCount;
  bool saved_frame_count_found = false;
  QString saved_stitch_frame_time = default_stitch_frame_time_;
  bool saved_stitch_frame_time_valid = true;
  int saved_max_output_width = default_stitch_max_output_width_;
  QString saved_control_point_matcher = default_control_point_matcher_;
  QString saved_mapping_backend = default_mapping_backend_;
  QString saved_status;
  QString saved_stale_from;
  bool saved_artifacts_invalidated = false;
  bool clean_all = false;
  bool clean_from_control_points = false;
  {
    auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
    if (!config_lock.ok()) {
      appendLog(
          QString("could not lock stitching calibration settings: %1").arg(config_lock.status().ToString().c_str()));
      return false;
    }
    YAML::Node config(YAML::NodeType::Map);
    try {
      if (fs::exists(config_path))
        config = YAML::LoadFile(config_path.string());
      if (!yaml_defined(config) || config.IsNull())
        config = YAML::Node(YAML::NodeType::Map);
      YAML::Node saved;
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.control_points", &saved) && saved.IsScalar()) {
        saved_control_points = saved.as<int>();
        saved_found = true;
      }
      YAML::Node saved_frame_count_node;
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.frame_count", &saved_frame_count_node) &&
          saved_frame_count_node.IsScalar()) {
        saved_frame_count = saved_frame_count_node.as<int>();
        saved_frame_count_found = true;
      }
      saved_stitch_frame_time_valid =
          read_stitch_frame_time(config, &saved_stitch_frame_time, nullptr, default_stitch_frame_time_);
      YAML::Node control_point_matcher;
      if (lookup_yaml_path(config, "stitching.control_point_matcher", &control_point_matcher) &&
          control_point_matcher.IsScalar()) {
        saved_control_point_matcher = canonical_or_normalized_matcher_choice(
            QString::fromStdString(control_point_matcher.as<std::string>()), default_control_point_matcher_);
      }
      YAML::Node mapping_backend;
      if (lookup_yaml_path(config, "stitching.mapping_backend", &mapping_backend) && mapping_backend.IsScalar()) {
        saved_mapping_backend = canonical_or_normalized_mapping_choice(
            QString::fromStdString(mapping_backend.as<std::string>()), default_mapping_backend_);
      }
      try {
        saved_max_output_width = read_stitch_max_output_width_from_config(
            config,
            default_stitch_max_output_width_,
            stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->maximum() : std::numeric_limits<int>::max());
      } catch (const std::exception& ex) {
        qWarning() << "Ignoring malformed existing stitch max output width while preparing play:" << ex.what();
        saved_max_output_width = std::numeric_limits<int>::min();
      }
      YAML::Node status;
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.status", &status) && status.IsScalar()) {
        saved_status = QString::fromStdString(status.as<std::string>());
      }
      YAML::Node stale_from;
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.stale_from", &stale_from) &&
          stale_from.IsScalar()) {
        saved_stale_from = QString::fromStdString(stale_from.as<std::string>());
      }
      YAML::Node artifacts_invalidated;
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.artifacts_invalidated", &artifacts_invalidated) &&
          artifacts_invalidated.IsScalar()) {
        saved_artifacts_invalidated = artifacts_invalidated.as<bool>();
      }
    } catch (const std::exception& exc) {
      appendLog(QString("could not read stitching calibration settings: %1").arg(exc.what()));
      return false;
    }

    const bool control_points_changed = !saved_found || saved_control_points != control_points;
    const bool frame_count_changed = !saved_frame_count_found || saved_frame_count != frame_count;
    const bool max_output_width_changed =
        saved_max_output_width != active_stitch_max_output_width_ ||
        has_conflicting_stitch_max_output_width_native_alias(
            config,
            active_stitch_max_output_width_,
            stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->maximum() : std::numeric_limits<int>::max());
    const bool control_point_matcher_changed = saved_control_point_matcher != active_control_point_matcher_;
    const bool mapping_backend_changed = saved_mapping_backend != active_mapping_backend_;
    const bool stitch_frame_time_changed =
        !saved_stitch_frame_time_valid || saved_stitch_frame_time != active_stitch_frame_time_;
    remove_yaml_path(config, {"stitching", "stitch_frame_time"});
    remove_yaml_path(config, {"stitching", "control_point_matcher"});
    remove_yaml_path(config, {"stitching", "mapping_backend"});
    remove_yaml_path(config, {"stitching", "max_output_width"});
    remove_yaml_path(config, {"stitching", "calibration_frame_count"});
    remove_stitch_max_output_width_native_aliases(config);
    remove_yaml_path(config, {"hstream_ui", "generated_stitching_backend_choices"});
    if (active_stitch_frame_time_ != default_stitch_frame_time_) {
      config["stitching"]["stitch_frame_time"] = active_stitch_frame_time_.toStdString();
    }
    config["stitching"]["control_point_matcher"] = active_control_point_matcher_.toStdString();
    config["stitching"]["mapping_backend"] = active_mapping_backend_.toStdString();
    if (active_calibration_frame_count_ != kDefaultStitchCalibrationFrameCount) {
      config["stitching"]["calibration_frame_count"] = active_calibration_frame_count_;
    }
    config["stitching"]["max_output_width"] = active_stitch_max_output_width_ > 0
        ? YAML::Node(active_stitch_max_output_width_)
        : YAML::Node(YAML::NodeType::Null);
    const bool needs_calibration = active_force_reconfigure_ || stitch_frame_time_changed || control_points_changed ||
        frame_count_changed || control_point_matcher_changed || mapping_backend_changed || max_output_width_changed ||
        saved_status != "complete";
    if (!needs_calibration) {
      active_calibration_start_stage_.clear();
      // Reserve one generation owner before the process starts. Program can
      // discover a missing artifact only after the first stitched frame; this
      // token lets that backend work fail closed if a newer UI invalidation
      // supersedes the run before it publishes anything.
      active_calibration_invalidation_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
      config["hstream_ui"]["stitching_calibration"]["invalidation_id"] =
          active_calibration_invalidation_id_.toStdString();
      const auto publish = publish_yaml_config(config_path, config);
      if (!publish.ok()) {
        appendLog(QString("failed to reserve stitching calibration owner %1: %2")
                      .arg(active_calibration_invalidation_id_, publish.ToString().c_str()));
        active_calibration_invalidation_id_.clear();
        return false;
      }
      return true;
    }

    QString stale_from = saved_stale_from;
    if (!calibration_stage_index(stale_from).has_value()) {
      stale_from = (mapping_backend_changed || max_output_width_changed) && !control_point_matcher_changed &&
              !control_points_changed && !frame_count_changed
          ? QString("canvas")
          : QString("input");
    }
    const size_t features_index = *calibration_stage_index("features");
    if ((control_points_changed || control_point_matcher_changed) && saved_status == "complete") {
      stale_from = "features";
    } else if (
        (control_points_changed || control_point_matcher_changed) && saved_found &&
        features_index < *calibration_stage_index(stale_from)) {
      stale_from = "features";
    }
    const size_t canvas_index = *calibration_stage_index("canvas");
    if ((mapping_backend_changed || max_output_width_changed) && !control_point_matcher_changed &&
        !control_points_changed && !frame_count_changed && canvas_index < *calibration_stage_index(stale_from)) {
      stale_from = "canvas";
    }
    if (active_force_reconfigure_ || stitch_frame_time_changed || frame_count_changed)
      stale_from = "input";
    active_calibration_start_stage_ = stale_from;

    clean_from_control_points = !active_force_reconfigure_ && !stitch_frame_time_changed && stale_from == "features" &&
        (control_points_changed || control_point_matcher_changed || !saved_artifacts_invalidated);
    clean_all = active_force_reconfigure_ || stitch_frame_time_changed ||
        (stale_from != "features" &&
         (!saved_artifacts_invalidated || control_points_changed || control_point_matcher_changed ||
          mapping_backend_changed || max_output_width_changed));

    active_calibration_invalidation_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points;
    calibration["frame_count"] = frame_count;
    calibration["status"] = "pending";
    calibration["rink_mask_status"] = "pending";
    calibration["stale_from"] = stale_from.toStdString();
    calibration["artifacts_invalidated"] = !(clean_all || clean_from_control_points);
    calibration["invalidation_id"] = active_calibration_invalidation_id_.toStdString();
    const auto publish = publish_yaml_config(config_path, config);
    if (!publish.ok()) {
      appendLog(QString("failed to write stitching calibration settings %1: %2")
                    .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
      return false;
    }
  }

  if (active_force_reconfigure_)
    appendLog("stitching calibration restart requested; rebuilding the complete dependency graph");
  if (saved_stitch_frame_time != active_stitch_frame_time_ || !saved_stitch_frame_time_valid) {
    appendLog(QString("stitch frame time changed %1 -> %2; rebuilding stitching from the selected frame")
                  .arg(saved_stitch_frame_time_valid ? saved_stitch_frame_time : QString("invalid"))
                  .arg(active_stitch_frame_time_));
  }
  if (clean_all) {
    appendLog(QString("stitching calibration dependency %1 is stale; cleaning it and all downstream artifacts")
                  .arg(active_calibration_start_stage_));
    if (!runStitchingClean(
            runner,
            working_dir,
            env,
            /*from_control_points=*/false,
            active_calibration_invalidation_id_)) {
      return false;
    }
  } else if (clean_from_control_points) {
    const QString previous = saved_found ? QString::number(saved_control_points) : QString("unset");
    appendLog(QString(
                  "stitching calibration control points changed %1 -> %2; invalidating control points and "
                  "downstream artifacts")
                  .arg(previous)
                  .arg(control_points));
    if (!runStitchingClean(
            runner,
            working_dir,
            env,
            /*from_control_points=*/true,
            active_calibration_invalidation_id_)) {
      return false;
    }
  } else {
    appendLog(QString("stitching calibration resuming from stale dependency %1 without cleaning cached inputs")
                  .arg(active_calibration_start_stage_));
  }

  if (clean_all || clean_from_control_points) {
    bool state_applied = false;
    if (!saveStitchingCalibrationState(
            active_run_game_id_,
            control_points,
            "pending",
            active_calibration_start_stage_,
            active_calibration_invalidation_id_,
            true,
            /*require_matching_pending=*/true,
            &state_applied)) {
      return false;
    }
    if (!state_applied) {
      appendLog("stitching calibration cleanup was superseded by a newer dependency invalidation");
      return false;
    }
  }
  *calibration_required = true;
  return true;
}

void HStreamWindow::showStitchingCalibrationDialog() {
  if (!calibration_dialog_) {
    auto* dialog = new StitchingCalibrationDialog(this);
    calibration_dialog_ = dialog;
    dialog->setObjectName("stitchCalibrationDialog");
    dialog->setWindowTitle("Stitching calibration");
    // Keep calibration modal to this HStream window. A system-wide
    // WindowStaysOnTopHint prevents the operator from using unrelated apps
    // while calibration runs, which can take several minutes.
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumWidth(560);
    dialog->setStyleSheet(
        "QLabel[calibrationState=\"pending\"] { color: #667085; }"
        "QLabel[calibrationState=\"skipped\"] { color: #667085; font-style: italic; }"
        "QLabel[calibrationState=\"active\"] { color: #1570ef; font-weight: 600; }"
        "QLabel[calibrationState=\"complete\"] { color: #039855; }"
        "QLabel[calibrationState=\"failed\"] { color: #d92d20; font-weight: 600; }");

    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 24, 24, 20);
    root->setSpacing(14);

    auto* heading = new QHBoxLayout();
    heading->setSpacing(14);
    calibration_icon_ = new QLabel(dialog);
    calibration_icon_->setObjectName("stitchCalibrationIcon");
    calibration_icon_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    calibration_icon_->setFixedSize(40, 40);
    heading->addWidget(calibration_icon_);
    auto* heading_text = new QVBoxLayout();
    heading_text->setSpacing(4);
    calibration_headline_ = new QLabel(dialog);
    calibration_headline_->setObjectName("stitchCalibrationHeadline");
    QFont headline_font = calibration_headline_->font();
    headline_font.setPointSize(headline_font.pointSize() + 2);
    headline_font.setBold(true);
    calibration_headline_->setFont(headline_font);
    calibration_detail_ = new QLabel(dialog);
    calibration_detail_->setObjectName("stitchCalibrationDetail");
    calibration_detail_->setWordWrap(true);
    calibration_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading_text->addWidget(calibration_headline_);
    heading_text->addWidget(calibration_detail_);
    heading->addLayout(heading_text, 1);
    root->addLayout(heading);

    calibration_progress_ = new QProgressBar(dialog);
    calibration_progress_->setObjectName("stitchCalibrationProgress");
    calibration_progress_->setRange(0, 0);
    calibration_progress_->setTextVisible(false);
    root->addWidget(calibration_progress_);

    auto* stage_box = new QVBoxLayout();
    stage_box->setSpacing(8);
    for (const CalibrationStageSpec& spec : kCalibrationStages) {
      const QString id = QString::fromLatin1(spec.id);
      auto* row = new QHBoxLayout();
      row->setSpacing(9);
      auto* icon = new QLabel(QString::fromUtf8("\u25cb"), dialog);
      icon->setObjectName(QString("stitchCalibrationStageIcon_%1").arg(id));
      icon->setAlignment(Qt::AlignCenter);
      icon->setFixedWidth(20);
      auto* label = new QLabel(QString::fromLatin1(spec.label), dialog);
      label->setObjectName(QString("stitchCalibrationStage_%1").arg(id));
      row->addWidget(icon);
      row->addWidget(label, 1);
      stage_box->addLayout(row);
      calibration_stage_icons_[id] = icon;
      calibration_stage_labels_[id] = label;
    }
    root->addLayout(stage_box);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    calibration_cancel_button_ = new QPushButton("Stop calibration", dialog);
    calibration_cancel_button_->setObjectName("stitchCalibrationCancelButton");
    set_control_help(
        calibration_cancel_button_,
        "Stop the active stitching calibration and its pipeline. Completed calibration stages remain visible in "
        "the runtime log.");
    calibration_ok_button_ = new QPushButton("OK", dialog);
    calibration_ok_button_->setObjectName("stitchCalibrationOkButton");
    calibration_ok_button_->setDefault(true);
    set_control_help(
        calibration_ok_button_,
        "Close this calibration result after reviewing the failure details. Successful calibration closes "
        "automatically and continues the pipeline.");
    buttons->addWidget(calibration_cancel_button_);
    buttons->addWidget(calibration_ok_button_);
    root->addLayout(buttons);

    connect(calibration_cancel_button_, &QPushButton::clicked, this, [this]() {
      if (calibration_detail_)
        calibration_detail_->setText("Stopping calibration…");
      if (calibration_cancel_button_)
        calibration_cancel_button_->setEnabled(false);
      stopPipeline();
    });
    connect(calibration_ok_button_, &QPushButton::clicked, dialog, &QDialog::accept);
  }

  calibration_dialog_failed_ = false;
  active_calibration_stage_.clear();
  auto apply_state = [](QLabel* label, const char* state) {
    if (!label)
      return;
    label->setProperty("calibrationState", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
  };
  for (const auto& [id, icon] : calibration_stage_icons_) {
    (void)id;
    icon->setText(QString::fromUtf8("\u25cb"));
    apply_state(icon, "pending");
  }
  for (const CalibrationStageSpec& spec : kCalibrationStages) {
    const QString id = QString::fromLatin1(spec.id);
    auto label_it = calibration_stage_labels_.find(id);
    if (label_it == calibration_stage_labels_.end())
      continue;
    QLabel* label = label_it->second;
    label->setText(QString::fromLatin1(spec.label));
    label->setToolTip({});
    apply_state(label, "pending");
  }
  calibration_icon_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(32, 32));
  apply_state(calibration_icon_, "active");
  calibration_headline_->setText("Calibrating stitching…");
  apply_state(calibration_headline_, "active");
  const QString start_stage =
      active_calibration_start_stage_.isEmpty() ? QString("input") : active_calibration_start_stage_;
  calibration_detail_->setText(
      start_stage == "features"
          ? QString(
                "Camera orientation and synchronization are current. Resuming at control-point detection with "
                "a limit of %1.")
                .arg(active_calibration_control_points_)
          : QString("Waiting for synchronized frames from both cameras. Control-point limit: %1.")
                .arg(active_calibration_control_points_));
  calibration_progress_->setVisible(true);
  calibration_ok_button_->setVisible(false);
  calibration_cancel_button_->setVisible(true);
  calibration_cancel_button_->setEnabled(true);
  static_cast<StitchingCalibrationDialog*>(calibration_dialog_)->setCloseAllowed(false);
  for (const CalibrationStageSpec& spec : kCalibrationStages) {
    const QString stage = QString::fromLatin1(spec.id);
    if (stage == start_stage)
      break;
    setStitchingCalibrationStage(stage, "complete", {});
  }
  setStitchingCalibrationStage(start_stage, "started", calibration_detail_->text());
  if (active_run_is_calibration_)
    setStitchingCalibrationStage("rink-mask", "skipped", {});
  calibration_dialog_->show();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

bool HStreamWindow::beginObservedStitchingCalibration(const QString& reported_stage) {
  if (calibration_pending_)
    return true;
  if (active_run_game_id_.isEmpty()) {
    appendLog("ignored unowned stitching calibration progress without an active game");
    return false;
  }

  const QString reported_start_stage =
      calibration_stage_index(reported_stage).has_value() ? reported_stage : QString("input");
  const fs::path config_path = fs::path(gameDirectory(active_run_game_id_).toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock runtime-discovered calibration state: %1")
                  .arg(config_lock.status().ToString().c_str()));
    return false;
  }
  try {
    YAML::Node config = fs::is_regular_file(config_path) ? YAML::LoadFile(config_path.string()) : YAML::Node();
    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    const QString current_owner = calibration["invalidation_id"] && calibration["invalidation_id"].IsScalar()
        ? QString::fromStdString(calibration["invalidation_id"].as<std::string>())
        : QString();
    const QString current_status = calibration["status"] && calibration["status"].IsScalar()
        ? QString::fromStdString(calibration["status"].as<std::string>())
        : QString();
    if (active_calibration_invalidation_id_.isEmpty() || current_owner != active_calibration_invalidation_id_ ||
        (current_status != "complete" && current_status != "pending")) {
      appendLog("runtime-discovered calibration was superseded before the pipeline could claim it");
      return false;
    }

    QString current_start_stage = calibration["stale_from"] && calibration["stale_from"].IsScalar()
        ? QString::fromStdString(calibration["stale_from"].as<std::string>())
        : reported_start_stage;
    if (!calibration_stage_index(current_start_stage).has_value() ||
        *calibration_stage_index(reported_start_stage) < *calibration_stage_index(current_start_stage)) {
      current_start_stage = reported_start_stage;
    }
    active_calibration_start_stage_ = current_start_stage;
    calibration["control_points"] = active_calibration_control_points_;
    calibration["frame_count"] = active_calibration_frame_count_;
    calibration["status"] = "pending";
    calibration["rink_mask_status"] = "pending";
    calibration["stale_from"] = active_calibration_start_stage_.toStdString();
    calibration["artifacts_invalidated"] = true;
    const auto publish = publish_yaml_config(config_path, config);
    if (!publish.ok()) {
      appendLog(QString("could not claim runtime-discovered calibration: %1").arg(publish.ToString().c_str()));
      active_calibration_start_stage_.clear();
      return false;
    }
  } catch (const std::exception& exception) {
    appendLog(QString("could not claim runtime-discovered calibration: %1").arg(exception.what()));
    active_calibration_start_stage_.clear();
    return false;
  }

  calibration_pending_ = true;
  updatePlaybackSeekControls();
  appendLog(QString("running pipeline discovered stitching calibration at stage %1; opening progress window")
                .arg(active_calibration_start_stage_));
  showStitchingCalibrationDialog();
  return true;
}

void HStreamWindow::setStitchingCalibrationStage(const QString& stage, const QString& status, const QString& message) {
  auto icon_it = calibration_stage_icons_.find(stage);
  auto label_it = calibration_stage_labels_.find(stage);
  if (icon_it == calibration_stage_icons_.end() || label_it == calibration_stage_labels_.end()) {
    if (!message.isEmpty() && calibration_detail_)
      calibration_detail_->setText(message);
    return;
  }
  auto apply_state = [](QLabel* label, const QString& state) {
    if (!label)
      return;
    label->setProperty("calibrationState", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
  };
  auto mark_complete = [&](const QString& id) {
    const auto previous_icon = calibration_stage_icons_.find(id);
    const auto previous_label = calibration_stage_labels_.find(id);
    if (previous_icon == calibration_stage_icons_.end() || previous_label == calibration_stage_labels_.end())
      return;
    previous_icon->second->setText(QString::fromUtf8("\u2713"));
    apply_state(previous_icon->second, "complete");
    apply_state(previous_label->second, "complete");
  };

  if (status == "started") {
    if (!active_calibration_stage_.isEmpty() && active_calibration_stage_ != stage)
      mark_complete(active_calibration_stage_);
    active_calibration_stage_ = stage;
    icon_it->second->setText(QString::fromUtf8("\u25cf"));
    apply_state(icon_it->second, "active");
    apply_state(label_it->second, "active");
  } else if (status == "complete") {
    mark_complete(stage);
    if (active_calibration_stage_ == stage)
      active_calibration_stage_.clear();
  } else if (status == "failed") {
    active_calibration_stage_ = stage;
    icon_it->second->setText(QString::fromUtf8("\u2715"));
    apply_state(icon_it->second, "failed");
    apply_state(label_it->second, "failed");
  } else if (status == "skipped") {
    icon_it->second->setText(QString::fromUtf8("\u2014"));
    label_it->second->setText("Find the ice surface (Program only)");
    label_it->second->setToolTip("Rink-mask generation is omitted from stitching-only calibration.");
    apply_state(icon_it->second, "skipped");
    apply_state(label_it->second, "skipped");
  }
  if (!message.isEmpty() && calibration_detail_)
    calibration_detail_->setText(message);
}

void HStreamWindow::handleStitchingCalibrationOutput(const QString& line) {
  static const QRegularExpression event_pattern(
      R"(^HSTREAM_CALIBRATION\s+stage=([a-z0-9-]+)\s+status=(started|complete|failed)(?:\s+message=(.*))?$)");
  const QRegularExpressionMatch match = event_pattern.match(line);
  if (!match.hasMatch())
    return;
  const QString stage = match.captured(1);
  const QString status = match.captured(2);
  const QString message = match.captured(3).trimmed();
  if (stage == "playback-restart") {
    if (status == "complete" && !active_run_game_id_.isEmpty()) {
      calibration_playback_restart_observed_ = true;
      if (calibration_waiting_for_playback_restart_)
        completeStitchingCalibration();
    } else if (status == "failed" && calibration_pending_) {
      failStitchingCalibration(message.isEmpty() ? "Playback could not restart after stitching." : message);
    }
    return;
  }
  if (!calibration_pending_ && !beginObservedStitchingCalibration(stage))
    return;
  if (status == "started" && calibration_dialog_)
    calibration_dialog_->show();
  if (stage == "calibration") {
    if (status == "complete") {
      calibration_waiting_for_playback_restart_ = true;
      for (const CalibrationStageSpec& spec : kCalibrationStages) {
        const QString stage_id = QString::fromLatin1(spec.id);
        setStitchingCalibrationStage(
            stage_id, active_run_is_calibration_ && stage_id == "rink-mask" ? "skipped" : "complete", {});
      }
      if (calibration_headline_)
        calibration_headline_->setText("Restarting playback…");
      if (calibration_detail_)
        calibration_detail_->setText("Stitching is ready. Restarting continuous playback from the beginning.");
      if (preview_status_)
        preview_status_->setText("Restarting playback after stitching calibration");
      appendLog("one-pass stitching calibration complete; waiting for continuous playback to restart");
      if (calibration_playback_restart_observed_)
        completeStitchingCalibration();
    } else if (status == "failed") {
      failStitchingCalibration(message.isEmpty() ? "The native stitching calibration failed." : message);
    }
    return;
  }
  setStitchingCalibrationStage(stage, status, message);
}

void HStreamWindow::completeStitchingCalibration() {
  if (!calibration_pending_ || active_run_game_id_.isEmpty())
    return;
  bool state_applied = false;
  if (!saveStitchingCalibrationState(
          active_run_game_id_,
          active_calibration_control_points_,
          "complete",
          active_calibration_start_stage_,
          active_calibration_invalidation_id_,
          true,
          /*require_matching_pending=*/true,
          &state_applied)) {
    failStitchingCalibration("Stitching finished, but its completed state could not be saved.");
    return;
  }
  if (!state_applied) {
    failStitchingCalibration(
        "Stitching inputs changed while calibration was running. Stop and press Play to rebuild the newer dependency.");
    return;
  }
  calibration_waiting_for_playback_restart_ = false;
  calibration_playback_restart_observed_ = false;
  calibration_pending_ = false;
  updatePlaybackSeekControls();
  for (const CalibrationStageSpec& spec : kCalibrationStages) {
    const QString stage_id = QString::fromLatin1(spec.id);
    setStitchingCalibrationStage(
        stage_id, active_run_is_calibration_ && stage_id == "rink-mask" ? "skipped" : "complete", {});
  }
  if (calibration_icon_) {
    calibration_icon_->setPixmap(style()->standardIcon(QStyle::SP_DialogApplyButton).pixmap(32, 32));
    calibration_icon_->setProperty("calibrationState", "complete");
  }
  if (calibration_headline_) {
    calibration_headline_->setText("Stitching calibration complete");
    calibration_headline_->setProperty("calibrationState", "complete");
  }
  if (calibration_detail_) {
    calibration_detail_->setText(
        active_run_is_calibration_
            ? "The stitching maps and panorama are ready. Program will validate or build its rink mask when it starts."
            : "The stitched panorama and ice-surface calibration are ready.");
  }
  if (calibration_progress_)
    calibration_progress_->setVisible(false);
  if (calibration_ok_button_)
    calibration_ok_button_->setVisible(false);
  if (calibration_cancel_button_)
    calibration_cancel_button_->setVisible(false);
  if (calibration_dialog_)
    static_cast<StitchingCalibrationDialog*>(calibration_dialog_)->setCloseAllowed(true);

  const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
  preview_status_->setText(
      active_run_is_calibration_ ? (render_video ? "Continuous stitched preview running"
                                                 : "Stitching pipeline running without video rendering")
                                 : (render_video ? "Program pipeline playing after stitching calibration"
                                                 : "Program pipeline running without video rendering after stitching "
                                                   "calibration"));
  if (stitched_status_) {
    stitched_status_->setText(
        active_run_is_calibration_ ? (render_video ? "Stitching calibrated\nContinuous stitched preview running"
                                                   : "Stitching calibrated\nVideo rendering disabled")
                                   : "Stitching calibrated during program playback");
  }
  appendLog(
      active_run_is_calibration_
          ? (render_video
                 ? "one-pass stitching calibration complete; continuous stitched preview running; camera controls "
                   "remain available"
                 : "one-pass stitching calibration complete; pipeline continuing without video rendering; camera "
                   "controls remain available")
          : (render_video
                 ? "one-pass stitching calibration complete; continuous program playback running; camera controls "
                   "remain available"
                 : "one-pass stitching calibration complete; program pipeline continuing without video rendering; "
                   "camera controls remain available"));
  QTimer::singleShot(250, this, [this]() {
    if (!calibration_pending_ && !calibration_dialog_failed_)
      closeStitchingCalibrationDialog();
  });
}

void HStreamWindow::failStitchingCalibration(const QString& message) {
  if (calibration_dialog_failed_)
    return;
  calibration_waiting_for_playback_restart_ = false;
  calibration_playback_restart_observed_ = false;
  if (!calibration_dialog_)
    showStitchingCalibrationDialog();
  calibration_dialog_failed_ = true;
  if (calibration_pending_ && !active_run_game_id_.isEmpty())
    saveStitchingCalibrationState(
        active_run_game_id_,
        active_calibration_control_points_,
        "failed",
        active_calibration_start_stage_,
        active_calibration_invalidation_id_,
        true,
        /*require_matching_pending=*/true);
  if (!active_calibration_stage_.isEmpty())
    setStitchingCalibrationStage(active_calibration_stage_, "failed", {});
  calibration_icon_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(32, 32));
  calibration_icon_->setProperty("calibrationState", "failed");
  calibration_headline_->setText("Stitching calibration failed");
  calibration_headline_->setProperty("calibrationState", "failed");
  calibration_headline_->style()->unpolish(calibration_headline_);
  calibration_headline_->style()->polish(calibration_headline_);
  calibration_detail_->setText(
      QString("%1\n\nThe pipeline log has the full diagnostic details.")
          .arg(message.isEmpty() ? "The native stitching calibration did not complete." : message));
  calibration_progress_->setVisible(false);
  calibration_cancel_button_->setVisible(false);
  calibration_ok_button_->setVisible(true);
  calibration_ok_button_->setEnabled(true);
  static_cast<StitchingCalibrationDialog*>(calibration_dialog_)->setCloseAllowed(true);
  calibration_dialog_->show();
  appendLog(QString("stitching calibration failed: %1").arg(message));
}

void HStreamWindow::closeStitchingCalibrationDialog() {
  if (!calibration_dialog_)
    return;
  static_cast<StitchingCalibrationDialog*>(calibration_dialog_)->setCloseAllowed(true);
  calibration_dialog_->hide();
  active_calibration_stage_.clear();
}

QStringList HStreamWindow::pipelineArguments() const {
  const QString game_id = !active_run_game_id_.isEmpty()
      ? active_run_game_id_
      : (game_id_edit_ ? game_id_edit_->text().trimmed() : QString());
  const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
  const bool test_embedded_preview = !qgetenv("HSTREAM_UI_TEST_RUNNER").isEmpty() &&
      qEnvironmentVariableIsSet("HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW");
  const bool embed_render_window =
      hm::ui_internal::supports_x11_embedding(QGuiApplication::platformName(), is_tegra_runtime()) ||
      test_embedded_preview;
  QStringList args;
  args << "-g" << game_id << "--enable-sources=URI-MULTIPLE";
  if (active_force_reconfigure_)
    args << "--force-reconfigure";
  if (!active_calibration_invalidation_id_.isEmpty())
    args << QString("--clean-expected-invalidation-id=%1").arg(active_calibration_invalidation_id_);
  if (isCalibrationRun()) {
    args << "--stitching-calibration-only";
    args << "-c" << pipelineConfigPath("ds_hockey_app_config.yaml");
    args << QString("--enable-sinks=%1").arg(render_video || embed_render_window ? "RENDER" : "FAKE");
    if (render_video || embed_render_window) {
      args << "--show";
    }
  } else {
    args << "-c" << pipelineConfigPath("ds_hockey_app_config.yaml");
    QStringList sinks = enabledSinkNames();
    if (embed_render_window) {
      // Embedded GPU preview mode substitutes a cheap fakesink for the
      // conventional RENDER video sink. Keep the logical sink enabled because
      // its audio side remains the operator's local monitor.
      sinks.removeAll("FAKE");
      if (!sinks.contains("RENDER"))
        sinks.push_front("RENDER");
    }
    args << QString("--enable-sinks=%1").arg(sinks.join(","));
    if (render_video || embed_render_window) {
      args << "--show";
    }
  }
  if (isCalibrationRun() || calibration_pending_) {
    args << QString("--options=%1").arg(kStitchedPreviewPipelineOptions);
    args << QString("--options=pipeline.hmstitcher.calibration-frame-count=%1")
                .arg(
                    active_calibration_frame_count_ > 0 ? active_calibration_frame_count_
                                                        : stitchingCalibrationFrameCount());
  }
  if (!active_stitch_frame_time_.isEmpty() && active_stitch_frame_time_ != default_stitch_frame_time_) {
    args << QString("--stitch-frame-time=%1").arg(active_stitch_frame_time_);
  }
  if (embed_render_window) {
    QStringList preview_windows;
    auto add_window = [&preview_windows](const QString& channel, QWidget* target) {
      if (!target)
        return false;
      const WId window_id = target->winId();
      if (window_id == 0)
        return false;
      preview_windows << QString("%1:%2").arg(channel).arg(static_cast<qulonglong>(window_id));
      return true;
    };
    const bool calibration_only = isCalibrationRun();
    const bool have_required_windows = add_window("stitched", stitched_render_target_) &&
        (calibration_only || add_window("program", preview_render_target_));
    for (int camera_index = 0; camera_index < static_cast<int>(camera_preview_render_targets_.size()); ++camera_index) {
      add_window(QString("source%1").arg(camera_index), camera_preview_render_targets_[camera_index]);
    }
    if (have_required_windows) {
      QString initial_channel = "program";
      if (calibration_only) {
        initial_channel = "stitched";
      } else if (preview_tabs_) {
        initial_channel = hm::ui_internal::preview_channel_for_tab(
            preview_tabs_->currentIndex(), static_cast<int>(camera_preview_render_targets_.size()));
      }
      args << QString("--ui-preview-windows=%1").arg(preview_windows.join(','));
      args << QString("--ui-preview-active=%1")
                  .arg(render_video ? (initial_channel.isEmpty() ? "program" : initial_channel) : "none");
    }
  }
  const bool use_high_bit_grading = cameraControlValue("Use_10_Bit_Grading") != 0;
  args << QString("--options=pipeline.hmstitcher.properties.high-bit-depth=%1").arg(use_high_bit_grading ? 1 : 0);
  if (!isCalibrationRun()) {
    args
        << QString("--options=rink.camera.zoom_in_aggressiveness=%1").arg(cameraControlValue("Zoom_In_Aggressiveness"));
    const QString active_tone_element = use_high_bit_grading ? "hmstitcher" : "hmplaycropper";
    const QString bypassed_tone_element = use_high_bit_grading ? "hmplaycropper" : "hmstitcher";
    args << QString("--options=pipeline.%1.properties.shadow-lift=%2")
                .arg(active_tone_element)
                .arg(cameraControlValue("Bring_Up_Shadows"));
    args << QString("--options=pipeline.%1.properties.shadow-lift-black-point=%2")
                .arg(active_tone_element)
                .arg(cameraControlValue("Lift_Shadow_Black_Point"));
    args << QString("--options=pipeline.%1.properties.exposure=%2")
                .arg(active_tone_element)
                .arg(QString::number(cameraControlValue("Exposure_x100") / 100.0, 'f', 2));
    args << QString("--options=pipeline.%1.properties.shadow-lift=0").arg(bypassed_tone_element);
    args << QString("--options=pipeline.%1.properties.shadow-lift-black-point=0").arg(bypassed_tone_element);
    args << QString("--options=pipeline.%1.properties.exposure=0").arg(bypassed_tone_element);
    if (drivegpt_csv_toggle_ && drivegpt_csv_toggle_->isChecked()) {
      args << QString("--options=pipeline.ds-playtracker.private-properties.telemetry-csv-dir=%1")
                  .arg(QDir::cleanPath(gameDirectory(game_id)));
    }
  }
  args << "--options=pipeline.hmaudio.enable=1";
  return args;
}

void HStreamWindow::startPipeline() {
  if (!pipeline_process_ || pipeline_process_->state() != QProcess::NotRunning ||
      (archive_finalize_process_ && archive_finalize_process_->state() != QProcess::NotRunning)) {
    appendLog(
        archive_finalize_process_ && archive_finalize_process_->state() != QProcess::NotRunning
            ? "the completed archive is still being finalized"
            : "pipeline already running");
    return;
  }
  if (!archive_finalize_blocked_source_path_.isEmpty() && !QFileInfo::exists(archive_finalize_blocked_source_path_)) {
    archive_finalize_blocked_source_path_.clear();
    releaseArchiveFinalizerOwnership(true);
  }
  const auto blocked_archive_toggle = output_toggles_.find("archive-file");
  const bool blocked_archive_enabled = !isCalibrationRun() && blocked_archive_toggle != output_toggles_.end() &&
      blocked_archive_toggle->second && blocked_archive_toggle->second->isChecked();
  if (blocked_archive_enabled && !archive_finalize_blocked_source_path_.isEmpty()) {
    appendLog(QString("archive run blocked until the retained work file is moved to safety: %1")
                  .arg(archive_finalize_blocked_source_path_));
    updateRunControls();
    return;
  }
  pipeline_state_->setText("STARTING");
  resetPlaybackProgress(true);
  setPlaybackStartupStage("ui", "Preparing the game directory and saved configuration");
  const auto show_startup_error = [this](const QString& detail) {
    pipeline_state_->setText("STOPPED");
    resetPlaybackProgress(true);
    setPlaybackProgressState(PlaybackProgressState::kError, detail);
  };
  clearPreviewFrames();
  if (!ensureGameDirectory()) {
    show_startup_error("The selected game directory could not be prepared");
    updateRunControls();
    return;
  }
  active_run_game_id_ = game_id_edit_->text().trimmed();
  active_run_is_calibration_ = isCalibrationRun();
  active_run_high_bit_depth_ = cameraControlValue("Use_10_Bit_Grading") != 0;
  const QStringList active_sinks = enabledSinkNames();
  active_run_local_render_only_ = !active_run_is_calibration_ &&
      (!render_video_toggle_ || render_video_toggle_->isChecked()) && active_sinks.size() == 1 &&
      active_sinks.front() == "RENDER";
  updatePlaybackSeekControls();
  calibration_waiting_for_playback_restart_ = false;
  calibration_playback_restart_observed_ = false;
  active_calibration_control_points_ = 0;
  active_calibration_frame_count_ = stitchingCalibrationFrameCount();
  active_stitch_max_output_width_ = stitchingMaxOutputWidth();
  active_stitch_frame_time_ = stitchFrameTime();
  active_control_point_matcher_ = controlPointMatcher();
  active_mapping_backend_ = mappingBackend();
  active_calibration_start_stage_.clear();
  active_calibration_invalidation_id_.clear();
  active_force_reconfigure_ = active_run_is_calibration_ && calibration_restart_requested_;
  calibration_restart_requested_ = false;
  scoreboard_selector_url_.clear();
  pending_runtime_controls_.clear();
  preview_frame_channels_received_.clear();
  if (preview_tabs_) {
    preview_tabs_->setCurrentIndex(isCalibrationRun() ? 1 : 0);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }

  const QString runner = pipelineRunnerPath();
  if (QFileInfo(runner).isAbsolute() && !QFileInfo::exists(runner)) {
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Pipeline failed to start");
    const QString error_message = QString("pipeline process error=missing runner %1").arg(runner);
    appendLog(error_message);
    show_startup_error(error_message);
    updateRunControls();
    return;
  }
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString working_dir = pipelineWorkingDirectory();
  if (!baseline_config_root_.isEmpty())
    env.insert("HM_CONFIG_ROOT", baseline_config_root_);
  const QString runtime_error = configure_pipeline_runtime_environment(env, working_dir, development_bazel_bin_);
  if (!runtime_error.isEmpty()) {
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    preview_status_->setText("Pipeline failed to start");
    appendLog(runtime_error);
    show_startup_error(runtime_error);
    updateRunControls();
    return;
  }
  setPlaybackStartupStage("stitching", "Validating saved stitching state and Left/Right video assignments");
  active_archive_output_path_.clear();
  active_archive_recovery_path_.clear();
  active_archive_initial_size_ = -1;
  active_archive_initial_mtime_ms_ = -1;
  active_archive_video_is_hevc_ = false;
  const auto archive_toggle = output_toggles_.find("archive-file");
  const bool archive_enabled = !active_run_is_calibration_ && archive_toggle != output_toggles_.end() &&
      archive_toggle->second && archive_toggle->second->isChecked();
  if (archive_enabled) {
    const QString output_work_dir = archive_output_work_dir(env, working_dir);
    env.insert("HM_OUTPUT_WORK_DIR", output_work_dir);
    const QString archive_run_id = QString("%1-%2")
                                       .arg(QCoreApplication::applicationPid())
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    env.insert("HSTREAM_ARCHIVE_RUN_ID", archive_run_id);
    active_archive_output_path_ = archive_output_path(output_work_dir, active_run_game_id_);
    const QString archive_dir = QFileInfo(active_archive_output_path_).absolutePath();
    if (!QDir().mkpath(archive_dir)) {
      output_states_["archive-file"]->setText("ERROR");
      if (archive_output_path_label_)
        archive_output_path_label_->setText(QString("Archive directory could not be created: %1").arg(archive_dir));
      appendLog(QString("archive output directory could not be created: %1").arg(archive_dir));
      active_archive_output_path_.clear();
      active_run_game_id_.clear();
      active_run_is_calibration_ = false;
      pipeline_state_->setText("STOPPED");
      preview_status_->setText("Archive output setup failed");
      show_startup_error(QString("Archive output directory could not be created: %1").arg(archive_dir));
      updateRunControls();
      return;
    }
    beginArchiveJobLog(active_archive_output_path_, archive_run_id);
    output_states_["archive-file"]->setText("WRITING");
    if (archive_output_path_label_)
      archive_output_path_label_->setText(QString("Archive: %1").arg(active_archive_output_path_));
    appendLog(QString("archive output: %1 (the playable file is finalized when playback stops)")
                  .arg(active_archive_output_path_));
  }
  const auto abandon_archive_start = [this](const QString& reason) {
    if (active_archive_output_path_.isEmpty())
      return;
    output_states_["archive-file"]->setText("NO FILE");
    appendLog(
        QString("archive output was not started (%1); expected path: %2").arg(reason, active_archive_output_path_));
    active_archive_output_path_.clear();
    active_archive_initial_size_ = -1;
    active_archive_initial_mtime_ms_ = -1;
  };
  active_calibration_control_points_ = stitchingCalibrationControlPoints();
  active_calibration_frame_count_ = stitchingCalibrationFrameCount();
  active_stitch_max_output_width_ = stitchingMaxOutputWidth();
  bool calibration_required = false;
  if (!prepareStitchingCalibrationRun(runner, working_dir, env, &calibration_required)) {
    abandon_archive_start("stitching setup failed");
    showStitchingCalibrationDialog();
    failStitchingCalibration("Could not prepare the game for stitching calibration.");
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    active_calibration_control_points_ = 0;
    active_calibration_frame_count_ = 0;
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Stitching setup failed");
    if (stitched_status_)
      stitched_status_->setText("Stitched canvas preview");
    show_startup_error("The game could not be prepared for stitching calibration");
    finishArchiveJobLog();
    updateRunControls();
    return;
  }
  saved_stitch_frame_time_ = active_stitch_frame_time_;
  saved_stitching_control_points_ = active_calibration_control_points_;
  saved_stitching_calibration_frame_count_ = active_calibration_frame_count_;
  saved_stitch_max_output_width_ = active_stitch_max_output_width_;
  saved_control_point_matcher_ = active_control_point_matcher_;
  saved_mapping_backend_ = active_mapping_backend_;
  updatePresetDirtyState();
  calibration_pending_ = calibration_required;
  if (calibration_pending_)
    showStitchingCalibrationDialog();

  const QStringList args = pipelineArguments();
  const QString initial_preview_channel = preview_tabs_
      ? hm::ui_internal::preview_channel_for_tab(
            preview_tabs_->currentIndex(), static_cast<int>(camera_preview_render_targets_.size()))
      : QString("program");
  preview_generation_ = 1;
  active_preview_channel_.clear();
  const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
  pending_preview_channel_ =
      render_video ? (initial_preview_channel.isEmpty() ? QString("program") : initial_preview_channel) : QString();
  pending_preview_generation_ = pending_preview_channel_.isEmpty() ? 0 : preview_generation_;
  setPlaybackStartupStage("assets", "Checking pretrained assets and pipeline inputs");
  if (!setupPretrainedAssets(args)) {
    abandon_archive_start("asset setup failed");
    if (calibration_pending_)
      failStitchingCalibration("The pretrained calibration assets could not be prepared.");
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    active_calibration_control_points_ = 0;
    active_calibration_frame_count_ = 0;
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Asset setup failed");
    if (stitched_status_)
      stitched_status_->setText("Stitched canvas preview");
    show_startup_error("Required pretrained assets could not be prepared");
    finishArchiveJobLog();
    updateRunControls();
    return;
  }
  setPlaybackStartupStage("launch", "Preparing GPU preview windows and launching hstream-cli");
  if (!active_run_is_calibration_)
    logMissingTensorRtEngineCaches(args);

  const bool embedded_render = std::any_of(
      args.begin(), args.end(), [](const QString& argument) { return argument.startsWith("--ui-preview-windows="); });
  pipeline_render_embedded_ = embedded_render;
  setAllPreviewFocusAvailable(false);
  if (preview_surface_)
    preview_surface_->hide();
  if (preview_render_target_)
    preview_render_target_->hide();
  if (stitched_surface_)
    stitched_surface_->hide();
  if (stitched_render_target_)
    stitched_render_target_->hide();
  for (QWidget* surface : camera_preview_surfaces_) {
    if (surface)
      surface->hide();
  }
  for (QWidget* target : camera_preview_render_targets_) {
    if (target)
      target->hide();
  }
  if (preview_external_notice_)
    preview_external_notice_->show();
  if (stitched_external_notice_)
    stitched_external_notice_->show();
  for (QLabel* notice : camera_preview_notices_) {
    if (notice)
      notice->show();
  }
  const QString render_notice = !render_video
      ? "Video rendering is disabled"
      : (embedded_render ? "Starting GPU preview…" : "Video is displayed in a separate DeepStream window");
  if (preview_external_notice_)
    preview_external_notice_->setText(render_notice);
  if (stitched_external_notice_)
    stitched_external_notice_->setText(render_notice);
  const QString camera_render_notice = !render_video
      ? "Video rendering is disabled"
      : (embedded_render ? "Starting GPU preview…" : "Camera preview requires embedded X11 rendering");
  for (QLabel* notice : camera_preview_notices_) {
    if (notice)
      notice->setText(camera_render_notice);
  }
  if (render_video && !embedded_render)
    appendLog("render output will open in a separate DeepStream window; embedded preview is disabled");
  else if (!render_video)
    appendLog("video rendering disabled; pipeline will run without a display sink");
  if (calibration_pending_) {
    const int control_points = active_calibration_control_points_;
    const int frame_count =
        active_calibration_frame_count_ > 0 ? active_calibration_frame_count_ : kDefaultStitchCalibrationFrameCount;
    env.insert("HM_MAX_CONTROL_POINTS", QString::number(control_points));
    env.insert("HM_STITCH_CALIBRATION_FRAME_COUNT", QString::number(frame_count));
    env.insert("HSTREAM_CALIBRATION_PENDING", "1");
    env.insert("HSTREAM_CALIBRATION_START_STAGE", active_calibration_start_stage_);
    if (active_run_is_calibration_) {
      appendLog(QString("stitching calibration control points=%1 frames=%2; starting one-pass stitched playback")
                    .arg(control_points)
                    .arg(frame_count));
    } else {
      appendLog(QString(
                    "video inputs require stitching calibration; starting one-pass program playback with control "
                    "points=%1 frames=%2")
                    .arg(control_points)
                    .arg(frame_count));
    }
  } else if (active_run_is_calibration_) {
    appendLog(
        render_video ? "stitching calibration is complete; starting continuous stitched preview"
                     : "stitching calibration is complete; starting without video rendering");
  }
  if (!active_calibration_invalidation_id_.isEmpty())
    env.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", active_calibration_invalidation_id_);
  env.insert("HSTREAM_RENDER_AUDIO_MUTED", render_video ? "0" : "1");
  QStringList preview_overlays;
  if (render_video && !active_run_is_calibration_ && show_player_tracking_toggle_ &&
      show_player_tracking_toggle_->isChecked())
    preview_overlays << "players";
  if (render_video && !active_run_is_calibration_ && show_play_tracking_toggle_ &&
      show_play_tracking_toggle_->isChecked())
    preview_overlays << "play";
  if (render_video && !active_run_is_calibration_ && show_rink_mask_toggle_ && show_rink_mask_toggle_->isChecked())
    preview_overlays << "rink";
  env.insert("HSTREAM_UI_PREVIEW_OVERLAYS", preview_overlays.join(','));
  confirmed_show_player_tracking_ = preview_overlays.contains("players");
  confirmed_show_play_tracking_ = preview_overlays.contains("play");
  confirmed_show_rink_mask_ = preview_overlays.contains("rink");
  pending_preview_overlay_generation_ = 0;
  resetPreviewOverlayReconciliationState();
  appendLog(
      render_video
          ? "audio enabled via pipeline.hmaudio.enable=1; local monitor audio follows Render video"
          : "audio enabled for encoded/stream outputs; local monitor audio is muted because Render video is off");
  env.insert("HSTREAM_UI_PARENT_PID", QString::number(QCoreApplication::applicationPid()));
  pipeline_process_->setProcessEnvironment(env);
  pipeline_process_->setWorkingDirectory(working_dir);
#ifdef Q_OS_UNIX
  const QString setsid = "/usr/bin/setsid";
  const bool test_without_setsid =
      !qgetenv("HSTREAM_UI_TEST_RUNNER").isEmpty() && qEnvironmentVariableIsSet("HSTREAM_UI_TEST_BYPASS_SETSID");
  if (QFileInfo::exists(setsid) && !test_without_setsid) {
    QStringList wrapped_args;
    wrapped_args << runner;
    wrapped_args << args;
    pipeline_process_->setProgram(setsid);
    pipeline_process_->setArguments(wrapped_args);
    pipeline_uses_process_group_ = true;
  } else {
    pipeline_process_->setProgram(runner);
    pipeline_process_->setArguments(args);
    pipeline_uses_process_group_ = false;
  }
#else
  pipeline_process_->setProgram(runner);
  pipeline_process_->setArguments(args);
  pipeline_uses_process_group_ = false;
#endif
  pipeline_paused_ = false;
  pipeline_stop_requested_ = false;

  setPlaybackStartupStage("process", "Starting the pipeline process");
  if (active_run_is_calibration_) {
    if (render_video) {
      preview_status_->setText(
          calibration_pending_ ? "Starting one-pass stitching calibration and preview"
                               : "Starting continuous stitched preview");
    } else {
      preview_status_->setText(
          calibration_pending_ ? "Starting one-pass stitching calibration without video rendering"
                               : "Starting stitching pipeline without video rendering");
    }
  } else {
    preview_status_->setText(
        calibration_pending_ ? "Starting one-pass stitching calibration and program pipeline"
                             : "Starting program pipeline");
  }
  if (isCalibrationRun() && stitched_status_) {
    if (calibration_pending_) {
      stitched_status_->setText(
          QString("Calibrating stitching\nControl points: %1\nPlayback will continue automatically")
              .arg(active_calibration_control_points_));
    } else {
      stitched_status_->setText("Stitching calibrated\nContinuous stitched preview starting");
    }
    if (preview_tabs_) {
      preview_tabs_->setCurrentIndex(1);
    }
  } else if (preview_tabs_) {
    preview_tabs_->setCurrentIndex(0);
  }
  appendLog(QString("pipeline command %1 %2").arg(runner, args.join(' ')));
  pipeline_process_->start();
  updateRunControls();
}

void HStreamWindow::pauseOrResumePipeline() {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    appendLog("pause requested but pipeline is not running");
    return;
  }
  if (pending_playback_seek_generation_ != 0) {
    appendLog("pause requested while a playback seek is still completing");
    return;
  }
#ifdef Q_OS_UNIX
  const qint64 pid = pipeline_process_->processId();
  if (pid <= 0) {
    appendLog("pause requested before process id was available");
    return;
  }
  const int signal = pipeline_paused_ ? SIGCONT : SIGSTOP;
  const pid_t target = pipeline_uses_process_group_ ? -static_cast<pid_t>(pid) : static_cast<pid_t>(pid);
  if (::kill(target, signal) != 0) {
    appendLog("failed to signal pipeline process for pause/resume");
    return;
  }
  pipeline_paused_ = !pipeline_paused_;
  pipeline_state_->setText(pipeline_paused_ ? "PAUSED" : "PLAYING");
  if (!pipeline_paused_) {
    playback_eta_ = "Warming up";
    playback_speed_ = "Warming up";
    beginPlaybackProgressReset();
  }
  preview_status_->setText(pipeline_paused_ ? "Pipeline paused" : "Pipeline resumed");
  appendLog(pipeline_paused_ ? "pipeline paused" : "pipeline resumed");
  updateRunControls();
#else
  appendLog("pause/resume is not supported on this platform yet");
#endif
}

void HStreamWindow::stopPipeline() {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    closeStitchingCalibrationDialog();
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Pipeline stopped");
    if (stitched_status_)
      stitched_status_->setText("Stitched canvas preview");
    appendLog("pipeline already stopped");
    updateRunControls();
    return;
  }
  appendLog("pipeline stop requested");
  pipeline_stop_requested_ = true;
  if (calibration_pending_ && calibration_detail_ && !calibration_dialog_failed_)
    calibration_detail_->setText("Stopping calibration…");
#ifdef Q_OS_UNIX
  const qint64 pid = pipeline_process_->processId();
  const pid_t target = pipeline_uses_process_group_ ? -static_cast<pid_t>(pid) : static_cast<pid_t>(pid);
  if (pipeline_paused_ && pid > 0) {
    ::kill(target, SIGCONT);
    pipeline_paused_ = false;
  }
  if (pid > 0) {
    ::kill(target, SIGINT);
  } else {
    pipeline_process_->terminate();
  }
#else
  pipeline_process_->terminate();
#endif
  if (!pipeline_process_->waitForFinished(15000)) {
    appendLog("pipeline did not exit after terminate; killing");
#ifdef Q_OS_UNIX
    if (pid > 0 && pipeline_uses_process_group_) {
      ::kill(target, SIGKILL);
    }
#endif
    pipeline_process_->kill();
    if (!pipeline_process_->waitForFinished(5000)) {
      appendLog("pipeline still running after kill");
    }
  }
}

void HStreamWindow::handlePipelineStarted() {
  if (pipeline_inspector_) {
    pipeline_inspector_->setPipelineRunning(true);
    if (preview_tabs_ && preview_tabs_->currentIndex() == pipeline_inspector_tab_index_) {
      pipeline_inspector_->requestRefresh();
    }
  }
  pipeline_state_->setText("PLAYING");
  const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
  if (active_run_is_calibration_) {
    const bool previewing = !calibration_pending_;
    if (render_video) {
      preview_status_->setText(previewing ? "Continuous stitched preview running" : "Stitching calibration running");
    } else {
      preview_status_->setText(
          previewing ? "Stitching pipeline running without video rendering"
                     : "Stitching calibration running without video rendering");
    }
    if (stitched_status_) {
      if (render_video) {
        stitched_status_->setText(
            previewing ? "Stitching calibrated\nContinuous stitched preview running"
                       : QString("Calibrating stitching\nControl points: %1\nPlayback will continue automatically")
                             .arg(active_calibration_control_points_));
      } else {
        stitched_status_->setText(
            previewing ? "Stitching calibrated\nVideo rendering disabled"
                       : QString("Calibrating stitching\nControl points: %1\nVideo rendering disabled")
                             .arg(active_calibration_control_points_));
      }
    }
    if (previewing && render_video) {
      appendLog("continuous stitched preview running; camera controls remain available");
    }
  } else {
    if (calibration_pending_) {
      preview_status_->setText(
          render_video ? "Program pipeline calibrating stitching"
                       : "Program pipeline calibrating stitching without video rendering");
    } else {
      preview_status_->setText(
          render_video ? "Program pipeline running" : "Program pipeline running without video rendering");
    }
  }
  appendLog(QString("pipeline started pid=%1").arg(pipeline_process_ ? pipeline_process_->processId() : 0));
  if (pipeline_render_embedded_ && preview_status_)
    preview_status_->setText("Starting GPU preview with the video pipeline");
  if (pipeline_render_embedded_ && render_video)
    setPreviewRenderingLayout(true);
  updateRunControls();
}

void HStreamWindow::handlePipelineFinished(int exit_code, QProcess::ExitStatus exit_status) {
  ++scheduled_rotation_control_generation_;
  scheduled_rotation_controls_.clear();
  scheduled_rotation_controls_ready_ = false;
  ++scheduled_playtracker_control_generation_;
  scheduled_playtracker_controls_.clear();
  scheduled_playtracker_controls_ready_ = false;
  ++scheduled_playcropper_control_generation_;
  scheduled_playcropper_controls_.clear();
  scheduled_playcropper_controls_ready_ = false;
  publishing_playtracker_controls_.reset();
  scheduled_playtracker_force_all_targets_ = false;
  publishing_playtracker_force_all_targets_ = false;
  pending_playback_seek_generation_ = 0;
  playback_seek_recovery_generation_ = 0;
  playback_seek_channel_available_ = false;
  readPipelineOutput();
  if (!pipeline_stdout_buffer_.isEmpty()) {
    appendLog(pipeline_stdout_buffer_.trimmed());
    pipeline_stdout_buffer_.clear();
  }
  if (!pipeline_stderr_buffer_.isEmpty()) {
    appendLog(pipeline_stderr_buffer_.trimmed());
    pipeline_stderr_buffer_.clear();
  }
  if (pipeline_inspector_)
    pipeline_inspector_->setPipelineRunning(false);
  const bool stopped_by_user = pipeline_stop_requested_;
  const bool calibration_ended_incomplete = calibration_pending_ && !stopped_by_user;
  const bool completed_successfully =
      exit_status == QProcess::NormalExit && exit_code == 0 && !stopped_by_user && !calibration_ended_incomplete;
  QString archive_result;
  QString archive_to_finalize;
  QString archive_game_id;
  bool retain_archive_log_guard_for_recovery = false;
  if (!active_archive_output_path_.isEmpty()) {
    const QFileInfo archive_info(active_archive_output_path_);
    const bool output_updated = archive_info.isFile() &&
        (active_archive_initial_size_ < 0 || archive_info.size() != active_archive_initial_size_ ||
         archive_info.lastModified().toMSecsSinceEpoch() != active_archive_initial_mtime_ms_);
    if (output_updated && archive_info.size() > 0) {
      output_states_["archive-file"]->setText(completed_successfully ? "FINALIZING" : "INCOMPLETE");
      archive_result =
          QString("archive %1: %2 (%3 bytes)")
              .arg(
                  completed_successfully ? "container closed; lossless MP4 finalization starting" : "may be incomplete",
                  active_archive_output_path_)
              .arg(archive_info.size());
      if (completed_successfully) {
        archive_to_finalize = active_archive_output_path_;
        archive_game_id = active_run_game_id_;
      } else {
        retain_archive_log_guard_for_recovery = true;
      }
    } else if (output_updated) {
      output_states_["archive-file"]->setText("INCOMPLETE");
      archive_result = QString("archive output is empty and incomplete: %1").arg(active_archive_output_path_);
    } else {
      output_states_["archive-file"]->setText("NO FILE");
      archive_result = archive_info.isFile()
          ? QString("archive output was not updated; existing file remains at: %1").arg(active_archive_output_path_)
          : QString("archive output was not created; expected: %1").arg(active_archive_output_path_);
    }
  }
  pipeline_paused_ = false;
  pipeline_uses_process_group_ = false;
  pipeline_render_embedded_ = false;
  if (scoreboard_selection_dialog_)
    scoreboard_selection_dialog_->closeAfterBackendCompletion();
  clearPreviewFrames();
  pipeline_stop_requested_ = false;
  if (calibration_pending_ && !stopped_by_user) {
    appendLog("one-pass stitching calibration ended before completion; calibration remains pending");
    if (!calibration_dialog_failed_) {
      failStitchingCalibration(QString("The calibration process ended before it finished (exit %1, %2).")
                                   .arg(exit_code)
                                   .arg(exit_status == QProcess::NormalExit ? "normal exit" : "crashed"));
    }
  } else if (calibration_pending_ && stopped_by_user) {
    closeStitchingCalibrationDialog();
  }
  failPendingRuntimeControls("pipeline-finished");
  if (!last_playtracker_runtime_snapshot_.isEmpty()) {
    QFile::remove(last_playtracker_runtime_snapshot_);
  }
  last_playtracker_runtime_snapshot_.clear();
  calibration_pending_ = false;
  calibration_waiting_for_playback_restart_ = false;
  calibration_playback_restart_observed_ = false;
  active_run_game_id_.clear();
  active_run_is_calibration_ = false;
  active_run_high_bit_depth_ = false;
  active_run_local_render_only_ = false;
  active_calibration_control_points_ = 0;
  active_calibration_frame_count_ = 0;
  active_calibration_start_stage_.clear();
  active_calibration_invalidation_id_.clear();
  active_force_reconfigure_ = false;
  pipeline_state_->setText("STOPPED");
  if (completed_successfully) {
    setPlaybackProgressState(PlaybackProgressState::kCompleted);
  } else if (stopped_by_user) {
    setPlaybackProgressState(PlaybackProgressState::kStopped);
  } else {
    setPlaybackProgressState(
        PlaybackProgressState::kError,
        QString("Pipeline exited with code %1 (%2)")
            .arg(exit_code)
            .arg(exit_status == QProcess::NormalExit ? "normal exit" : "crashed"));
  }
  preview_status_->setText("Pipeline stopped");
  if (stitched_status_)
    stitched_status_->setText("Stitched canvas preview");
  appendLog(QString("pipeline finished exit=%1 status=%2")
                .arg(exit_code)
                .arg(exit_status == QProcess::NormalExit ? "normal" : "crashed"));
  if (!archive_result.isEmpty())
    appendLog(archive_result);
  active_archive_output_path_.clear();
  active_archive_initial_size_ = -1;
  active_archive_initial_mtime_ms_ = -1;
  if (!archive_to_finalize.isEmpty()) {
    startArchiveFinalization(archive_to_finalize, archive_game_id, active_archive_video_is_hevc_);
  } else {
    releaseArchiveFinalizerOwnership(true);
    finishArchiveJobLog(!retain_archive_log_guard_for_recovery);
  }
  updateRunControls();
}

void HStreamWindow::clearPreviewFrames() {
  if (preview_focus_mode_) {
    const int restore_tab =
        focused_preview_tab_ >= 0 ? focused_preview_tab_ : (preview_tabs_ ? preview_tabs_->currentIndex() : 0);
    setPreviewFocusMode(false, restore_tab);
  }
  setPreviewRenderingLayout(false);
  setAllPreviewFocusAvailable(false);
  std::vector<QWidget*> surfaces = {preview_surface_, stitched_surface_};
  surfaces.insert(surfaces.end(), camera_preview_surfaces_.begin(), camera_preview_surfaces_.end());
  for (QWidget* surface : surfaces) {
    if (!surface)
      continue;
    surface->setProperty("previewRendererState", "idle");
    surface->setProperty("previewRendererGeneration", 0);
  }
  std::vector<QWidget*> targets = {preview_render_target_, stitched_render_target_};
  targets.insert(targets.end(), camera_preview_render_targets_.begin(), camera_preview_render_targets_.end());
  for (QWidget* target : targets) {
    if (target)
      target->hide();
  }
  preview_frame_channels_received_.clear();
  active_preview_channel_.clear();
  pending_preview_channel_.clear();
  pending_preview_generation_ = 0;
  pending_preview_overlay_generation_ = 0;
  resetPreviewOverlayReconciliationState();
  preview_recovery_attempts_ = 0;
  preview_disable_attempts_ = 0;
  preview_runtime_ready_ = false;
}

void HStreamWindow::handlePipelineError(QProcess::ProcessError error) {
  ++scheduled_rotation_control_generation_;
  scheduled_rotation_controls_.clear();
  scheduled_rotation_controls_ready_ = false;
  ++scheduled_playtracker_control_generation_;
  scheduled_playtracker_controls_.clear();
  scheduled_playtracker_controls_ready_ = false;
  ++scheduled_playcropper_control_generation_;
  scheduled_playcropper_controls_.clear();
  scheduled_playcropper_controls_ready_ = false;
  publishing_playtracker_controls_.reset();
  scheduled_playtracker_force_all_targets_ = false;
  publishing_playtracker_force_all_targets_ = false;
  const QString error_message = QString("pipeline process error=%1 message=%2")
                                    .arg(static_cast<int>(error))
                                    .arg(pipeline_process_ ? pipeline_process_->errorString() : QString());
  if (error != QProcess::FailedToStart && error != QProcess::Crashed) {
    if (error == QProcess::WriteError || error == QProcess::ReadError) {
      failPendingRuntimeControls(error == QProcess::WriteError ? "pipeline-write-error" : "pipeline-read-error");
      if (pending_playback_seek_generation_ != 0 || playback_seek_recovery_generation_ != 0) {
        appendLog(
            error == QProcess::WriteError ? "playback seek failed: pipeline command channel write error"
                                          : "playback seek failed: pipeline command channel read error");
      }
      pending_playback_seek_generation_ = 0;
      playback_seek_recovery_generation_ = 0;
      playback_seek_channel_available_ = false;
      if (error == QProcess::WriteError && render_video_toggle_ && !render_video_toggle_->isChecked() &&
          pending_preview_channel_ == "none" && pending_preview_generation_ != 0) {
        recoverPreviewDisableFailure("the pipeline command channel reported a write error");
      }
    }
    appendLog(error_message + "; pipeline remains running");
    updateRunControls();
    return;
  }
  if (pipeline_inspector_)
    pipeline_inspector_->setPipelineRunning(false);
  if (pipeline_stop_requested_) {
    appendLog(error_message + "; pipeline is stopping at the user's request");
    return;
  }
  if (calibration_pending_ && !calibration_dialog_failed_)
    failStitchingCalibration(QString("The calibration process could not continue: %1").arg(error_message));
  pipeline_paused_ = false;
  pending_playback_seek_generation_ = 0;
  playback_seek_recovery_generation_ = 0;
  playback_seek_channel_available_ = false;
  pipeline_uses_process_group_ = false;
  pipeline_render_embedded_ = false;
  pipeline_stop_requested_ = false;
  clearPreviewFrames();
  if (scoreboard_selection_dialog_)
    scoreboard_selection_dialog_->closeAfterBackendCompletion();
  failPendingRuntimeControls("pipeline-error");
  calibration_pending_ = false;
  calibration_waiting_for_playback_restart_ = false;
  calibration_playback_restart_observed_ = false;
  if (!active_archive_output_path_.isEmpty()) {
    if (error == QProcess::Crashed) {
      output_states_["archive-file"]->setText("CHECKING");
      appendLog(QString("pipeline crashed; checking archive for partial output: %1").arg(active_archive_output_path_));
    } else {
      output_states_["archive-file"]->setText("FAILED");
      appendLog(QString("archive output was not created; expected: %1").arg(active_archive_output_path_));
      active_archive_output_path_.clear();
      active_archive_initial_size_ = -1;
      active_archive_initial_mtime_ms_ = -1;
    }
  }
  active_run_game_id_.clear();
  active_run_is_calibration_ = false;
  active_calibration_control_points_ = 0;
  active_calibration_frame_count_ = 0;
  active_calibration_start_stage_.clear();
  active_calibration_invalidation_id_.clear();
  active_force_reconfigure_ = false;
  pipeline_state_->setText("STOPPED");
  setPlaybackProgressState(PlaybackProgressState::kError, error_message);
  preview_status_->setText("Pipeline failed to start");
  if (stitched_status_)
    stitched_status_->setText("Stitched canvas preview");
  appendLog(error_message);
  if (error == QProcess::FailedToStart)
    finishArchiveJobLog();
  updateRunControls();
}

void HStreamWindow::readPipelineOutput() {
  if (!pipeline_process_) {
    return;
  }
  auto drain = [this](QByteArray output, QString* buffer) {
    if (output.isEmpty() || !buffer) {
      return;
    }
    *buffer += QString::fromLocal8Bit(output);
    for (;;) {
      const qsizetype newline = buffer->indexOf('\n');
      if (newline < 0) {
        break;
      }
      QString line = buffer->left(newline);
      buffer->remove(0, newline + 1);
      line.remove('\r');
      if (!line.trimmed().isEmpty()) {
        const QString trimmed = line.trimmed();
        if (handleStartupProgressOutput(trimmed)) {
          continue;
        }
        if (handlePlaybackProgressOutput(trimmed)) {
          continue;
        }
        if (handlePlaybackSeekOutput(trimmed)) {
          continue;
        }
        if (handleGpuPreviewStatus(trimmed)) {
          continue;
        }
        if (handlePreviewOverlayResponse(trimmed)) {
          continue;
        }
        if (pipeline_inspector_ && pipeline_inspector_->handleBackendLine(trimmed)) {
          continue;
        }
        handleArchiveOutputStatus(trimmed);
        appendLog(trimmed);
        handleRuntimeControlResponse(trimmed);
        handleScoreboardSelectorOutput(trimmed);
        handleStitchingCalibrationOutput(trimmed);
      }
    }
  };
  drain(pipeline_process_->readAllStandardOutput(), &pipeline_stdout_buffer_);
  if (pipeline_process_->processChannelMode() != QProcess::MergedChannels) {
    drain(pipeline_process_->readAllStandardError(), &pipeline_stderr_buffer_);
  }
}

bool HStreamWindow::handleStartupProgressOutput(const QString& line) {
  static const QRegularExpression startup_pattern(QStringLiteral(R"(^HSTREAM_STARTUP stage=([^ ]+) message=(.+)$)"));
  const QRegularExpressionMatch match = startup_pattern.match(line);
  if (!match.hasMatch()) {
    return false;
  }
  setPlaybackStartupStage(match.captured(1), match.captured(2));
  appendLog(QString("startup [%1]: %2").arg(match.captured(1), match.captured(2)));
  return true;
}

bool HStreamWindow::handlePlaybackProgressOutput(const QString& line) {
  static const QString prefix = QStringLiteral("HSTREAM_PROGRESS ");
  if (!line.startsWith(prefix)) {
    return false;
  }

  std::map<QString, QString> fields;
  const QStringList tokens = line.mid(prefix.size()).split(' ', Qt::SkipEmptyParts);
  for (const QString& token : tokens) {
    const qsizetype separator = token.indexOf('=');
    if (separator > 0 && separator + 1 < token.size()) {
      fields[token.left(separator)] = token.mid(separator + 1);
    }
  }
  const auto generation_field = fields.find("generation");
  bool generation_ok = false;
  const quint64 generation =
      generation_field == fields.end() ? 0 : generation_field->second.toULongLong(&generation_ok);
  const auto status = fields.find("status");
  if (status != fields.end() && status->second == "reset") {
    if (generation_ok && generation == playback_reset_generation_) {
      pending_playback_reset_generation_ = 0;
      playback_reset_attempts_ = 0;
      playback_warming_after_resume_ = false;
      playback_accept_stale_after_reset_timeout_ = false;
      playback_eta_ = "Warming up";
      playback_speed_ = "Warming up";
      updatePlaybackProgressPresentation();
    }
    return true;
  }
  const auto instance = fields.find("instance");
  if (instance != fields.end() && instance->second != "aggregate") {
    return true;
  }
  if (playback_reset_generation_ > 0 && (!generation_ok || generation < playback_reset_generation_) &&
      !playback_accept_stale_after_reset_timeout_) {
    return true;
  }
  if (generation_ok && generation > playback_reset_generation_) {
    return true;
  }

  auto parse_nanoseconds = [&fields](const QString& name, quint64* value) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->second == "unknown") {
      return false;
    }
    bool ok = false;
    const quint64 parsed = found->second.toULongLong(&ok);
    if (ok && value) {
      *value = parsed;
    }
    return ok;
  };
  auto format_duration = [](quint64 nanoseconds) {
    quint64 total_seconds = nanoseconds / 1000000000ULL;
    const quint64 hours = total_seconds / 3600;
    total_seconds %= 3600;
    const quint64 minutes = total_seconds / 60;
    const quint64 seconds = total_seconds % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
  };

  quint64 processed_ns = 0;
  if (!parse_nanoseconds("processed_ns", &processed_ns)) {
    return true;
  }
  playback_position_ns_ = static_cast<qint64>(std::min<quint64>(processed_ns, std::numeric_limits<qint64>::max()));
  playback_startup_detail_.clear();
  playback_elapsed_ = format_duration(processed_ns);

  quint64 total_ns = 0;
  const bool have_total = parse_nanoseconds("total_ns", &total_ns) && total_ns > 0;
  playback_duration_ns_ =
      have_total ? static_cast<qint64>(std::min<quint64>(total_ns, std::numeric_limits<qint64>::max())) : 0;
  playback_total_ = have_total ? format_duration(total_ns) : "Unknown";

  quint64 remaining_ns = 0;
  playback_remaining_ = parse_nanoseconds("remaining_ns", &remaining_ns) ? format_duration(remaining_ns) : "Unknown";
  quint64 eta_ns = 0;
  playback_eta_ =
      parse_nanoseconds("eta_ns", &eta_ns) ? format_duration(eta_ns) : (have_total ? "Warming up" : "Unknown");

  const auto speed = fields.find("speed_x");
  bool speed_ok = false;
  const double speed_value = speed == fields.end() ? 0.0 : speed->second.toDouble(&speed_ok);
  playback_speed_ = speed_ok && speed_value > 0.0 ? QString::number(speed_value, 'f', 2) + "x" : "Warming up";
  const auto fps = fields.find("fps");
  bool fps_ok = false;
  const double fps_value = fps == fields.end() ? 0.0 : fps->second.toDouble(&fps_ok);
  playback_fps_ = fps_ok && std::isfinite(fps_value) && fps_value > 0.0
      ? QString::number(fps_value, 'f', 2)
      : (fps == fields.end() ? "Unknown" : "Warming up");
  const auto fps_average = fields.find("fps_avg");
  bool fps_average_ok = false;
  const double fps_average_value = fps_average == fields.end() ? 0.0 : fps_average->second.toDouble(&fps_average_ok);
  playback_fps_average_ = fps_average_ok && std::isfinite(fps_average_value) && fps_average_value > 0.0
      ? QString::number(fps_average_value, 'f', 2)
      : "Unknown";
  const auto stage = fields.find("stage");
  playback_stage_ = stage == fields.end() ? "Unknown" : stage->second;
  const auto instances = fields.find("instances");
  playback_instances_ = instances == fields.end() ? "1" : instances->second;
  if (playback_warming_after_resume_) {
    playback_eta_ = "Warming up";
    playback_speed_ = "Warming up";
  }

  const auto fraction = fields.find("fraction");
  bool fraction_ok = false;
  const double fraction_value = fraction == fields.end() ? 0.0 : fraction->second.toDouble(&fraction_ok);
  playback_progress_determinate_ = have_total && fraction_ok && std::isfinite(fraction_value);
  if (playback_progress_determinate_) {
    playback_progress_x10_ = qRound(std::clamp(fraction_value, 0.0, 1.0) * 1000.0);
  }
  setPlaybackProgressState(PlaybackProgressState::kRunning);
  updatePlaybackProgressPresentation();
  updatePlaybackSeekControls();
  return true;
}

bool HStreamWindow::handlePlaybackSeekOutput(const QString& line) {
  static const QString prefix = QStringLiteral("HSTREAM_SEEK ");
  static const QString recovery_prefix = QStringLiteral("HSTREAM_SEEK_RECOVERY ");
  const bool recovery = line.startsWith(recovery_prefix);
  if (!recovery && !line.startsWith(prefix)) {
    return false;
  }
  std::map<QString, QString> fields;
  const qsizetype prefix_size = recovery ? recovery_prefix.size() : prefix.size();
  for (const QString& token : line.mid(prefix_size).split(' ', Qt::SkipEmptyParts)) {
    const qsizetype separator = token.indexOf('=');
    if (separator > 0 && separator + 1 < token.size()) {
      fields[token.left(separator)] = token.mid(separator + 1);
    }
  }
  bool generation_ok = false;
  const quint64 generation = fields["generation"].toULongLong(&generation_ok);
  if (recovery) {
    if (!generation_ok || generation != playback_seek_recovery_generation_) {
      appendLog(QString("ignored stale playback seek recovery: %1").arg(line));
      return true;
    }
    playback_seek_recovery_generation_ = 0;
    appendLog("playback recovered after a timed-out seek reconstruction");
    beginPlaybackProgressReset();
    updatePlaybackSeekControls();
    flushScheduledRuntimeControls();
    return true;
  }
  if (!generation_ok || generation != pending_playback_seek_generation_) {
    appendLog(QString("ignored stale playback seek response: %1").arg(line));
    return true;
  }
  pending_playback_seek_generation_ = 0;
  const QString status = fields["status"];
  if (status == "ok") {
    bool position_ok = false;
    const quint64 position = fields["position_ns"].toULongLong(&position_ok);
    if (position_ok) {
      playback_position_ns_ = static_cast<qint64>(std::min<quint64>(position, std::numeric_limits<qint64>::max()));
    }
    appendLog(QString("playback seek complete at %1").arg(format_video_time_ns(playback_position_ns_)));
    beginPlaybackProgressReset();
  } else {
    if (fields["reason"] == "pipeline-recreate-timeout") {
      playback_seek_recovery_generation_ = generation;
    }
    appendLog(QString("playback seek %1: %2").arg(status, fields["reason"]));
  }
  updatePlaybackSeekControls();
  if (playback_seek_recovery_generation_ == 0) {
    flushScheduledRuntimeControls();
  }
  return true;
}

void HStreamWindow::requestPlaybackSeek(qint64 target_ns) {
  updatePlaybackSeekControls();
  const bool enabled = playback_seek_slider_ && playback_seek_slider_->isEnabled();
  if (!enabled || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    appendLog("playback seek ignored because this run is not local-render-only and seekable");
    return;
  }
  target_ns = std::clamp<qint64>(target_ns, 0, playback_duration_ns_);
  const quint64 generation = ++playback_seek_generation_;
  pending_playback_seek_generation_ = generation;
  const QByteArray command = QString("@seek %1 %2\n").arg(target_ns).arg(generation).toUtf8();
  if (pipeline_process_->write(command) != command.size()) {
    pending_playback_seek_generation_ = 0;
    appendLog("playback seek failed because the pipeline command could not be written");
    updatePlaybackSeekControls();
    return;
  }
  appendLog(QString("playback seek requested at %1").arg(format_video_time_ns(target_ns)));
  updatePlaybackSeekControls();
}

void HStreamWindow::requestPlaybackSeekRelative(qint64 delta_ns) {
  updatePlaybackSeekControls();
  const bool enabled = playback_seek_slider_ && playback_seek_slider_->isEnabled();
  if (!enabled || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    appendLog("relative playback seek ignored because this run is not local-render-only and seekable");
    return;
  }
  if (delta_ns == 0) {
    return;
  }
  const quint64 generation = ++playback_seek_generation_;
  pending_playback_seek_generation_ = generation;
  const QByteArray command = QString("@seek-relative %1 %2\n").arg(delta_ns).arg(generation).toUtf8();
  if (pipeline_process_->write(command) != command.size()) {
    pending_playback_seek_generation_ = 0;
    appendLog("relative playback seek failed because the pipeline command could not be written");
    updatePlaybackSeekControls();
    return;
  }
  appendLog(QString("playback jump requested %1 seconds").arg(static_cast<double>(delta_ns) / 1000000000.0));
  updatePlaybackSeekControls();
}

void HStreamWindow::updatePlaybackSeekControls() {
  if (!playback_seek_controls_ || !playback_seek_slider_) {
    return;
  }
  const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  if (pause_button_) {
    pause_button_->setEnabled(
        running && pending_playback_seek_generation_ == 0 && playback_seek_recovery_generation_ == 0);
  }
  if (program_control_tabs_) {
    program_control_tabs_->setEnabled(
        pending_playback_seek_generation_ == 0 && playback_seek_recovery_generation_ == 0);
  }
  if (stitched_control_tabs_) {
    stitched_control_tabs_->setEnabled(
        pending_playback_seek_generation_ == 0 && playback_seek_recovery_generation_ == 0);
  }
  playback_seek_controls_->setVisible(running);
  if (QWidget* transport = playback_seek_controls_->parentWidget()) {
    transport->setVisible(running || (playback_progress_ && !playback_progress_->isHidden()));
  }
  const bool rendering = !render_video_toggle_ || render_video_toggle_->isChecked();
  const bool allowed = running && !pipeline_paused_ && active_run_local_render_only_ && !active_run_is_calibration_ &&
      !calibration_pending_ && rendering && playback_seek_channel_available_ && playback_duration_ns_ > 0 &&
      pending_playback_seek_generation_ == 0 && playback_seek_recovery_generation_ == 0;
  playback_seek_slider_->setEnabled(allowed);
  if (playback_seek_back_button_)
    playback_seek_back_button_->setEnabled(allowed);
  if (playback_seek_forward_button_)
    playback_seek_forward_button_->setEnabled(allowed);
  if (!playback_seek_slider_->isSliderDown() && playback_duration_ns_ > 0) {
    const long double fraction =
        static_cast<long double>(std::clamp<qint64>(playback_position_ns_, 0, playback_duration_ns_)) /
        static_cast<long double>(playback_duration_ns_);
    const bool blocked = playback_seek_slider_->blockSignals(true);
    playback_seek_slider_->setValue(
        static_cast<int>(std::llround(fraction * static_cast<long double>(playback_seek_slider_->maximum()))));
    playback_seek_slider_->blockSignals(blocked);
  }
  if (playback_seek_position_) {
    playback_seek_position_->setText(
        playback_duration_ns_ > 0
            ? QString("%1 / %2").arg(
                  format_video_time_ns(playback_position_ns_), format_video_time_ns(playback_duration_ns_))
            : "00:00:00 / --:--:--");
  }
  QString reason;
  if (!running) {
    reason = "Start a Program run with only Render video enabled to seek.";
  } else if (!active_run_local_render_only_ || active_run_is_calibration_) {
    reason = "Seeking is disabled because this run includes a nonlocal output or is not Program playback.";
  } else if (!rendering) {
    reason = "Enable Render video to seek.";
  } else if (!playback_seek_channel_available_) {
    reason = "Seeking is unavailable because the pipeline command channel failed.";
  } else if (calibration_pending_) {
    reason = "Seeking becomes available after one-pass stitching calibration finishes.";
  } else if (playback_duration_ns_ <= 0) {
    reason = "Waiting for the video duration before enabling seek.";
  } else if (pending_playback_seek_generation_ != 0) {
    reason = "Waiting for the current seek to finish.";
  } else if (playback_seek_recovery_generation_ != 0) {
    reason = "The seek timed out; waiting for the backend reconstruction worker to return safely.";
  } else if (pipeline_paused_) {
    reason = "Resume playback before seeking.";
  } else {
    reason =
        "Drag to seek relative to the configured run start, or jump ten seconds backward or forward. Local "
        "rendering is the only active output.";
  }
  set_control_help(playback_seek_controls_, reason);
  set_control_help(playback_seek_slider_, reason);
}

void HStreamWindow::setPlaybackStartupStage(const QString& stage, const QString& detail) {
  playback_stage_ = stage;
  playback_startup_detail_ = detail;
  updatePlaybackProgressPresentation();
  if (playback_progress_)
    playback_progress_->repaint();
}

void HStreamWindow::setPlaybackProgressState(PlaybackProgressState state, const QString& detail) {
  playback_progress_state_ = state;
  playback_terminal_detail_ = detail;
  if (state == PlaybackProgressState::kCompleted) {
    if (playback_total_ != "Unknown") {
      playback_elapsed_ = playback_total_;
    }
    playback_remaining_ = "00:00:00";
    playback_eta_ = "00:00:00";
    playback_progress_x10_ = 1000;
    playback_progress_determinate_ = true;
  }
  if (!playback_progress_) {
    return;
  }
  const char* property_value = "idle";
  switch (state) {
    case PlaybackProgressState::kRunning:
      property_value = "running";
      break;
    case PlaybackProgressState::kCompleted:
      property_value = "completed";
      break;
    case PlaybackProgressState::kError:
      property_value = "error";
      break;
    case PlaybackProgressState::kStopped:
      property_value = "stopped";
      break;
    case PlaybackProgressState::kIdle:
      break;
  }
  if (playback_progress_->property("playbackState").toString() != QString::fromLatin1(property_value)) {
    playback_progress_->setProperty("playbackState", property_value);
    playback_progress_->style()->unpolish(playback_progress_);
    playback_progress_->style()->polish(playback_progress_);
    playback_progress_->update();
  }
  updatePlaybackProgressPresentation();
}

void HStreamWindow::resetPlaybackProgress(bool starting) {
  playback_elapsed_ = "Waiting for first frame";
  playback_total_ = "Unknown";
  playback_remaining_ = "Unknown";
  playback_eta_ = "Warming up";
  playback_speed_ = "Warming up";
  playback_fps_ = "Warming up";
  playback_fps_average_ = "Unknown";
  playback_stage_ = "Unknown";
  playback_startup_detail_.clear();
  playback_instances_ = "Unknown";
  playback_progress_x10_ = 0;
  playback_progress_determinate_ = false;
  playback_warming_after_resume_ = false;
  playback_accept_stale_after_reset_timeout_ = false;
  playback_reset_generation_ = 0;
  pending_playback_reset_generation_ = 0;
  playback_reset_attempts_ = 0;
  playback_position_ns_ = 0;
  playback_duration_ns_ = 0;
  pending_playback_seek_generation_ = 0;
  playback_seek_recovery_generation_ = 0;
  playback_seek_channel_available_ = starting;
  if (playback_progress_) {
    playback_progress_->setRange(0, starting ? 0 : 1000);
    playback_progress_->setValue(0);
  }
  setPlaybackProgressState(starting ? PlaybackProgressState::kRunning : PlaybackProgressState::kIdle);
  updatePlaybackSeekControls();
}

int HStreamWindow::playbackProgressResetTimeoutMs() const {
  bool test_timeout_valid = false;
  const int test_timeout =
      qEnvironmentVariableIntValue("HSTREAM_UI_TEST_PROGRESS_RESET_TIMEOUT_MS", &test_timeout_valid);
  return test_timeout_valid && test_timeout > 0 ? test_timeout : 5000;
}

void HStreamWindow::beginPlaybackProgressReset() {
  playback_warming_after_resume_ = true;
  playback_accept_stale_after_reset_timeout_ = false;
  pending_playback_reset_generation_ = ++playback_reset_generation_;
  playback_reset_attempts_ = 0;
  sendPlaybackProgressReset(pending_playback_reset_generation_);
}

void HStreamWindow::sendPlaybackProgressReset(quint64 generation) {
  if (generation == 0 || generation != pending_playback_reset_generation_ || !pipeline_process_ ||
      pipeline_process_->state() == QProcess::NotRunning || pipeline_paused_) {
    return;
  }
  constexpr int kResetAttemptLimit = 3;
  if (playback_reset_attempts_ >= kResetAttemptLimit) {
    pending_playback_reset_generation_ = 0;
    playback_warming_after_resume_ = false;
    playback_accept_stale_after_reset_timeout_ = true;
    appendLog("playback speed reset was not acknowledged; using recovered adjacent-sample rate");
    updatePlaybackProgressPresentation();
    return;
  }

  ++playback_reset_attempts_;
  const QByteArray command = QString("@reset-progress-rate %1\n").arg(generation).toUtf8();
  if (pipeline_process_->write(command) != command.size() && playback_reset_attempts_ == 1) {
    appendLog("playback speed reset write was delayed; retrying safely");
  }
  QTimer::singleShot(playbackProgressResetTimeoutMs(), this, [this, generation]() {
    if (generation == pending_playback_reset_generation_) {
      sendPlaybackProgressReset(generation);
    }
  });
}

void HStreamWindow::updatePlaybackProgressPresentation() {
  if (!playback_progress_) {
    return;
  }
  const QString pipeline_state = pipeline_state_ ? pipeline_state_->text() : QString("STARTING");
  const bool paused = pipeline_state == "PAUSED";
  const QString eta = paused ? "Paused" : playback_eta_;
  const QString speed = paused ? "Paused" : playback_speed_;
  const QString fps = paused ? "Paused" : playback_fps_;
  auto fps_label = [](const QString& value) {
    if (value == "Paused") {
      return QString("FPS paused");
    }
    if (value == "Warming up") {
      return QString("FPS warming up");
    }
    if (value == "Unknown") {
      return QString("FPS unknown");
    }
    return QString("%1 FPS").arg(value);
  };
  const QString active_fps = fps_label(fps);
  const QString active_eta = eta == "Paused" ? "ETA paused" : QString("ETA %1").arg(eta.toLower());

  switch (playback_progress_state_) {
    case PlaybackProgressState::kRunning:
      if (playback_progress_determinate_) {
        playback_progress_->setRange(0, 1000);
        playback_progress_->setValue(playback_progress_x10_);
        playback_progress_->setFormat(QString("%1%2 / %3  •  %4%  •  %5  •  %6")
                                          .arg(paused ? "PAUSED  •  " : "", playback_elapsed_, playback_total_)
                                          .arg(playback_progress_x10_ / 10.0, 0, 'f', 1)
                                          .arg(active_fps, active_eta));
      } else {
        playback_progress_->setRange(0, 0);
        if (!playback_startup_detail_.isEmpty()) {
          playback_progress_->setFormat(
              QString("STARTING  •  %1  •  FPS warming up  •  ETA warming up").arg(playback_startup_detail_));
        } else {
          playback_progress_->setFormat(
              pipeline_state == "STARTING"
                  ? "STARTING  •  Waiting for first frame  •  FPS warming up  •  ETA warming up"
                  : QString("%1%2 elapsed  •  %3  •  %4")
                        .arg(paused ? "PAUSED  •  " : "", playback_elapsed_, active_fps, active_eta));
        }
      }
      break;
    case PlaybackProgressState::kCompleted:
      playback_progress_->setRange(0, 1000);
      playback_progress_->setValue(1000);
      playback_progress_->setFormat(
          QString("COMPLETED  •  %1  •  %2  •  ETA 00:00:00").arg(playback_elapsed_, fps_label(playback_fps_)));
      break;
    case PlaybackProgressState::kError:
      playback_progress_->setRange(0, 1000);
      playback_progress_->setValue(1000);
      playback_progress_->setFormat(
          playback_elapsed_ == "Waiting for first frame"
              ? "ERROR  •  Pipeline did not start"
              : QString("ERROR  •  %1 elapsed  •  %2").arg(playback_elapsed_, fps_label(playback_fps_)));
      break;
    case PlaybackProgressState::kStopped:
      playback_progress_->setFormat("STOPPED");
      break;
    case PlaybackProgressState::kIdle:
      playback_progress_->setRange(0, 1000);
      playback_progress_->setValue(0);
      playback_progress_->setFormat("No pipeline run yet");
      break;
  }
  QString state_label = pipeline_state;
  if (playback_progress_state_ == PlaybackProgressState::kCompleted) {
    state_label = "COMPLETED";
  } else if (playback_progress_state_ == PlaybackProgressState::kError) {
    state_label = "ERROR";
  }
  playback_progress_->setToolTip(
      QString(
          "Pipeline: %1\nStage: %2\nActive pipelines: %3\nElapsed: %4\nTotal: %5\nRemaining: %6\nETA: %7\n"
          "Output FPS: %8 (average %9)\nProcessing speed: %10%11")
          .arg(state_label, playback_stage_, playback_instances_, playback_elapsed_, playback_total_)
          .arg(playback_remaining_, eta, fps, playback_fps_average_)
          .arg(
              speed,
              playback_terminal_detail_.isEmpty() ? QString()
                                                  : QString("\nDetails: %1").arg(playback_terminal_detail_)));
  playback_progress_->setAccessibleDescription(playback_progress_->format());
}

void HStreamWindow::handleArchiveOutputStatus(const QString& line) {
  static const QRegularExpression recovery_status(R"(^HSTREAM_OUTPUT_RECOVERY type=archive sink=(-?\d+) path=(.+)$)");
  const QRegularExpressionMatch recovery_match = recovery_status.match(line);
  if (recovery_match.hasMatch() && !active_archive_output_path_.isEmpty()) {
    active_archive_recovery_path_ = QFileInfo(recovery_match.captured(2)).absoluteFilePath();
    if (archive_output_path_label_) {
      archive_output_path_label_->setText(QString("Archive: %1\nPrevious archive retained for recovery: %2")
                                              .arg(active_archive_output_path_, active_archive_recovery_path_));
    }
    appendLog(QString("pre-existing archive work file preserved for recovery: %1").arg(active_archive_recovery_path_));
    return;
  }
  static const QRegularExpression output_status(
      R"(^HSTREAM_OUTPUT type=archive sink=(-?\d+) existed=([01]) size=(-?\d+) mtime-ms=(-?\d+)(?: codec=(h264|hevc|unknown))? path=(.+)$)");
  const QRegularExpressionMatch match = output_status.match(line);
  if (!match.hasMatch() || active_archive_output_path_.isEmpty()) {
    return;
  }
  const QString resolved_path = QFileInfo(match.captured(6)).absoluteFilePath();
  if (resolved_path.isEmpty()) {
    return;
  }
  resolveArchiveJobLogPath(resolved_path);
  QString ownership_error;
  if (!acquireArchiveFinalizerOwnership(resolved_path, &ownership_error)) {
    appendLog(QString("archive finalizer ownership could not be established: %1").arg(ownership_error));
  }
  const bool path_changed = resolved_path != active_archive_output_path_;
  active_archive_output_path_ = resolved_path;
  const bool output_existed = match.captured(2) == "1";
  active_archive_initial_size_ = output_existed ? match.captured(3).toLongLong() : -1;
  active_archive_initial_mtime_ms_ = output_existed ? match.captured(4).toLongLong() : -1;
  active_archive_video_is_hevc_ = match.captured(5) == "hevc";
  if (archive_output_path_label_) {
    archive_output_path_label_->setText(
        active_archive_recovery_path_.isEmpty() ? QString("Archive: %1").arg(active_archive_output_path_)
                                                : QString("Archive: %1\nPrevious archive retained for recovery: %2")
                                                      .arg(active_archive_output_path_, active_archive_recovery_path_));
  }
  appendLog(QString("archive backend %1 output: %2")
                .arg(path_changed ? "resolved" : "confirmed", active_archive_output_path_));
}

void HStreamWindow::beginArchiveJobLog(const QString& configured_output_path, const QString& run_id) {
  finishArchiveJobLog();
  archive_job_log_enabled_ = true;

  const QFileInfo output_info(configured_output_path);
  QString safe_run_id = run_id.trimmed();
  safe_run_id.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
  if (safe_run_id.isEmpty())
    safe_run_id = "unknown";
  const QString extension = output_info.suffix().isEmpty() ? QString() : "." + output_info.suffix();
  archive_job_log_path_ = output_info.dir().filePath(
      QString("%1.hstream-run-ui-%2%3.log").arg(output_info.completeBaseName(), safe_run_id, extension));
  archive_job_log_.setFileName(archive_job_log_path_);
  if (!archive_job_log_.open(
          QIODevice::WriteOnly | QIODevice::Text | QIODevice::NewOnly,
          QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    const QString error = archive_job_log_.errorString();
    archive_job_log_enabled_ = false;
    archive_job_log_path_.clear();
    appendLog(QString("archive job log could not be created: %1").arg(error));
    return;
  }
#ifdef Q_OS_UNIX
  struct stat initial_log_stat{};
  if (::fstat(archive_job_log_.handle(), &initial_log_stat) != 0 || !S_ISREG(initial_log_stat.st_mode)) {
    const QString identity_error = QString::fromLocal8Bit(std::strerror(errno));
    archive_job_log_.close();
    archive_job_log_enabled_ = false;
    archive_job_log_path_.clear();
    appendLog(QString("archive job log could not be identity-pinned: %1").arg(identity_error));
    return;
  }
  archive_job_log_device_ = static_cast<quint64>(initial_log_stat.st_dev);
  archive_job_log_inode_ = static_cast<quint64>(initial_log_stat.st_ino);
  QString guard_error;
  if (!create_open_file_guard(
          archive_job_log_.handle(), archive_job_log_path_, &archive_job_log_guard_path_, &guard_error)) {
    QString cleanup_error;
    if (!remove_path_if_same_identity(archive_job_log_path_, initial_log_stat, &cleanup_error) &&
        !cleanup_error.isEmpty()) {
      guard_error += QString("; created log was retained: %1").arg(cleanup_error);
    }
    archive_job_log_.close();
    archive_job_log_enabled_ = false;
    archive_job_log_path_.clear();
    archive_job_log_guard_path_.clear();
    appendLog(QString("archive job log could not be identity-pinned: %1").arg(guard_error));
    return;
  }
#endif
  appendLog(QString("archive job log: %1").arg(archive_job_log_path_));
#ifdef Q_OS_UNIX
  QString durability_error;
  if (!sync_open_file(archive_job_log_, &durability_error) ||
      !sync_parent_directory(archive_job_log_path_, &durability_error)) {
    appendLog(QString("archive job log initial durability sync failed: %1").arg(durability_error));
  }
#endif
}

void HStreamWindow::resolveArchiveJobLogPath(const QString& resolved_output_path) {
  if (!archive_job_log_enabled_ || resolved_output_path.isEmpty())
    return;
  const QString resolved_log_path = QFileInfo(resolved_output_path).absoluteFilePath() + ".log";
  if (archive_job_log_path_ == resolved_log_path)
    return;

  const QString provisional_path = archive_job_log_path_;
  quint64 expected_log_device = 0;
  quint64 expected_log_inode = 0;
  int pinned_log_fd = -1;
  struct stat open_log_stat{};
  QString durability_error;
#ifdef Q_OS_UNIX
  if (archive_job_log_.isOpen()) {
    if (::fstat(archive_job_log_.handle(), &open_log_stat) == 0) {
      expected_log_device = static_cast<quint64>(open_log_stat.st_dev);
      expected_log_inode = static_cast<quint64>(open_log_stat.st_ino);
      if (!archive_job_log_guard_path_.isEmpty()) {
        const QByteArray encoded_guard = QFile::encodeName(archive_job_log_guard_path_);
        pinned_log_fd = ::open(encoded_guard.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat pinned_log_stat{};
        if (pinned_log_fd < 0 || ::fstat(pinned_log_fd, &pinned_log_stat) != 0 ||
            !same_file_identity(pinned_log_stat, open_log_stat)) {
          if (pinned_log_fd >= 0)
            ::close(pinned_log_fd);
          pinned_log_fd = -1;
        }
      }
    }
    sync_open_file(archive_job_log_, &durability_error);
  }
#endif
  bool renamed = false;
  bool cross_filesystem_copy_ready = false;
  bool copied_across_filesystems = false;
#ifdef Q_OS_UNIX
  int rename_errno = 0;
  const QString provisional_guard_path = archive_job_log_guard_path_;
  const QString resolved_guard_path = resolved_log_path + ".hstream-pin";
  const auto restore_provisional_log = [&](int* saved_errno, QString* error) {
    if (path_has_file_identity(provisional_path, open_log_stat))
      return sync_parent_directory(provisional_path, error);
    if (!link_open_file_no_replace(pinned_log_fd, provisional_path, saved_errno) ||
        !path_has_file_identity(provisional_path, open_log_stat)) {
      if (error && error->isEmpty())
        *error = QString::fromLocal8Bit(std::strerror(*saved_errno));
      return false;
    }
    return sync_parent_directory(provisional_path, error);
  };
  bool resolved_guard_published = false;
  if (!provisional_path.isEmpty() && expected_log_inode != 0 && pinned_log_fd >= 0) {
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_COLLISION")) {
      qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_COLLISION");
      const QByteArray encoded_guard = QFile::encodeName(resolved_guard_path);
      const int collision_fd =
          ::open(encoded_guard.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
      if (collision_fd >= 0) {
        constexpr char kCollision[] = "injected foreign resolved log guard";
        const ssize_t collision_bytes = ::write(collision_fd, kCollision, sizeof(kCollision) - 1);
        (void)collision_bytes;
        ::close(collision_fd);
      }
    }
    int guard_link_errno = 0;
    resolved_guard_published = link_open_file_no_replace(pinned_log_fd, resolved_guard_path, &guard_link_errno) &&
        path_has_file_identity(resolved_guard_path, open_log_stat);
    if (!resolved_guard_published) {
      rename_errno = guard_link_errno;
      if (guard_link_errno != EXDEV) {
        durability_error = QString("could not protect resolved log path: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(guard_link_errno)));
      }
    } else {
      QString guard_sync_error;
      const bool force_guard_sync_failure =
          qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_SYNC_FAILURE");
      if (force_guard_sync_failure)
        qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_SYNC_FAILURE");
      const bool guard_is_durable = !force_guard_sync_failure &&
          sync_parent_directory(resolved_guard_path, &guard_sync_error) &&
          path_has_file_identity(resolved_guard_path, open_log_stat);
      if (force_guard_sync_failure)
        guard_sync_error = "resolved log guard sync failure requested by test";
      if (!guard_is_durable) {
        durability_error = guard_sync_error.isEmpty() ? QString("resolved log guard changed before durability sync")
                                                      : guard_sync_error;
        QString guard_cleanup_error;
        if (!remove_path_if_same_identity(resolved_guard_path, open_log_stat, &guard_cleanup_error))
          durability_error += QString("; could not roll back unused resolved log guard: %1").arg(guard_cleanup_error);
        resolved_guard_published = false;
      } else {
        const bool rename_succeeded = rename_path_no_replace(provisional_path, resolved_log_path, &rename_errno);
        if (rename_succeeded &&
            qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_REPLACEMENT_AFTER_RENAME")) {
          qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_REPLACEMENT_AFTER_RENAME");
          const QByteArray encoded_log = QFile::encodeName(resolved_log_path);
          ::unlink(encoded_log.constData());
          const int replacement_fd =
              ::open(encoded_log.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
          if (replacement_fd >= 0) {
            constexpr char kReplacement[] = "injected foreign resolved log after rename";
            const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
            (void)replacement_bytes;
            ::close(replacement_fd);
          }
        }
        renamed = rename_succeeded && path_has_file_identity(resolved_log_path, open_log_stat);
        if (!renamed) {
          if (rename_succeeded) {
            QString restore_error;
            int restore_errno = 0;
            if (!restore_provisional_log(&restore_errno, &restore_error)) {
              durability_error =
                  QString("resolved log was replaced after rename; provisional restore failed: %1").arg(restore_error);
            } else {
              QString guard_cleanup_error;
              if (!remove_path_if_same_identity(
                      resolved_guard_path, open_log_stat, &guard_cleanup_error, provisional_path, &open_log_stat)) {
                durability_error = QString("resolved log was replaced; guard retained: %1").arg(guard_cleanup_error);
              } else {
                resolved_guard_published = false;
              }
            }
          } else {
            QString guard_cleanup_error;
            if (!remove_path_if_same_identity(resolved_guard_path, open_log_stat, &guard_cleanup_error) &&
                durability_error.isEmpty()) {
              durability_error = QString("could not roll back unused resolved log guard: %1").arg(guard_cleanup_error);
            }
            resolved_guard_published = false;
          }
        }
      }
    }
  }
  int copied_log_fd = -1;
  struct stat copied_log_stat{};
  QString copied_log_guard_path;
  const auto rollback_cross_filesystem_copy = [&]() {
    QString rollback_error;
    if (path_has_file_identity(resolved_log_path, copied_log_stat)) {
      QString cleanup_error;
      if (!remove_path_if_same_identity(resolved_log_path, copied_log_stat, &cleanup_error))
        rollback_error = cleanup_error;
    }
    if (!copied_log_guard_path.isEmpty() && path_has_file_identity(copied_log_guard_path, copied_log_stat)) {
      QString cleanup_error;
      if (!remove_path_if_same_identity(copied_log_guard_path, copied_log_stat, &cleanup_error) &&
          rollback_error.isEmpty()) {
        rollback_error = cleanup_error;
      }
    }
    QString directory_sync_error;
    if (!sync_parent_directory(resolved_log_path, &directory_sync_error)) {
      rollback_error = rollback_error.isEmpty()
          ? directory_sync_error
          : QString("%1; destination rollback sync failed: %2").arg(rollback_error, directory_sync_error);
    }
    return rollback_error;
  };
  if (!renamed && rename_errno == EXDEV && pinned_log_fd >= 0 && expected_log_inode != 0) {
    int copy_errno = 0;
    QString copy_rollback_error;
    if (copy_open_file_no_replace(
            pinned_log_fd, resolved_log_path, &copied_log_fd, &copied_log_stat, &copy_errno, &copy_rollback_error)) {
      QString copied_guard_error;
      if (create_open_file_guard(copied_log_fd, resolved_log_path, &copied_log_guard_path, &copied_guard_error)) {
        cross_filesystem_copy_ready = true;
      } else {
        const QString rollback_error = rollback_cross_filesystem_copy();
        ::close(copied_log_fd);
        copied_log_fd = -1;
        durability_error = QString("could not protect cross-filesystem log copy: %1").arg(copied_guard_error);
        if (!rollback_error.isEmpty())
          durability_error += QString("; rollback remains unresolved: %1").arg(rollback_error);
      }
    } else {
      durability_error = QString("could not copy resolved log across filesystems: %1")
                             .arg(QString::fromLocal8Bit(std::strerror(copy_errno)));
      if (!copy_rollback_error.isEmpty())
        durability_error += QString("; rollback remains unresolved: %1").arg(copy_rollback_error);
      QString rollback_sync_error;
      if (!sync_parent_directory(resolved_log_path, &rollback_sync_error))
        durability_error += QString("; destination rollback sync failed: %1").arg(rollback_sync_error);
    }
  }
#else
  renamed = !provisional_path.isEmpty() && QFile::rename(provisional_path, resolved_log_path);
#endif
  if (archive_job_log_.isOpen()) {
    archive_job_log_.flush();
    archive_job_log_.close();
  }
  archive_job_log_path_ = renamed ? resolved_log_path : provisional_path;
  quint64 selected_log_device = expected_log_device;
  quint64 selected_log_inode = expected_log_inode;
#ifdef Q_OS_UNIX
  if (renamed) {
    QString publication_error;
    const bool force_publication_sync_failure =
        qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_SAME_FILESYSTEM_SYNC_FAILURE");
    if (force_publication_sync_failure)
      qunsetenv("HSTREAM_UI_TEST_ARCHIVE_SAME_FILESYSTEM_SYNC_FAILURE");
    bool publication_synced = !force_publication_sync_failure &&
        sync_parent_directory(resolved_log_path, &publication_error) &&
        (QFileInfo(provisional_path).absolutePath() == QFileInfo(resolved_log_path).absolutePath() ||
         sync_parent_directory(provisional_path, &publication_error)) &&
        path_has_file_identity(resolved_log_path, open_log_stat) &&
        path_has_file_identity(resolved_guard_path, open_log_stat);
    if (force_publication_sync_failure)
      publication_error = "same-filesystem log publication sync failure requested by test";
    if (publication_synced) {
      archive_job_log_guard_path_ = resolved_guard_path;
      if (!provisional_guard_path.isEmpty() && path_has_file_identity(provisional_guard_path, open_log_stat)) {
        QString old_guard_cleanup_error;
        if (!remove_path_if_same_identity(
                provisional_guard_path, open_log_stat, &old_guard_cleanup_error, resolved_log_path, &open_log_stat)) {
          appendLog(QString("provisional archive log guard retained at %1: %2")
                        .arg(provisional_guard_path, old_guard_cleanup_error));
        }
      }
    } else {
      durability_error = publication_error.isEmpty()
          ? QString("same-filesystem log publication was replaced before it became durable")
          : publication_error;
      int rollback_errno = 0;
      QString restore_error;
      const bool provisional_restored = restore_provisional_log(&rollback_errno, &restore_error);
      if (provisional_restored && path_has_file_identity(provisional_guard_path, open_log_stat)) {
        QString cleanup_error;
        if (path_has_file_identity(resolved_log_path, open_log_stat) &&
            !remove_path_if_same_identity(resolved_log_path, open_log_stat, &cleanup_error)) {
          durability_error += QString("; resolved log rollback remains unresolved: %1").arg(cleanup_error);
        }
        cleanup_error.clear();
        if (path_has_file_identity(resolved_guard_path, open_log_stat) &&
            !remove_path_if_same_identity(resolved_guard_path, open_log_stat, &cleanup_error)) {
          durability_error += QString("; resolved guard rollback remains unresolved: %1").arg(cleanup_error);
        }
        QString rollback_sync_error;
        if (!sync_parent_directory(provisional_path, &rollback_sync_error) ||
            (QFileInfo(provisional_path).absolutePath() != QFileInfo(resolved_log_path).absolutePath() &&
             !sync_parent_directory(resolved_log_path, &rollback_sync_error))) {
          durability_error += QString("; rollback sync failed: %1").arg(rollback_sync_error);
        }
      } else {
        durability_error +=
            QString("; could not restore the guarded provisional log: %1")
                .arg(restore_error.isEmpty() ? QString::fromLocal8Bit(std::strerror(rollback_errno)) : restore_error);
      }
      renamed = false;
      resolved_guard_published = false;
      archive_job_log_path_ = provisional_path;
      archive_job_log_guard_path_ = provisional_guard_path;
    }
  }
  if (cross_filesystem_copy_ready) {
    QString destination_sync_error;
    bool force_destination_sync_failure =
        qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_SYNC_FAILURE");
    if (force_destination_sync_failure)
      qunsetenv("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_SYNC_FAILURE");
    const bool destination_parent_synced = !force_destination_sync_failure &&
        sync_parent_directory(resolved_log_path, &destination_sync_error) &&
        path_has_file_identity(resolved_log_path, copied_log_stat) &&
        path_has_file_identity(copied_log_guard_path, copied_log_stat);
    if (force_destination_sync_failure)
      destination_sync_error = "cross-filesystem destination sync failure requested by test";
    if (destination_parent_synced) {
      archive_job_log_path_ = resolved_log_path;
      selected_log_device = static_cast<quint64>(copied_log_stat.st_dev);
      selected_log_inode = static_cast<quint64>(copied_log_stat.st_ino);
    } else {
      if (durability_error.isEmpty()) {
        durability_error = destination_sync_error.isEmpty()
            ? QString("cross-filesystem log publication was replaced before it became durable")
            : destination_sync_error;
      }
      const QString rollback_error = rollback_cross_filesystem_copy();
      if (!rollback_error.isEmpty())
        durability_error += QString("; rollback remains unresolved: %1").arg(rollback_error);
      cross_filesystem_copy_ready = false;
    }
  }
#endif
  QString reopen_error;
  bool reopened = reopenArchiveJobLog(archive_job_log_path_, &reopen_error, selected_log_device, selected_log_inode);
#ifdef Q_OS_UNIX
  if (cross_filesystem_copy_ready && reopened) {
    if (path_has_file_identity(resolved_log_path, copied_log_stat) &&
        path_has_file_identity(copied_log_guard_path, copied_log_stat)) {
      copied_across_filesystems = true;
      archive_job_log_device_ = selected_log_device;
      archive_job_log_inode_ = selected_log_inode;
    } else {
      archive_job_log_.close();
      reopened = false;
      reopen_error = "cross-filesystem log publication was replaced while being reopened";
    }
  }
  if (cross_filesystem_copy_ready && !copied_across_filesystems) {
    const QString rollback_error = rollback_cross_filesystem_copy();
    if (!rollback_error.isEmpty())
      reopen_error += QString("; destination rollback remains unresolved: %1").arg(rollback_error);
    archive_job_log_path_ = provisional_path;
    QString provisional_reopen_error;
    reopened =
        reopenArchiveJobLog(provisional_path, &provisional_reopen_error, expected_log_device, expected_log_inode);
    if (!reopened && !provisional_reopen_error.isEmpty())
      reopen_error += QString("; provisional log reopen failed: %1").arg(provisional_reopen_error);
    cross_filesystem_copy_ready = false;
  }
  if (!reopened) {
    // The no-replace rename may have succeeded before the destination was
    // concurrently removed/replaced.  Recover the still-open trusted inode
    // under its provisional name instead of adopting the replacement.
    if (pinned_log_fd >= 0 && expected_log_inode != 0 && !path_has_file_identity(provisional_path, open_log_stat)) {
      int restore_errno = 0;
      if (link_open_file_no_replace(pinned_log_fd, provisional_path, &restore_errno)) {
        archive_job_log_path_ = provisional_path;
        QString restoration_sync_error;
        if (sync_parent_directory(provisional_path, &restoration_sync_error)) {
          reopen_error.clear();
          reopened = reopenArchiveJobLog(archive_job_log_path_, &reopen_error, expected_log_device, expected_log_inode);
        } else {
          reopen_error = QString("restored provisional log could not be made durable: %1").arg(restoration_sync_error);
        }
      }
    }
  }
  if (copied_log_fd >= 0) {
    ::close(copied_log_fd);
    copied_log_fd = -1;
  }
  if (pinned_log_fd >= 0) {
    ::close(pinned_log_fd);
    pinned_log_fd = -1;
  }
#endif
  if (!archive_job_log_.isOpen()) {
    archive_job_log_enabled_ = false;
    appendLog(QString("archive job log could not continue after output path resolution: %1")
                  .arg(reopen_error.isEmpty() ? QString("unknown error") : reopen_error));
    return;
  }
  if (copied_across_filesystems) {
#ifdef Q_OS_UNIX
    QString cleanup_error;
    const bool old_log_removed = remove_path_if_same_identity(
        provisional_path, open_log_stat, &cleanup_error, resolved_log_path, &copied_log_stat);
    if (!archive_job_log_guard_path_.isEmpty() && path_has_file_identity(archive_job_log_guard_path_, open_log_stat)) {
      QString guard_cleanup_error;
      if (!remove_path_if_same_identity(
              archive_job_log_guard_path_, open_log_stat, &guard_cleanup_error, resolved_log_path, &copied_log_stat)) {
        appendLog(QString("provisional archive log guard retained at %1: %2")
                      .arg(archive_job_log_guard_path_, guard_cleanup_error));
      }
    }
    archive_job_log_guard_path_ = copied_log_guard_path;
    if (!old_log_removed)
      appendLog(QString("provisional archive job log retained after cross-filesystem copy: %1").arg(cleanup_error));
#endif
    appendLog(QString("archive job log copied to resolved filesystem: %1").arg(archive_job_log_path_));
  } else if (renamed) {
    appendLog(QString("archive job log resolved: %1").arg(archive_job_log_path_));
  } else {
    appendLog(QString("archive job log remains at %1 because it could not be renamed to %2")
                  .arg(archive_job_log_path_, resolved_log_path));
  }
  if (!durability_error.isEmpty())
    appendLog(QString("archive job log path durability sync failed: %1").arg(durability_error));
}

bool HStreamWindow::reopenArchiveJobLog(
    const QString& path,
    QString* error,
    quint64 expected_device,
    quint64 expected_inode) {
  if (path.isEmpty()) {
    if (error)
      *error = "log path is empty";
    return false;
  }
  if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_LOG_REOPEN_FAIL") && path.contains(".hstream-run-v3-")) {
    if (error)
      *error = "archive log reopen failure requested by test";
    return false;
  }
  const QString forced_cross_filesystem_reopen_failure =
      qEnvironmentVariable("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_REOPEN_FAILURE");
  if (!forced_cross_filesystem_reopen_failure.isEmpty() && path == forced_cross_filesystem_reopen_failure) {
    qunsetenv("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_REOPEN_FAILURE");
    if (error)
      *error = "cross-filesystem archive log reopen failure requested by test";
    return false;
  }
  archive_job_log_.setFileName(path);
#ifdef Q_OS_UNIX
  const QByteArray encoded_path = QFile::encodeName(path);
  const int fd = ::open(encoded_path.constData(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  struct stat log_stat{};
  const int stat_result = ::fstat(fd, &log_stat);
  const bool identity_matches = expected_inode == 0 ||
      (static_cast<quint64>(log_stat.st_dev) == expected_device &&
       static_cast<quint64>(log_stat.st_ino) == expected_inode);
  if (stat_result != 0 || !S_ISREG(log_stat.st_mode) || !identity_matches) {
    if (error) {
      *error = stat_result != 0 ? QString::fromLocal8Bit(std::strerror(errno))
                                : (!S_ISREG(log_stat.st_mode) ? QString("resolved log path is not a regular file")
                                                              : QString("resolved log path was replaced"));
    }
    ::close(fd);
    return false;
  }
  if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    if (error)
      *error = QString::fromLocal8Bit(std::strerror(errno));
    ::close(fd);
    return false;
  }
  if (!archive_job_log_.open(
          fd, QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text, QFileDevice::AutoCloseHandle)) {
    if (error)
      *error = archive_job_log_.errorString();
    ::close(fd);
    return false;
  }
  return true;
#else
  const bool reopened = archive_job_log_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
  if (!reopened && error)
    *error = archive_job_log_.errorString();
  return reopened;
#endif
}

void HStreamWindow::finishArchiveJobLog(bool retire_identity_guard) {
  QString durability_error;
  struct stat open_log_stat{};
  bool have_open_log_identity = false;
  if (archive_job_log_.isOpen()) {
#ifdef Q_OS_UNIX
    have_open_log_identity = ::fstat(archive_job_log_.handle(), &open_log_stat) == 0 && S_ISREG(open_log_stat.st_mode);
    sync_open_file(archive_job_log_, &durability_error);
#else
    if (!archive_job_log_.flush())
      durability_error = archive_job_log_.errorString();
#endif
    archive_job_log_.close();
  }
#ifdef Q_OS_UNIX
  if (retire_identity_guard && !archive_job_log_guard_path_.isEmpty() && have_open_log_identity) {
    QString guard_cleanup_error;
    if (!remove_path_if_same_identity(
            archive_job_log_guard_path_, open_log_stat, &guard_cleanup_error, archive_job_log_path_, &open_log_stat)) {
      appendLog(QString("archive job log identity guard retained at %1: %2")
                    .arg(archive_job_log_guard_path_, guard_cleanup_error));
    }
  }
#endif
  archive_job_log_.setFileName({});
  archive_job_log_path_.clear();
  archive_job_log_guard_path_.clear();
  archive_job_log_device_ = 0;
  archive_job_log_inode_ = 0;
  archive_job_log_enabled_ = false;
  if (!durability_error.isEmpty())
    appendLog(QString("archive job log final durability sync failed: %1").arg(durability_error));
}

void HStreamWindow::updateArchiveOutputPathLabel() {
  if (!archive_output_path_label_) {
    return;
  }
  const auto archive_toggle = output_toggles_.find("archive-file");
  const bool archive_enabled =
      archive_toggle != output_toggles_.end() && archive_toggle->second && archive_toggle->second->isChecked();
  const bool pipeline_running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  if (pipeline_running && !active_archive_output_path_.isEmpty()) {
    archive_output_path_label_->setText(
        active_archive_recovery_path_.isEmpty()
            ? QString("Current archive: %1\nRoute change applies to the next run").arg(active_archive_output_path_)
            : QString(
                  "Current archive: %1\nPrevious archive retained for recovery: "
                  "%2\nRoute change applies to the next run")
                  .arg(active_archive_output_path_, active_archive_recovery_path_));
  } else if (archive_enabled) {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    archive_output_path_label_->setText(QString("Archive: %1")
                                            .arg(archive_output_path(
                                                archive_output_work_dir(env, pipelineWorkingDirectory()),
                                                game_id_edit_ ? game_id_edit_->text().trimmed() : QString())));
  } else {
    archive_output_path_label_->setText("Archive path will be shown when enabled");
  }
}

bool HStreamWindow::acquireArchiveFinalizerOwnership(const QString& source_path, QString* error) {
  const QString absolute_source = QFileInfo(source_path).absoluteFilePath();
  const QString lock_path = absolute_source + ".hstream-owner-lock";
  if (archive_finalize_owner_lock_fd_ >= 0 && archive_finalize_owner_lock_path_ == lock_path)
    return true;
  releaseArchiveFinalizerOwnership(false);
#ifdef Q_OS_UNIX
  const QByteArray encoded_path = QFile::encodeName(lock_path);
  const int lock_fd = ::open(encoded_path.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (lock_fd < 0) {
    if (error) {
      *error = QString("Could not open archive finalizer ownership lock %1: %2")
                   .arg(lock_path, QString::fromLocal8Bit(std::strerror(errno)));
    }
    return false;
  }
  if (::flock(lock_fd, LOCK_SH) != 0) {
    const int saved_errno = errno;
    ::close(lock_fd);
    if (error) {
      *error = QString("Could not claim archive finalizer ownership lock %1: %2")
                   .arg(lock_path, QString::fromLocal8Bit(std::strerror(saved_errno)));
    }
    return false;
  }
  archive_finalize_owner_lock_fd_ = lock_fd;
  archive_finalize_owner_lock_path_ = lock_path;
  return true;
#else
  if (error)
    *error = "Archive finalizer ownership locks are unavailable on this platform.";
  return false;
#endif
}

void HStreamWindow::releaseArchiveFinalizerOwnership(bool remove_lock_file) {
#ifdef Q_OS_UNIX
  if (archive_finalize_owner_lock_fd_ >= 0) {
    struct stat lock_stat{};
    const bool have_lock_identity =
        ::fstat(archive_finalize_owner_lock_fd_, &lock_stat) == 0 && S_ISREG(lock_stat.st_mode);
    if (remove_lock_file && have_lock_identity && !archive_finalize_owner_lock_path_.isEmpty()) {
      if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_OWNER_LOCK_REPLACEMENT")) {
        const QByteArray encoded_lock = QFile::encodeName(archive_finalize_owner_lock_path_);
        ::unlink(encoded_lock.constData());
        const int replacement_fd =
            ::open(encoded_lock.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (replacement_fd >= 0) {
          constexpr char kReplacement[] = "injected foreign owner lock";
          const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
          (void)replacement_bytes;
          ::close(replacement_fd);
        }
      }
      QString cleanup_error;
      if (!remove_path_if_same_identity(archive_finalize_owner_lock_path_, lock_stat, &cleanup_error)) {
        appendLog(QString("archive finalizer ownership lock retained at %1: %2")
                      .arg(archive_finalize_owner_lock_path_, cleanup_error));
      }
    }
    ::flock(archive_finalize_owner_lock_fd_, LOCK_UN);
    ::close(archive_finalize_owner_lock_fd_);
  }
#endif
  archive_finalize_owner_lock_fd_ = -1;
  archive_finalize_owner_lock_path_.clear();
}

bool HStreamWindow::releaseArchiveFinalizeSource(bool remove_guard, bool require_target_identity) {
  bool released = true;
#ifdef Q_OS_UNIX
  struct stat source_stat{};
  if (remove_guard && archive_finalize_source_fd_ >= 0 && !archive_finalize_source_guard_path_.isEmpty() &&
      ::fstat(archive_finalize_source_fd_, &source_stat) == 0 && S_ISREG(source_stat.st_mode)) {
    QString cleanup_error;
    const bool source_path_is_primary = path_has_file_identity(archive_finalize_source_path_, source_stat);
    struct stat target_stat{};
    const bool target_identity_available = require_target_identity && archive_finalize_target_fd_ >= 0 &&
        ::fstat(archive_finalize_target_fd_, &target_stat) == 0 && S_ISREG(target_stat.st_mode) &&
        static_cast<quint64>(target_stat.st_dev) == archive_finalize_target_device_ &&
        static_cast<quint64>(target_stat.st_ino) == archive_finalize_target_inode_;
    const QString required_path = require_target_identity
        ? archive_finalize_target_path_
        : (source_path_is_primary ? archive_finalize_source_path_ : QString());
    const struct stat* required_stat = require_target_identity ? (target_identity_available ? &target_stat : nullptr)
                                                               : (source_path_is_primary ? &source_stat : nullptr);
    if ((require_target_identity && !target_identity_available) ||
        !remove_path_if_same_identity(
            archive_finalize_source_guard_path_, source_stat, &cleanup_error, required_path, required_stat)) {
      released = false;
      if (cleanup_error.isEmpty())
        cleanup_error = "the completed MP4 identity is unavailable";
      appendLog(QString("archive source identity guard retained at %1: %2")
                    .arg(archive_finalize_source_guard_path_, cleanup_error));
    }
  } else if (remove_guard && !archive_finalize_source_guard_path_.isEmpty()) {
    released = false;
    appendLog(QString("archive source identity guard retained at %1: source identity is unavailable")
                  .arg(archive_finalize_source_guard_path_));
  }
  if ((!remove_guard || released) && archive_finalize_source_fd_ >= 0)
    ::close(archive_finalize_source_fd_);
#endif
  if (remove_guard && !released)
    return false;
  archive_finalize_source_fd_ = -1;
  archive_finalize_source_device_ = 0;
  archive_finalize_source_inode_ = 0;
  archive_finalize_source_guard_path_.clear();
  return true;
}

bool HStreamWindow::releaseArchiveFinalizeTarget(bool remove_guard) {
  bool released = true;
#ifdef Q_OS_UNIX
  struct stat target_stat{};
  if (remove_guard && archive_finalize_target_fd_ >= 0 && !archive_finalize_target_guard_path_.isEmpty() &&
      ::fstat(archive_finalize_target_fd_, &target_stat) == 0 && S_ISREG(target_stat.st_mode)) {
    QString cleanup_error;
    if (!remove_path_if_same_identity(
            archive_finalize_target_guard_path_,
            target_stat,
            &cleanup_error,
            archive_finalize_target_path_,
            &target_stat)) {
      released = false;
      appendLog(QString("completed archive identity guard retained at %1: %2")
                    .arg(archive_finalize_target_guard_path_, cleanup_error));
    }
  } else if (remove_guard && !archive_finalize_target_guard_path_.isEmpty()) {
    released = false;
    appendLog(QString("completed archive identity guard retained at %1: target identity is unavailable")
                  .arg(archive_finalize_target_guard_path_));
  }
  if ((!remove_guard || released) && archive_finalize_target_fd_ >= 0)
    ::close(archive_finalize_target_fd_);
#endif
  if (remove_guard && !released)
    return false;
  archive_finalize_target_fd_ = -1;
  archive_finalize_target_device_ = 0;
  archive_finalize_target_inode_ = 0;
  archive_finalize_target_guard_path_.clear();
  return true;
}

void HStreamWindow::startArchiveFinalization(const QString& source_path, const QString& game_id, bool hevc_video) {
  if (!archive_finalize_process_ || archive_finalize_process_->state() != QProcess::NotRunning)
    return;

  if (!archive_finalize_dialog_) {
    auto* dialog = new StitchingCalibrationDialog(this);
    archive_finalize_dialog_ = dialog;
    dialog->setObjectName("archiveFinalizeDialog");
    dialog->setWindowTitle("Finalizing completed video");
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumWidth(540);
    dialog->setStyleSheet(
        "QLabel[finalizationState=active] { color: #1570ef; font-weight: 600; }"
        "QLabel[finalizationState=complete] { color: #039855; font-weight: 600; }"
        "QLabel[finalizationState=failed] { color: #d92d20; font-weight: 600; }"
        "QProgressBar[finalizationState=active]::chunk { background: #1570ef; }"
        "QProgressBar[finalizationState=complete]::chunk { background: #039855; }"
        "QProgressBar[finalizationState=failed]::chunk { background: #d92d20; }");

    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 24, 24, 20);
    root->setSpacing(14);
    auto* heading = new QHBoxLayout();
    heading->setSpacing(14);
    archive_finalize_icon_ = new QLabel(dialog);
    archive_finalize_icon_->setObjectName("archiveFinalizeIcon");
    archive_finalize_icon_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    archive_finalize_icon_->setFixedSize(40, 40);
    heading->addWidget(archive_finalize_icon_);
    auto* heading_text = new QVBoxLayout();
    heading_text->setSpacing(4);
    archive_finalize_headline_ = new QLabel(dialog);
    archive_finalize_headline_->setObjectName("archiveFinalizeHeadline");
    QFont headline_font = archive_finalize_headline_->font();
    headline_font.setPointSize(headline_font.pointSize() + 2);
    headline_font.setBold(true);
    archive_finalize_headline_->setFont(headline_font);
    archive_finalize_detail_ = new QLabel(dialog);
    archive_finalize_detail_->setObjectName("archiveFinalizeDetail");
    archive_finalize_detail_->setWordWrap(true);
    archive_finalize_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading_text->addWidget(archive_finalize_headline_);
    heading_text->addWidget(archive_finalize_detail_);
    heading->addLayout(heading_text, 1);
    root->addLayout(heading);

    archive_finalize_progress_ = new QProgressBar(dialog);
    archive_finalize_progress_->setObjectName("archiveFinalizeProgress");
    archive_finalize_progress_->setTextVisible(true);
    root->addWidget(archive_finalize_progress_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    archive_finalize_ok_button_ = new QPushButton("OK", dialog);
    archive_finalize_ok_button_->setObjectName("archiveFinalizeOkButton");
    archive_finalize_ok_button_->setDefault(true);
    set_control_help(
        archive_finalize_ok_button_,
        "Close the finalization result after reviewing where the completed MP4 or recoverable work file was saved.");
    archive_finalize_ok_button_->hide();
    buttons->addWidget(archive_finalize_ok_button_);
    root->addLayout(buttons);
    connect(archive_finalize_ok_button_, &QPushButton::clicked, dialog, &QDialog::accept);
  }

  archive_finalize_failed_ = false;
  releaseArchiveFinalizeSource(false);
  releaseArchiveFinalizeTarget(false);
#ifdef Q_OS_UNIX
  if (archive_finalize_recovery_log_fd_ >= 0)
    ::close(archive_finalize_recovery_log_fd_);
#endif
  archive_finalize_recovery_log_fd_ = -1;
  archive_finalize_recovery_log_device_ = 0;
  archive_finalize_recovery_log_inode_ = 0;
  archive_finalize_source_path_ = QFileInfo(source_path).absoluteFilePath();
  archive_finalize_game_id_ = game_id;
  const QString archive_game_directory = gameDirectory(game_id);
#ifdef Q_OS_UNIX
  QSet<QString> cleanup_directories = {
      QFileInfo(archive_finalize_source_path_).absolutePath(), QFileInfo(archive_game_directory).absoluteFilePath()};
  for (const QString& cleanup_directory : cleanup_directories) {
    QString cleanup_error;
    if (!reconcile_scoped_ui_cleanup_directory(cleanup_directory, &cleanup_error)) {
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
      showArchiveFinalizationFailure(
          QString(
              "Could not reconcile an interrupted archive cleanup in %1: %2\n\nDo not start another archive "
              "run until the retained files have been copied to safety.")
              .arg(cleanup_directory, cleanup_error));
      return;
    }
  }
#endif
  archive_finalize_target_path_ = available_final_archive_path(archive_game_directory, game_id);
  archive_finalize_stdout_buffer_.clear();
  archive_finalize_error_output_.clear();
  archive_finalize_pending_failure_detail_.clear();
  archive_finalize_duration_us_ = media_clock_us(playback_total_);
  archive_finalize_stage_ = ArchiveFinalizeStage::kRemux;

  auto apply_state = [](QWidget* widget, const char* state) {
    if (!widget)
      return;
    widget->setProperty("finalizationState", state);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
  };
  apply_state(archive_finalize_headline_, "active");
  apply_state(archive_finalize_progress_, "active");
  archive_finalize_icon_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(32, 32));
  archive_finalize_headline_->setText("Finalizing completed video…");
  archive_finalize_detail_->setText(
      "Losslessly remuxing the archive for iPhone and Plex compatibility. The video is not being re-encoded.");
  if (archive_finalize_duration_us_ > 0) {
    archive_finalize_progress_->setRange(0, 1000);
    archive_finalize_progress_->setValue(0);
    archive_finalize_progress_->setFormat("Preparing MP4  •  0.0%");
  } else {
    archive_finalize_progress_->setRange(0, 0);
    archive_finalize_progress_->setFormat("Preparing MP4");
  }
  archive_finalize_ok_button_->hide();
  static_cast<StitchingCalibrationDialog*>(archive_finalize_dialog_)->setCloseAllowed(false);
  archive_finalize_dialog_->show();
  archive_finalize_dialog_->raise();

  QString ownership_error;
  if (!acquireArchiveFinalizerOwnership(archive_finalize_source_path_, &ownership_error)) {
    archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
    showArchiveFinalizationFailure(
        ownership_error +
        "\n\nThe completed work file could not be protected for finalization. Do not start another archive run until "
        "this file has been copied to safety.");
    return;
  }

#ifdef Q_OS_UNIX
  const QByteArray encoded_source = QFile::encodeName(archive_finalize_source_path_);
  const int source_fd = ::open(encoded_source.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct stat source_stat{};
  struct stat named_source_stat{};
  if (source_fd < 0 || ::fstat(source_fd, &source_stat) != 0 || !S_ISREG(source_stat.st_mode) ||
      ::lstat(encoded_source.constData(), &named_source_stat) != 0 ||
      !same_file_identity(source_stat, named_source_stat)) {
    const int saved_errno = source_fd < 0 ? errno : ESTALE;
    if (source_fd >= 0)
      ::close(source_fd);
    archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
    showArchiveFinalizationFailure(
        QString(
            "Could not pin the completed archive for finalization: %1\n\nDo not start another archive run until "
            "this file has been copied to safety.")
            .arg(QString::fromLocal8Bit(std::strerror(saved_errno))));
    return;
  }
  archive_finalize_source_fd_ = source_fd;
  archive_finalize_source_device_ = static_cast<quint64>(source_stat.st_dev);
  archive_finalize_source_inode_ = static_cast<quint64>(source_stat.st_ino);
  QString source_guard_error;
  if (!create_open_file_guard(
          archive_finalize_source_fd_,
          archive_finalize_source_path_,
          &archive_finalize_source_guard_path_,
          &source_guard_error)) {
    releaseArchiveFinalizeSource(false);
    archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
    showArchiveFinalizationFailure(
        QString(
            "Could not protect the completed archive during finalization: %1\n\nDo not start another archive "
            "run until this file has been copied to safety.")
            .arg(source_guard_error));
    return;
  }
#endif

  if (archive_finalize_target_path_.isEmpty()) {
    failArchiveFinalization("Could not find an available final filename in the game directory.");
    return;
  }
  const QFileInfo target_info(archive_finalize_target_path_);
  if (!QDir().mkpath(target_info.absolutePath())) {
    failArchiveFinalization(QString("Could not create the game directory: %1").arg(target_info.absolutePath()));
    return;
  }
  QTemporaryDir temporary_directory(
      QDir(target_info.absolutePath())
          .filePath(QString(".%1.hstream-finalize-XXXXXX").arg(target_info.completeBaseName())));
  if (!temporary_directory.isValid()) {
    failArchiveFinalization(
        QString("Could not create a private temporary directory in %1.").arg(target_info.absolutePath()));
    return;
  }
  if (!QFile::setPermissions(
          temporary_directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
    failArchiveFinalization(
        QString("Could not secure the temporary video directory in %1.").arg(target_info.absolutePath()));
    return;
  }
  temporary_directory.setAutoRemove(false);
  archive_finalize_temporary_dir_ = temporary_directory.path();
  archive_finalize_partial_path_ = QDir(archive_finalize_temporary_dir_).filePath("completed.mp4");

  QString ffmpeg = qEnvironmentVariable("HSTREAM_UI_FFMPEG").trimmed();
  if (ffmpeg.isEmpty())
    ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  else if (QFileInfo(ffmpeg).isRelative())
    ffmpeg = QStandardPaths::findExecutable(ffmpeg);
  if (ffmpeg.isEmpty()) {
    failArchiveFinalization("ffmpeg is not installed or could not be found in PATH.");
    return;
  }

  QString archive_input = archive_finalize_source_path_;
#ifdef Q_OS_UNIX
  archive_input = QString("/proc/self/fd/%1").arg(archive_finalize_source_fd_);
#endif
  QStringList arguments = {
      "-hide_banner",
      "-n",
      "-progress",
      "pipe:1",
      "-nostats",
      "-i",
      archive_input,
      "-map",
      "0:v:0",
      "-map",
      "0:a?",
      "-map_metadata",
      "0",
      "-c",
      "copy",
      "-movflags",
      "+faststart"};
  if (hevc_video)
    arguments << "-tag:v" << "hvc1";
  arguments << archive_finalize_partial_path_;
  output_states_["archive-file"]->setText("FINALIZING");
  appendLog(
      QString("finalizing archive without re-encoding: %1 -> %2").arg(source_path, archive_finalize_target_path_));
  archive_finalize_process_->setProgram(ffmpeg);
  archive_finalize_process_->setArguments(arguments);
  archive_finalize_process_->setWorkingDirectory(target_info.absolutePath());
#ifdef Q_OS_UNIX
  const int inherited_source_fd = archive_finalize_source_fd_;
  archive_finalize_process_->setChildProcessModifier([inherited_source_fd]() {
    const int descriptor_flags = ::fcntl(inherited_source_fd, F_GETFD);
    if (descriptor_flags < 0 || ::fcntl(inherited_source_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0)
      ::_exit(127);
  });
#endif
  archive_finalize_process_->start();
  updateRunControls();
}

void HStreamWindow::readArchiveFinalizationProgress() {
  if (!archive_finalize_process_ || archive_finalize_stage_ != ArchiveFinalizeStage::kRemux)
    return;
  archive_finalize_stdout_buffer_ += QString::fromLocal8Bit(archive_finalize_process_->readAllStandardOutput());
  while (true) {
    const qsizetype newline = archive_finalize_stdout_buffer_.indexOf('\n');
    if (newline < 0)
      break;
    const QString line = archive_finalize_stdout_buffer_.left(newline).trimmed();
    archive_finalize_stdout_buffer_.remove(0, newline + 1);
    if (!line.startsWith("out_time="))
      continue;
    const qint64 completed_us = media_clock_us(line.mid(QString("out_time=").size()));
    if (completed_us < 0 || archive_finalize_duration_us_ <= 0 || !archive_finalize_progress_)
      continue;
    const int value = static_cast<int>(std::clamp<qint64>(completed_us * 1000 / archive_finalize_duration_us_, 0, 999));
    archive_finalize_progress_->setRange(0, 1000);
    archive_finalize_progress_->setValue(value);
    archive_finalize_progress_->setFormat(QString("Preparing MP4  •  %1%").arg(value / 10.0, 0, 'f', 1));
  }
}

bool HStreamWindow::startArchiveDurabilitySync(const QString& path, ArchiveFinalizeStage stage, QString* error) {
  QString sync_program = qEnvironmentVariable("HSTREAM_UI_SYNC").trimmed();
  if (sync_program.isEmpty())
    sync_program = QStandardPaths::findExecutable("sync");
  else if (QFileInfo(sync_program).isRelative())
    sync_program = QStandardPaths::findExecutable(sync_program);
  if (sync_program.isEmpty()) {
    if (error)
      *error = "The sync durability helper is not installed or could not be found in PATH.";
    return false;
  }

  archive_finalize_stage_ = stage;
  archive_finalize_stdout_buffer_.clear();
  archive_finalize_error_output_.clear();
  archive_finalize_headline_->setText(
      stage == ArchiveFinalizeStage::kSyncCompleted ? "Saving completed video safely…" : "Securing recovery archive…");
  archive_finalize_detail_->setText(
      stage == ArchiveFinalizeStage::kSyncCompleted
          ? QString("The MP4 is ready and is being flushed safely to disk at:\n%1").arg(path)
          : QString("The original archive is being flushed safely to disk at:\n%1").arg(path));
  archive_finalize_progress_->setRange(0, 0);
  archive_finalize_progress_->setFormat(
      stage == ArchiveFinalizeStage::kSyncCompleted ? "Saving MP4 safely" : "Securing recovery archive");
  appendLog(QString("durability sync starting for archive: %1").arg(path));
  archive_finalize_process_->setProgram(sync_program);
#ifdef Q_OS_UNIX
  const int inherited_video_fd =
      stage == ArchiveFinalizeStage::kSyncCompleted ? archive_finalize_target_fd_ : archive_finalize_source_fd_;
  const int inherited_log_fd = stage == ArchiveFinalizeStage::kSyncRecovery ? archive_finalize_recovery_log_fd_ : -1;
  if (inherited_video_fd < 0) {
    if (error)
      *error = "The archive durability identity is no longer available.";
    return false;
  }
  QStringList sync_arguments = {"-f", QString("/proc/self/fd/%1").arg(inherited_video_fd)};
  if (inherited_log_fd >= 0)
    sync_arguments << QString("/proc/self/fd/%1").arg(inherited_log_fd);
  archive_finalize_process_->setArguments(sync_arguments);
  archive_finalize_process_->setChildProcessModifier([inherited_video_fd, inherited_log_fd]() {
    int descriptor_flags = ::fcntl(inherited_video_fd, F_GETFD);
    if (descriptor_flags < 0 || ::fcntl(inherited_video_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0)
      ::_exit(127);
    if (inherited_log_fd >= 0) {
      descriptor_flags = ::fcntl(inherited_log_fd, F_GETFD);
      if (descriptor_flags < 0 || ::fcntl(inherited_log_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0)
        ::_exit(127);
    }
  });
#else
  archive_finalize_process_->setArguments({"-f", path});
#endif
  archive_finalize_process_->setWorkingDirectory(QFileInfo(path).absolutePath());
  archive_finalize_process_->start();
  return true;
}

void HStreamWindow::finishArchiveFinalization(int exit_code, QProcess::ExitStatus exit_status) {
  archive_finalize_error_output_ += QString::fromLocal8Bit(archive_finalize_process_->readAllStandardError());
  if (archive_finalize_stage_ == ArchiveFinalizeStage::kSyncRecovery) {
    QString failure_detail = archive_finalize_pending_failure_detail_;
    bool recovery_identity_valid = true;
    bool recovery_log_identity_valid = true;
    bool recovery_artifact_retained = true;
    bool recovery_log_retained = true;
#ifdef Q_OS_UNIX
    struct stat recovery_stat{};
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_REPLACEMENT_DURING_SYNC")) {
      const QByteArray encoded_recovery = QFile::encodeName(archive_finalize_source_path_);
      ::unlink(encoded_recovery.constData());
      const int replacement_fd =
          ::open(encoded_recovery.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign recovery during sync";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_GUARD_REPLACEMENT_DURING_SYNC")) {
      const QByteArray encoded_guard = QFile::encodeName(archive_finalize_source_guard_path_);
      ::unlink(encoded_guard.constData());
      const int replacement_fd =
          ::open(encoded_guard.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign recovery guard during sync";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    const bool recovery_fd_valid = archive_finalize_source_fd_ >= 0 &&
        ::fstat(archive_finalize_source_fd_, &recovery_stat) == 0 && S_ISREG(recovery_stat.st_mode) &&
        static_cast<quint64>(recovery_stat.st_dev) == archive_finalize_source_device_ &&
        static_cast<quint64>(recovery_stat.st_ino) == archive_finalize_source_inode_;
    recovery_identity_valid = recovery_fd_valid && path_has_file_identity(archive_finalize_source_path_, recovery_stat);
    const bool recovery_guard_valid = recovery_fd_valid && !archive_finalize_source_guard_path_.isEmpty() &&
        path_has_file_identity(archive_finalize_source_guard_path_, recovery_stat);
    struct stat recovery_log_stat{};
    if (archive_finalize_recovery_log_fd_ >= 0) {
      if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT_DURING_SYNC")) {
        const QByteArray encoded_log = QFile::encodeName(archive_job_log_path_);
        ::unlink(encoded_log.constData());
        const int replacement_fd =
            ::open(encoded_log.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (replacement_fd >= 0) {
          constexpr char kReplacement[] = "injected foreign recovery log during sync";
          const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
          (void)replacement_bytes;
          ::close(replacement_fd);
        }
      }
      if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_GUARD_REPLACEMENT_DURING_SYNC")) {
        const QByteArray encoded_log_guard = QFile::encodeName(archive_job_log_guard_path_);
        ::unlink(encoded_log_guard.constData());
        const int replacement_fd =
            ::open(encoded_log_guard.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (replacement_fd >= 0) {
          constexpr char kReplacement[] = "injected foreign recovery log guard during sync";
          const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
          (void)replacement_bytes;
          ::close(replacement_fd);
        }
      }
      const bool recovery_log_fd_valid = ::fstat(archive_finalize_recovery_log_fd_, &recovery_log_stat) == 0 &&
          S_ISREG(recovery_log_stat.st_mode) &&
          static_cast<quint64>(recovery_log_stat.st_dev) == archive_finalize_recovery_log_device_ &&
          static_cast<quint64>(recovery_log_stat.st_ino) == archive_finalize_recovery_log_inode_;
      recovery_log_identity_valid =
          recovery_log_fd_valid && path_has_file_identity(archive_job_log_path_, recovery_log_stat);
      const bool recovery_log_guard_valid = recovery_log_fd_valid && !archive_job_log_guard_path_.isEmpty() &&
          path_has_file_identity(archive_job_log_guard_path_, recovery_log_stat);
      if (!recovery_log_identity_valid) {
        if (archive_job_log_.isOpen()) {
          archive_job_log_.flush();
          archive_job_log_.close();
        }
        if (recovery_log_guard_valid) {
          archive_job_log_path_ = archive_job_log_guard_path_;
          archive_job_log_guard_path_.clear();
        } else if (recovery_log_fd_valid) {
          QString rescue_error;
          struct stat rescued_log_stat{};
          archive_job_log_path_ = rescue_open_file_no_replace(
              archive_finalize_recovery_log_fd_,
              recovery_log_stat,
              archive_job_log_path_,
              &rescue_error,
              &rescued_log_stat);
          archive_job_log_guard_path_.clear();
          if (archive_job_log_path_.isEmpty()) {
            recovery_log_retained = false;
            failure_detail += QString(
                                  "\n\nThe trusted recovery log pathname and guard were replaced, and a rescue "
                                  "link could not be published: %1")
                                  .arg(rescue_error);
          } else {
            recovery_log_stat = rescued_log_stat;
            archive_finalize_recovery_log_device_ = static_cast<quint64>(rescued_log_stat.st_dev);
            archive_finalize_recovery_log_inode_ = static_cast<quint64>(rescued_log_stat.st_ino);
          }
        } else {
          recovery_log_retained = false;
        }
        if (recovery_log_retained) {
          QString reopen_error;
          archive_job_log_enabled_ = reopenArchiveJobLog(
              archive_job_log_path_,
              &reopen_error,
              archive_finalize_recovery_log_device_,
              archive_finalize_recovery_log_inode_);
          if (!archive_job_log_enabled_)
            failure_detail += QString("\n\nThe trusted recovery log was retained at %1 but could not be reopened: %2")
                                  .arg(archive_job_log_path_, reopen_error);
        }
      } else if (!recovery_log_guard_valid) {
        archive_job_log_guard_path_.clear();
      }
    }
    if (!recovery_identity_valid) {
      if (recovery_guard_valid) {
        archive_finalize_source_path_ = archive_finalize_source_guard_path_;
        archive_finalize_source_guard_path_.clear();
      } else if (recovery_fd_valid) {
        QString rescue_error;
        struct stat rescued_video_stat{};
        archive_finalize_source_path_ = rescue_open_file_no_replace(
            archive_finalize_source_fd_,
            recovery_stat,
            archive_finalize_source_path_,
            &rescue_error,
            &rescued_video_stat);
        archive_finalize_source_guard_path_.clear();
        if (archive_finalize_source_path_.isEmpty()) {
          recovery_artifact_retained = false;
          failure_detail += QString(
                                "\n\nThe trusted recovery pathname and guard were replaced, and a rescue link "
                                "could not be published: %1")
                                .arg(rescue_error);
        } else {
          recovery_stat = rescued_video_stat;
          archive_finalize_source_device_ = static_cast<quint64>(rescued_video_stat.st_dev);
          archive_finalize_source_inode_ = static_cast<quint64>(rescued_video_stat.st_ino);
        }
      } else {
        recovery_artifact_retained = false;
      }
      if (recovery_artifact_retained)
        failure_detail += QString(
                              "\n\nA recovery pathname was replaced during durability sync; the trusted video "
                              "was retained at %1.")
                              .arg(archive_finalize_source_path_);
    } else if (!recovery_guard_valid) {
      archive_finalize_source_guard_path_.clear();
    }
#endif
    if (exit_status != QProcess::NormalExit || exit_code != 0) {
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
      QString helper_detail = archive_finalize_error_output_.trimmed();
      if (helper_detail.size() > 1000)
        helper_detail = helper_detail.right(1000);
      failure_detail +=
          QString(
              "\n\nThe recovery file was renamed, but durability sync failed with code %1.%2 Do not start "
              "another archive run until this file has been copied to safety.")
              .arg(exit_code)
              .arg(helper_detail.isEmpty() ? QString() : "\n" + helper_detail);
    } else if (recovery_artifact_retained && recovery_log_retained) {
      archive_finalize_blocked_source_path_.clear();
      appendLog(QString("recovery archive safely synced: %1").arg(archive_finalize_source_path_));
    } else {
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
    }
#ifdef Q_OS_UNIX
    if (archive_finalize_recovery_log_fd_ >= 0 && recovery_log_retained)
      ::close(archive_finalize_recovery_log_fd_);
#endif
    if (recovery_log_retained) {
      archive_finalize_recovery_log_fd_ = -1;
      archive_finalize_recovery_log_device_ = 0;
      archive_finalize_recovery_log_inode_ = 0;
    }
    if (recovery_artifact_retained) {
      releaseArchiveFinalizeSource(recovery_identity_valid && exit_status == QProcess::NormalExit && exit_code == 0);
    }
    showArchiveFinalizationFailure(failure_detail);
    return;
  }
  if (archive_finalize_stage_ == ArchiveFinalizeStage::kSyncCompleted) {
    if (exit_status != QProcess::NormalExit || exit_code != 0) {
      QString helper_detail = archive_finalize_error_output_.trimmed();
      if (helper_detail.size() > 1000)
        helper_detail = helper_detail.right(1000);
      failArchiveFinalization(QString("Archive durability sync failed with code %1.%2")
                                  .arg(exit_code)
                                  .arg(helper_detail.isEmpty() ? QString() : "\n" + helper_detail));
      return;
    }
#ifdef Q_OS_UNIX
    struct stat target_stat{};
    const bool target_fd_is_valid = archive_finalize_target_fd_ >= 0 &&
        ::fstat(archive_finalize_target_fd_, &target_stat) == 0 && S_ISREG(target_stat.st_mode) &&
        static_cast<quint64>(target_stat.st_dev) == archive_finalize_target_device_ &&
        static_cast<quint64>(target_stat.st_ino) == archive_finalize_target_inode_;
    if (!target_fd_is_valid) {
      failArchiveFinalization("The completed MP4 identity was lost during durability sync.");
      return;
    }
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_TARGET_REPLACEMENT_DURING_SYNC")) {
      const QByteArray encoded_target = QFile::encodeName(archive_finalize_target_path_);
      ::unlink(encoded_target.constData());
      const int replacement_fd =
          ::open(encoded_target.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign completed target";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    if (!path_has_file_identity(archive_finalize_target_path_, target_stat)) {
      const QString replaced_target = archive_finalize_target_path_;
      const QString old_guard_path = archive_finalize_target_guard_path_;
      bool republished = false;
      QString republish_error;
      for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString candidate =
            available_final_archive_path(gameDirectory(archive_finalize_game_id_), archive_finalize_game_id_);
        if (candidate.isEmpty()) {
          republish_error = "No safe filename remained for the pinned completed MP4.";
          break;
        }
        int publish_errno = 0;
        if (!link_open_file_no_replace(archive_finalize_target_fd_, candidate, &publish_errno)) {
          if (publish_errno == EEXIST)
            continue;
          republish_error = QString::fromLocal8Bit(std::strerror(publish_errno));
          break;
        }
        QString new_guard_path;
        QString guard_error;
        if (!create_open_file_guard(archive_finalize_target_fd_, candidate, &new_guard_path, &guard_error)) {
          QString cleanup_error;
          remove_path_if_same_identity(candidate, target_stat, &cleanup_error);
          struct stat occupied_guard_stat{};
          const QByteArray encoded_guard = QFile::encodeName(candidate + ".hstream-pin");
          if (::lstat(encoded_guard.constData(), &occupied_guard_stat) == 0)
            continue;
          republish_error = guard_error;
          break;
        }
        if (!old_guard_path.isEmpty() && path_has_file_identity(old_guard_path, target_stat)) {
          QString cleanup_error;
          if (!remove_path_if_same_identity(old_guard_path, target_stat, &cleanup_error, candidate, &target_stat)) {
            appendLog(QString("replaced completed-target guard retained at %1: %2").arg(old_guard_path, cleanup_error));
          }
        }
        archive_finalize_target_path_ = candidate;
        archive_finalize_target_guard_path_ = new_guard_path;
        republished = true;
        appendLog(QString("completed MP4 pathname was replaced; pinned video republished at: %1").arg(candidate));
        break;
      }
      if (!republished) {
        failArchiveFinalization(
            QString("The completed MP4 pathname was replaced during durability sync: %1").arg(republish_error));
        return;
      }
      appendLog(QString("replaced completed MP4 pathname left untouched at: %1").arg(replaced_target));
    }
#endif
    appendLog(QString("completed MP4 safely synced: %1").arg(archive_finalize_target_path_));
    completeArchiveFinalization();
    return;
  }
  if (archive_finalize_stage_ != ArchiveFinalizeStage::kRemux || archive_finalize_failed_)
    return;

  readArchiveFinalizationProgress();
  if (exit_status != QProcess::NormalExit || exit_code != 0) {
    QString detail = archive_finalize_error_output_.trimmed();
    if (detail.size() > 1000)
      detail = detail.right(1000);
    failArchiveFinalization(
        QString("ffmpeg exited with code %1.%2").arg(exit_code).arg(detail.isEmpty() ? QString() : "\n" + detail));
    return;
  }

  int pinned_partial_fd = -1;
  struct stat partial_stat{};
#ifdef Q_OS_UNIX
  const QByteArray encoded_partial = QFile::encodeName(archive_finalize_partial_path_);
  pinned_partial_fd = ::open(encoded_partial.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct stat named_partial_stat{};
  if (pinned_partial_fd < 0 || ::fstat(pinned_partial_fd, &partial_stat) != 0 || !S_ISREG(partial_stat.st_mode) ||
      partial_stat.st_size <= 0 || ::lstat(encoded_partial.constData(), &named_partial_stat) != 0 ||
      !same_file_identity(partial_stat, named_partial_stat)) {
    if (pinned_partial_fd >= 0)
      ::close(pinned_partial_fd);
    pinned_partial_fd = -1;
  }
#else
  const QFileInfo partial(archive_finalize_partial_path_);
#endif
  if (
#ifdef Q_OS_UNIX
      pinned_partial_fd < 0
#else
      !partial.isFile() || partial.size() <= 0
#endif
  ) {
    failArchiveFinalization("ffmpeg reported success but did not create a usable MP4.");
    return;
  }
  bool published = false;
  QString publication_error;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    const QString candidate =
        available_final_archive_path(gameDirectory(archive_finalize_game_id_), archive_finalize_game_id_);
    if (candidate.isEmpty()) {
      publication_error = "Could not find an available final filename in the game directory.";
      break;
    }
#ifdef Q_OS_UNIX
    int publish_errno = 0;
    if (link_open_file_no_replace(pinned_partial_fd, candidate, &publish_errno) &&
        path_has_file_identity(candidate, partial_stat)) {
      QString target_guard_error;
      if (!create_open_file_guard(
              pinned_partial_fd, candidate, &archive_finalize_target_guard_path_, &target_guard_error)) {
        QString cleanup_error;
        remove_path_if_same_identity(candidate, partial_stat, &cleanup_error);
        struct stat occupied_guard_stat{};
        const QByteArray encoded_guard = QFile::encodeName(candidate + ".hstream-pin");
        if (::lstat(encoded_guard.constData(), &occupied_guard_stat) == 0)
          continue;
        publication_error = QString("Could not protect the completed MP4 at %1: %2").arg(candidate, target_guard_error);
        break;
      }
      archive_finalize_target_fd_ = pinned_partial_fd;
      pinned_partial_fd = -1;
      archive_finalize_target_device_ = static_cast<quint64>(partial_stat.st_dev);
      archive_finalize_target_inode_ = static_cast<quint64>(partial_stat.st_ino);
      archive_finalize_target_path_ = candidate;
      published = true;
      break;
    }
    if (publish_errno != EEXIST) {
      publication_error = QString("Could not publish the completed MP4 as %1: %2")
                              .arg(candidate, QString::fromLocal8Bit(std::strerror(publish_errno)));
      break;
    }
#else
    if (QFile::rename(archive_finalize_partial_path_, candidate)) {
      archive_finalize_target_path_ = candidate;
      published = true;
      break;
    }
    if (!QFileInfo::exists(candidate)) {
      publication_error = QString("Could not publish the completed MP4 as %1.").arg(candidate);
      break;
    }
#endif
  }
  if (!published) {
#ifdef Q_OS_UNIX
    if (pinned_partial_fd >= 0)
      ::close(pinned_partial_fd);
#endif
    failArchiveFinalization(
        publication_error.isEmpty() ? "Could not atomically publish the completed MP4." : publication_error);
    return;
  }
#ifdef Q_OS_UNIX
  QString partial_cleanup_error;
  if (!remove_path_if_same_identity(
          archive_finalize_partial_path_,
          partial_stat,
          &partial_cleanup_error,
          archive_finalize_target_path_,
          &partial_stat)) {
    appendLog(QString("completed MP4 temporary link retained or replaced: %1").arg(partial_cleanup_error));
  }
#endif
  archive_finalize_partial_path_.clear();
  if (!archive_finalize_temporary_dir_.isEmpty()) {
    if (!QDir().rmdir(archive_finalize_temporary_dir_))
      appendLog(QString("could not remove empty archive temporary directory: %1").arg(archive_finalize_temporary_dir_));
    archive_finalize_temporary_dir_.clear();
  }

  QString durability_error;
  if (!startArchiveDurabilitySync(
          archive_finalize_target_path_, ArchiveFinalizeStage::kSyncCompleted, &durability_error)) {
    failArchiveFinalization(durability_error);
    return;
  }
}

void HStreamWindow::completeArchiveFinalization() {
  archive_finalize_stage_ = ArchiveFinalizeStage::kIdle;
  qint64 final_size = QFileInfo(archive_finalize_target_path_).size();
  bool source_removed = false;
  bool source_was_replaced = false;
#ifdef Q_OS_UNIX
  struct stat target_stat{};
  const bool target_is_pinned = archive_finalize_target_fd_ >= 0 &&
      ::fstat(archive_finalize_target_fd_, &target_stat) == 0 && S_ISREG(target_stat.st_mode) &&
      static_cast<quint64>(target_stat.st_dev) == archive_finalize_target_device_ &&
      static_cast<quint64>(target_stat.st_ino) == archive_finalize_target_inode_;
  if (!target_is_pinned || !path_has_file_identity(archive_finalize_target_path_, target_stat)) {
    failArchiveFinalization("The completed MP4 pathname changed before finalization could commit.");
    return;
  }
  if (target_is_pinned)
    final_size = target_stat.st_size;
  struct stat source_stat{};
  if (archive_finalize_source_fd_ >= 0 && ::fstat(archive_finalize_source_fd_, &source_stat) == 0 &&
      S_ISREG(source_stat.st_mode) && static_cast<quint64>(source_stat.st_dev) == archive_finalize_source_device_ &&
      static_cast<quint64>(source_stat.st_ino) == archive_finalize_source_inode_) {
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_SUCCESS_SOURCE_REPLACEMENT")) {
      const QByteArray encoded_source = QFile::encodeName(archive_finalize_source_path_);
      ::unlink(encoded_source.constData());
      const int replacement_fd =
          ::open(encoded_source.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign successful source";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    source_was_replaced = !path_has_file_identity(archive_finalize_source_path_, source_stat);
    QString cleanup_error;
    source_removed = !source_was_replaced &&
        remove_path_if_same_identity(
            archive_finalize_source_path_, source_stat, &cleanup_error, archive_finalize_target_path_, &target_stat);
    if (!source_removed && !source_was_replaced && !cleanup_error.isEmpty())
      appendLog(QString("completed archive source could not be safely removed: %1").arg(cleanup_error));
  }
#else
  source_removed = QFile::remove(archive_finalize_source_path_);
#endif
#ifdef Q_OS_UNIX
  if (!path_has_file_identity(archive_finalize_target_path_, target_stat)) {
    failArchiveFinalization("The completed MP4 pathname changed while committing source cleanup.");
    return;
  }
  if (source_removed) {
    QString source_cleanup_sync_error;
    const bool force_source_sync_failure =
        qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_SOURCE_PARENT_SYNC_FAILURE");
    if (force_source_sync_failure) {
      qunsetenv("HSTREAM_UI_TEST_ARCHIVE_SOURCE_PARENT_SYNC_FAILURE");
      source_cleanup_sync_error = "source-parent sync failure requested by test";
    }
    if (force_source_sync_failure ||
        !sync_parent_directory(archive_finalize_source_path_, &source_cleanup_sync_error)) {
      failArchiveFinalization(QString("The completed MP4 was saved, but source cleanup could not be made durable: %1")
                                  .arg(source_cleanup_sync_error));
      return;
    }
  }
#endif
  if (!releaseArchiveFinalizeSource(true, true)) {
    failArchiveFinalization("The completed MP4 pathname changed while retiring the source identity guard.");
    return;
  }
  if (!releaseArchiveFinalizeTarget(true)) {
    QString retained_target;
#ifdef Q_OS_UNIX
    struct stat retained_target_stat{};
    const bool have_target_identity = archive_finalize_target_fd_ >= 0 &&
        ::fstat(archive_finalize_target_fd_, &retained_target_stat) == 0 && S_ISREG(retained_target_stat.st_mode);
    if (have_target_identity && path_has_file_identity(archive_finalize_target_path_, retained_target_stat)) {
      retained_target = archive_finalize_target_path_;
    } else if (
        have_target_identity && path_has_file_identity(archive_finalize_target_guard_path_, retained_target_stat)) {
      retained_target = archive_finalize_target_guard_path_;
    } else if (
        have_target_identity &&
        path_has_file_identity(archive_finalize_target_guard_path_ + ".hstream-pin", retained_target_stat)) {
      retained_target = archive_finalize_target_guard_path_ + ".hstream-pin";
    } else if (have_target_identity) {
      for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString candidate =
            available_final_archive_path(gameDirectory(archive_finalize_game_id_), archive_finalize_game_id_);
        if (candidate.isEmpty())
          break;
        int rescue_errno = 0;
        if (!link_open_file_no_replace(archive_finalize_target_fd_, candidate, &rescue_errno)) {
          if (rescue_errno == EEXIST)
            continue;
          break;
        }
        QString sync_error;
        if (path_has_file_identity(candidate, retained_target_stat) && sync_parent_directory(candidate, &sync_error)) {
          retained_target = candidate;
          break;
        }
      }
    }
#endif
    archive_finalize_failed_ = true;
    if (!retained_target.isEmpty()) {
      archive_finalize_source_path_ = retained_target;
      archive_finalize_blocked_source_path_ = retained_target;
    }
    releaseArchiveFinalizeTarget(false);
    releaseArchiveFinalizerOwnership(true);
    showArchiveFinalizationFailure(
        retained_target.isEmpty()
            ? "The completed MP4 identity guard could not be retired, and its pinned pathname could not be rescued."
            : QString(
                  "The completed MP4 identity guard could not be retired safely. The trusted MP4 was retained at %1.")
                  .arg(retained_target));
    return;
  }
  releaseArchiveFinalizerOwnership(true);
  output_states_["archive-file"]->setText("SAVED");
  if (archive_output_path_label_)
    archive_output_path_label_->setText(QString("Completed archive: %1").arg(archive_finalize_target_path_));
  archive_finalize_progress_->setRange(0, 1000);
  archive_finalize_progress_->setValue(1000);
  archive_finalize_progress_->setFormat("COMPLETED  •  100.0%");
  archive_finalize_headline_->setText("Completed video is ready");
  archive_finalize_detail_->setText(archive_finalize_target_path_);
  archive_finalize_icon_->setPixmap(style()->standardIcon(QStyle::SP_DialogApplyButton).pixmap(32, 32));
  archive_finalize_headline_->setProperty("finalizationState", "complete");
  archive_finalize_progress_->setProperty("finalizationState", "complete");
  for (QWidget* widget :
       {static_cast<QWidget*>(archive_finalize_headline_), static_cast<QWidget*>(archive_finalize_progress_)}) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
  }
  static_cast<StitchingCalibrationDialog*>(archive_finalize_dialog_)->setCloseAllowed(true);
  appendLog(
      QString("completed archive published: %1 (%2 bytes)%3")
          .arg(archive_finalize_target_path_)
          .arg(final_size)
          .arg(
              source_removed
                  ? QString()
                  : (source_was_replaced
                         ? QString("; replaced source pathname left untouched at %1").arg(archive_finalize_source_path_)
                         : QString("; source retained at %1").arg(archive_finalize_source_path_))));
  finishArchiveJobLog();
  QTimer::singleShot(500, archive_finalize_dialog_, &QDialog::accept);
  updateRunControls();
}

void HStreamWindow::showArchiveFinalizationFailure(const QString& failure_detail) {
  archive_finalize_failed_ = true;
  archive_finalize_stage_ = ArchiveFinalizeStage::kIdle;
  output_states_["archive-file"]->setText("ERROR");
  archive_finalize_progress_->setRange(0, 1000);
  archive_finalize_progress_->setValue(1000);
  archive_finalize_progress_->setFormat("ERROR");
  archive_finalize_headline_->setText("Video finalization failed");
  archive_finalize_detail_->setText(QString("%1\n\nThe completed archive was retained for recovery at:\n%2")
                                        .arg(failure_detail, archive_finalize_source_path_));
  archive_finalize_icon_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(32, 32));
  for (QWidget* widget :
       {static_cast<QWidget*>(archive_finalize_headline_), static_cast<QWidget*>(archive_finalize_progress_)}) {
    widget->setProperty("finalizationState", "failed");
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
  }
  archive_finalize_ok_button_->show();
  static_cast<StitchingCalibrationDialog*>(archive_finalize_dialog_)->setCloseAllowed(true);
  archive_finalize_dialog_->show();
  appendLog(QString("archive finalization failed: %1; recovery archive retained at %2")
                .arg(failure_detail, archive_finalize_source_path_));
  finishArchiveJobLog();
  updateRunControls();
}

void HStreamWindow::failArchiveFinalization(const QString& message) {
  if (archive_finalize_failed_)
    return;
  archive_finalize_failed_ = true;
  QFile::remove(archive_finalize_partial_path_);
  archive_finalize_partial_path_.clear();
  if (!archive_finalize_temporary_dir_.isEmpty()) {
    QDir().rmdir(archive_finalize_temporary_dir_);
    archive_finalize_temporary_dir_.clear();
  }
  QString failure_detail = message;
#ifdef Q_OS_UNIX
  if (archive_finalize_target_fd_ >= 0) {
    struct stat failed_target_stat{};
    if (::fstat(archive_finalize_target_fd_, &failed_target_stat) == 0 && S_ISREG(failed_target_stat.st_mode)) {
      QString cleanup_error;
      if (path_has_file_identity(archive_finalize_target_path_, failed_target_stat))
        remove_path_if_same_identity(archive_finalize_target_path_, failed_target_stat, &cleanup_error);
      if (path_has_file_identity(archive_finalize_target_guard_path_, failed_target_stat))
        remove_path_if_same_identity(archive_finalize_target_guard_path_, failed_target_stat, &cleanup_error);
    }
    releaseArchiveFinalizeTarget(false);
  }
  struct stat original_archive_stat{};
  const bool has_pinned_archive = archive_finalize_source_fd_ >= 0 &&
      ::fstat(archive_finalize_source_fd_, &original_archive_stat) == 0 && S_ISREG(original_archive_stat.st_mode) &&
      static_cast<quint64>(original_archive_stat.st_dev) == archive_finalize_source_device_ &&
      static_cast<quint64>(original_archive_stat.st_ino) == archive_finalize_source_inode_;
  if (has_pinned_archive) {
    const QString original_archive_path = archive_finalize_source_path_;
    const QString original_log_path = archive_job_log_path_;
    if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_SOURCE_REPLACEMENT")) {
      const QByteArray encoded_archive = QFile::encodeName(original_archive_path);
      ::unlink(encoded_archive.constData());
      const int replacement_fd =
          ::open(encoded_archive.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign archive source";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    bool has_job_log = false;
    struct stat original_log_stat{};
    int pinned_log_fd = -1;
    if (archive_job_log_.isOpen() && ::fstat(archive_job_log_.handle(), &original_log_stat) == 0 &&
        S_ISREG(original_log_stat.st_mode)) {
      pinned_log_fd = ::dup(archive_job_log_.handle());
      has_job_log = pinned_log_fd >= 0;
    } else if (!original_log_path.isEmpty() && archive_job_log_inode_ != 0) {
      const QString pinned_log_path =
          !archive_job_log_guard_path_.isEmpty() ? archive_job_log_guard_path_ : original_log_path;
      const QByteArray encoded_log = QFile::encodeName(pinned_log_path);
      pinned_log_fd = ::open(encoded_log.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      has_job_log = pinned_log_fd >= 0 && ::fstat(pinned_log_fd, &original_log_stat) == 0 &&
          S_ISREG(original_log_stat.st_mode) &&
          static_cast<quint64>(original_log_stat.st_dev) == archive_job_log_device_ &&
          static_cast<quint64>(original_log_stat.st_ino) == archive_job_log_inode_;
      if (!has_job_log && pinned_log_fd >= 0) {
        ::close(pinned_log_fd);
        pinned_log_fd = -1;
      }
    }
    if (archive_job_log_.isOpen())
      archive_job_log_.flush();
    if (has_job_log && qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_DROP_LOG_BEFORE_RECOVERY")) {
      if (archive_job_log_.isOpen())
        archive_job_log_.close();
      QString cleanup_error;
      if (path_has_file_identity(original_log_path, original_log_stat)) {
        remove_path_if_same_identity(
            original_log_path, original_log_stat, &cleanup_error, archive_job_log_guard_path_, &original_log_stat);
      }
      if (path_has_file_identity(archive_job_log_guard_path_, original_log_stat))
        remove_path_if_same_identity(archive_job_log_guard_path_, original_log_stat, &cleanup_error);
      archive_job_log_guard_path_.clear();
      archive_job_log_path_.clear();
      archive_job_log_enabled_ = false;
      ::close(pinned_log_fd);
      pinned_log_fd = -1;
      has_job_log = false;
    }
    if (has_job_log && qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_OPEN_LOG_REPLACEMENT")) {
      const QByteArray encoded_log = QFile::encodeName(original_log_path);
      ::unlink(encoded_log.constData());
      const int replacement_fd =
          ::open(encoded_log.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign open log pathname";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }

    QString failed_archive_path;
    QString recovery_move_error;
    for (int suffix = 0; suffix < 1000; ++suffix) {
      const QString candidate = failed_archive_candidate(original_archive_path, suffix);
      const QString candidate_log = candidate + ".log";
      struct stat reservation_stat{};
      bool candidate_log_reserved = false;
      bool candidate_log_is_marker = false;
      int marker_fd = -1;
      if (has_job_log) {
        int saved_errno = 0;
        if (!link_open_file_no_replace(pinned_log_fd, candidate_log, &saved_errno)) {
          if (saved_errno == EEXIST)
            continue;
          recovery_move_error = QString("could not reserve recovery log path %1: %2")
                                    .arg(candidate_log, QString::fromLocal8Bit(std::strerror(saved_errno)));
          break;
        }
        if (!path_has_file_identity(candidate_log, original_log_stat)) {
          QString cleanup_error;
          remove_path_if_same_identity(candidate_log, original_log_stat, &cleanup_error);
          recovery_move_error = QString("recovery log path was replaced while being reserved: %1").arg(candidate_log);
          break;
        }
        reservation_stat = original_log_stat;
        candidate_log_reserved = true;
      } else {
        const QByteArray encoded_candidate_log = QFile::encodeName(candidate_log);
        marker_fd = ::open(
            encoded_candidate_log.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (marker_fd < 0) {
          if (errno == EEXIST)
            continue;
          recovery_move_error = QString("could not reserve recovery log path %1: %2")
                                    .arg(candidate_log, QString::fromLocal8Bit(std::strerror(errno)));
          break;
        }
        if (::fstat(marker_fd, &reservation_stat) != 0 || !S_ISREG(reservation_stat.st_mode) ||
            ::fsync(marker_fd) != 0) {
          const int saved_errno = errno;
          ::close(marker_fd);
          marker_fd = -1;
          QString cleanup_error;
          remove_path_if_same_identity(candidate_log, reservation_stat, &cleanup_error);
          recovery_move_error = QString("could not secure recovery log reservation %1: %2")
                                    .arg(candidate_log, QString::fromLocal8Bit(std::strerror(saved_errno)));
          break;
        }
        candidate_log_reserved = true;
        candidate_log_is_marker = true;
      }

      if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_VIDEO_COLLISION") && suffix == 1) {
        QFile collision(candidate);
        if (collision.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
          collision.write("injected recovery video collision");
          collision.close();
        }
      }

      int move_errno = 0;
      const bool video_published = link_open_file_no_replace(archive_finalize_source_fd_, candidate, &move_errno) &&
          path_has_file_identity(candidate, original_archive_stat);
      if (!video_published) {
        QString cleanup_error;
        bool log_cleanup_succeeded = true;
        if (candidate_log_reserved)
          log_cleanup_succeeded = remove_path_if_same_identity(candidate_log, reservation_stat, &cleanup_error);
        if (marker_fd >= 0)
          ::close(marker_fd);
        if (!log_cleanup_succeeded) {
          recovery_move_error = QString("could not move recovery video and could not safely clean %1: %2")
                                    .arg(candidate_log, cleanup_error);
          break;
        }
        if (move_errno == EEXIST)
          continue;
        recovery_move_error = QString("could not move recovery video to %1: %2")
                                  .arg(candidate, QString::fromLocal8Bit(std::strerror(move_errno)));
        break;
      }

      if (!candidate_log_is_marker && suffix == 2 &&
          qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT")) {
        QString replacement_cleanup_error;
        if (remove_path_if_same_identity(
                candidate_log, original_log_stat, &replacement_cleanup_error, candidate, &original_archive_stat)) {
          QFile replacement(candidate_log);
          if (replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            replacement.write("injected recovery log replacement");
            replacement.close();
          }
        }
      }

      if (!path_has_file_identity(candidate_log, reservation_stat)) {
        QString video_cleanup_error;
        const bool video_cleaned = remove_path_if_same_identity(candidate, original_archive_stat, &video_cleanup_error);
        if (marker_fd >= 0)
          ::close(marker_fd);
        if (video_cleaned)
          continue;
        failed_archive_path = candidate;
        recovery_move_error = QString("recovery log path %1 was replaced and video rollback failed: %2")
                                  .arg(candidate_log, video_cleanup_error);
        failure_detail += "\n\n" + recovery_move_error;
        break;
      }

      QString candidate_source_guard_path;
      QString candidate_source_guard_error;
      if (!create_open_file_guard(
              archive_finalize_source_fd_, candidate, &candidate_source_guard_path, &candidate_source_guard_error)) {
        QString cleanup_error;
        remove_path_if_same_identity(candidate, original_archive_stat, &cleanup_error);
        if (candidate_log_reserved)
          remove_path_if_same_identity(candidate_log, reservation_stat, &cleanup_error);
        if (marker_fd >= 0)
          ::close(marker_fd);
        struct stat occupied_guard_stat{};
        const QByteArray encoded_guard = QFile::encodeName(candidate + ".hstream-pin");
        if (::lstat(encoded_guard.constData(), &occupied_guard_stat) == 0)
          continue;
        recovery_move_error =
            QString("could not protect recovery video at %1: %2").arg(candidate, candidate_source_guard_error);
        break;
      }
      QString candidate_log_guard_path;
      if (has_job_log) {
        QString candidate_log_guard_error;
        if (!create_open_file_guard(
                pinned_log_fd, candidate_log, &candidate_log_guard_path, &candidate_log_guard_error)) {
          QString cleanup_error;
          remove_path_if_same_identity(candidate_source_guard_path, original_archive_stat, &cleanup_error);
          remove_path_if_same_identity(candidate, original_archive_stat, &cleanup_error);
          remove_path_if_same_identity(candidate_log, original_log_stat, &cleanup_error);
          if (marker_fd >= 0)
            ::close(marker_fd);
          struct stat occupied_guard_stat{};
          const QByteArray encoded_guard = QFile::encodeName(candidate_log + ".hstream-pin");
          if (::lstat(encoded_guard.constData(), &occupied_guard_stat) == 0)
            continue;
          recovery_move_error =
              QString("could not protect recovery log at %1: %2").arg(candidate_log, candidate_log_guard_error);
          break;
        }
      }

      QString recovery_publication_sync_error;
      const bool force_recovery_publication_sync_failure =
          qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_PUBLICATION_SYNC_FAILURE");
      if (force_recovery_publication_sync_failure) {
        qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_PUBLICATION_SYNC_FAILURE");
        recovery_publication_sync_error = "recovery-publication sync failure requested by test";
      }
      const bool recovery_publication_is_durable = !force_recovery_publication_sync_failure &&
          ::fsync(archive_finalize_source_fd_) == 0 && (!has_job_log || ::fsync(pinned_log_fd) == 0) &&
          sync_parent_directory(candidate, &recovery_publication_sync_error);
      if (!recovery_publication_is_durable) {
        if (recovery_publication_sync_error.isEmpty())
          recovery_publication_sync_error = QString::fromLocal8Bit(std::strerror(errno));
        QString cleanup_error;
        if (!candidate_log_guard_path.isEmpty())
          remove_path_if_same_identity(candidate_log_guard_path, original_log_stat, &cleanup_error);
        remove_path_if_same_identity(candidate_source_guard_path, original_archive_stat, &cleanup_error);
        remove_path_if_same_identity(candidate, original_archive_stat, &cleanup_error);
        if (candidate_log_reserved)
          remove_path_if_same_identity(candidate_log, reservation_stat, &cleanup_error);
        if (marker_fd >= 0)
          ::close(marker_fd);
        recovery_move_error = QString("could not make the recovery pair durable before source cleanup: %1")
                                  .arg(recovery_publication_sync_error);
        if (!cleanup_error.isEmpty())
          recovery_move_error += QString("; partial recovery cleanup remains unresolved: %1").arg(cleanup_error);
        break;
      }

      if (candidate_log_is_marker) {
        if (qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_MARKER_REPLACEMENT") && suffix == 0) {
          QString marker_cleanup_error;
          if (remove_path_if_same_identity(
                  candidate_log, reservation_stat, &marker_cleanup_error, candidate, &original_archive_stat)) {
            QFile replacement(candidate_log);
            if (replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
              replacement.write("injected recovery marker replacement");
              replacement.close();
            }
          }
        }
        QString marker_cleanup_error;
        const bool marker_removed = remove_path_if_same_identity(
            candidate_log, reservation_stat, &marker_cleanup_error, candidate, &original_archive_stat);
        if (marker_fd >= 0)
          ::close(marker_fd);
        if (!marker_removed) {
          QString video_cleanup_error;
          const bool video_removed =
              remove_path_if_same_identity(candidate, original_archive_stat, &video_cleanup_error);
          QString guard_cleanup_error;
          remove_path_if_same_identity(candidate_source_guard_path, original_archive_stat, &guard_cleanup_error);
          if (video_removed)
            continue;
          failed_archive_path = candidate;
          recovery_move_error = QString("recovery log reservation %1 was replaced and video rollback failed: %2")
                                    .arg(candidate_log, video_cleanup_error);
          failure_detail += "\n\n" + recovery_move_error;
          break;
        }
      } else {
        if (archive_job_log_.isOpen()) {
          archive_job_log_.flush();
          archive_job_log_.close();
        }
        archive_job_log_path_ = candidate_log;
        QString reopen_error;
        const bool reopened = reopenArchiveJobLog(
            candidate_log,
            &reopen_error,
            static_cast<quint64>(original_log_stat.st_dev),
            static_cast<quint64>(original_log_stat.st_ino));
        if (!reopened) {
          QString video_cleanup_error;
          archive_job_log_path_ = original_log_path;
          if (!path_has_file_identity(candidate_log, original_log_stat) &&
              remove_path_if_same_identity(candidate, original_archive_stat, &video_cleanup_error)) {
            QString guard_cleanup_error;
            remove_path_if_same_identity(candidate_source_guard_path, original_archive_stat, &guard_cleanup_error);
            remove_path_if_same_identity(candidate_log_guard_path, original_log_stat, &guard_cleanup_error);
            QString original_reopen_error;
            archive_job_log_enabled_ = reopenArchiveJobLog(
                original_log_path,
                &original_reopen_error,
                static_cast<quint64>(original_log_stat.st_dev),
                static_cast<quint64>(original_log_stat.st_ino));
            continue;
          }
        }
        QString old_log_cleanup_error;
        const bool original_log_is_ours = path_has_file_identity(original_log_path, original_log_stat);
        const bool old_log_removed = !original_log_is_ours ||
            remove_path_if_same_identity(
                original_log_path, original_log_stat, &old_log_cleanup_error, candidate_log, &original_log_stat);
        if (!old_log_removed) {
          failure_detail += QString("\n\nThe recovery log is paired at %1, but its old link could not be removed: %2")
                                .arg(candidate_log, old_log_cleanup_error);
        }
        if (!reopened) {
          archive_job_log_enabled_ = false;
          appendLog(QString("archive job log was retained at %1, but file logging could not continue: %2")
                        .arg(candidate_log, reopen_error));
        } else {
          archive_job_log_enabled_ = true;
          appendLog(QString("archive job log moved with recovery archive: %1").arg(candidate_log));
        }
      }

      QString old_video_cleanup_error;
      const bool original_video_is_ours = path_has_file_identity(original_archive_path, original_archive_stat);
      if (original_video_is_ours &&
          !remove_path_if_same_identity(
              original_archive_path,
              original_archive_stat,
              &old_video_cleanup_error,
              candidate,
              &original_archive_stat)) {
        if (!path_has_file_identity(candidate, original_archive_stat) && has_job_log) {
          if (archive_job_log_.isOpen()) {
            archive_job_log_.flush();
            archive_job_log_.close();
          }
          int restore_log_errno = 0;
          if (link_open_file_no_replace(pinned_log_fd, original_log_path, &restore_log_errno) &&
              path_has_file_identity(original_log_path, original_log_stat)) {
            QString candidate_log_cleanup_error;
            remove_path_if_same_identity(
                candidate_log, original_log_stat, &candidate_log_cleanup_error, original_log_path, &original_log_stat);
            remove_path_if_same_identity(
                candidate_log_guard_path,
                original_log_stat,
                &candidate_log_cleanup_error,
                original_log_path,
                &original_log_stat);
            QString restored_log_guard;
            QString restored_guard_error;
            create_open_file_guard(pinned_log_fd, original_log_path, &restored_log_guard, &restored_guard_error);
            archive_job_log_guard_path_ = restored_log_guard;
            QString candidate_guard_cleanup_error;
            remove_path_if_same_identity(
                candidate_source_guard_path, original_archive_stat, &candidate_guard_cleanup_error);
            archive_job_log_path_ = original_log_path;
            QString original_reopen_error;
            archive_job_log_enabled_ = reopenArchiveJobLog(
                original_log_path,
                &original_reopen_error,
                static_cast<quint64>(original_log_stat.st_dev),
                static_cast<quint64>(original_log_stat.st_ino));
            continue;
          }
        }
        recovery_move_error =
            QString("could not safely retire the original recovery video link: %1").arg(old_video_cleanup_error);
        failure_detail += "\n\n" + recovery_move_error;
        break;
      }
      if (!archive_finalize_source_guard_path_.isEmpty()) {
        const QString old_guard_path = archive_finalize_source_guard_path_;
        QString guard_cleanup_error;
        if (remove_path_if_same_identity(
                old_guard_path, original_archive_stat, &guard_cleanup_error, candidate, &original_archive_stat)) {
        } else {
          failure_detail += QString(
                                "\n\nThe recovery video is paired at %1, but its identity guard was retained at "
                                "%2: %3")
                                .arg(candidate, old_guard_path, guard_cleanup_error);
        }
      }
      archive_finalize_source_guard_path_ = candidate_source_guard_path;
      if (has_job_log && !archive_job_log_guard_path_.isEmpty()) {
        const QString old_guard_path = archive_job_log_guard_path_;
        QString guard_cleanup_error;
        if (remove_path_if_same_identity(
                old_guard_path, original_log_stat, &guard_cleanup_error, candidate_log, &original_log_stat)) {
        } else {
          failure_detail += QString(
                                "\n\nThe recovery log is paired at %1, but its identity guard was retained at "
                                "%2: %3")
                                .arg(candidate_log, old_guard_path, guard_cleanup_error);
        }
      }
      if (has_job_log)
        archive_job_log_guard_path_ = candidate_log_guard_path;
      failed_archive_path = candidate;
      break;
    }

    if (!failed_archive_path.isEmpty() && has_job_log && pinned_log_fd >= 0) {
      archive_finalize_recovery_log_fd_ = ::dup(pinned_log_fd);
      archive_finalize_recovery_log_device_ = static_cast<quint64>(original_log_stat.st_dev);
      archive_finalize_recovery_log_inode_ = static_cast<quint64>(original_log_stat.st_ino);
    }
    if (pinned_log_fd >= 0)
      ::close(pinned_log_fd);

    if (!failed_archive_path.isEmpty()) {
      archive_finalize_source_path_ = failed_archive_path;
      releaseArchiveFinalizerOwnership(true);
      archive_finalize_pending_failure_detail_ = failure_detail;
      QString durability_error;
      if (startArchiveDurabilitySync(
              archive_finalize_source_path_, ArchiveFinalizeStage::kSyncRecovery, &durability_error)) {
        return;
      }
#ifdef Q_OS_UNIX
      if (archive_finalize_recovery_log_fd_ >= 0)
        ::close(archive_finalize_recovery_log_fd_);
#endif
      archive_finalize_recovery_log_fd_ = -1;
      archive_finalize_recovery_log_device_ = 0;
      archive_finalize_recovery_log_inode_ = 0;
      releaseArchiveFinalizeSource(false);
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
      failure_detail += QString(
                            "\n\nThe recovery file was renamed, but it could not be durability-synced. %1 Do not "
                            "start another archive run until this file has been copied to safety.")
                            .arg(durability_error);
    } else {
      archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
      failure_detail += QString(
                            "\n\nThe retained work file could not be moved away from the next run's output path.%1 "
                            "Do not start another archive run until this file has been copied to safety.")
                            .arg(recovery_move_error.isEmpty() ? QString() : " " + recovery_move_error);
    }
  } else {
    releaseArchiveFinalizerOwnership(true);
    releaseArchiveFinalizeSource(false);
  }
#else
  if (!QFileInfo::exists(archive_finalize_source_path_)) {
    releaseArchiveFinalizerOwnership(true);
  } else {
    archive_finalize_blocked_source_path_ = archive_finalize_source_path_;
    failure_detail += "\n\nThe work file could not be safely recovered on this platform.";
  }
#endif
  showArchiveFinalizationFailure(failure_detail);
}

void HStreamWindow::handleScoreboardSelectorOutput(const QString& line) {
  static const QRegularExpression selector_url(
      R"((https?://[^\s]+/\?token=[0-9a-fA-F]{64}))", QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = selector_url.match(line);
  if (match.hasMatch()) {
    const QString url_text = match.captured(1);
    if (url_text == scoreboard_selector_url_) {
      if (scoreboard_selection_dialog_) {
        scoreboard_selection_dialog_->show();
        scoreboard_selection_dialog_->raise();
        scoreboard_selection_dialog_->activateWindow();
      }
      return;
    }
    scoreboard_selector_url_ = url_text;
    if (preview_status_) {
      preview_status_->setText("Waiting for scoreboard selection");
    }

    QVector<QPoint> initial_points;
    const QString game_dir = gameDirectory(active_run_game_id_);
    const QString config_path = QDir(game_dir).filePath("config.yaml");
    try {
      if (QFileInfo(config_path).isFile()) {
        const YAML::Node polygon =
            YAML::LoadFile(config_path.toStdString())["rink"]["scoreboard"]["perspective_polygon"];
        bool disabled = polygon && polygon.IsSequence() && polygon.size() == 4;
        if (disabled) {
          for (size_t index = 0; index < 4; ++index) {
            if (!polygon[index].IsSequence() || polygon[index].size() != 2 || polygon[index][0].as<int>() != 0 ||
                polygon[index][1].as<int>() != 0) {
              disabled = false;
              break;
            }
          }
        }
        if (polygon && polygon.IsSequence() && polygon.size() == 4 && !disabled) {
          QVector<QPoint> loaded_points;
          for (size_t index = 0; index < 4; ++index) {
            if (!polygon[index].IsSequence() || polygon[index].size() != 2) {
              loaded_points.clear();
              break;
            }
            loaded_points.push_back(QPoint(polygon[index][0].as<int>(), polygon[index][1].as<int>()));
          }
          if (loaded_points.size() == 4)
            initial_points = loaded_points;
        }
      }
    } catch (const std::exception& exception) {
      initial_points.clear();
      appendLog(QString("could not load existing scoreboard points: %1").arg(exception.what()));
    }

    if (scoreboard_selection_dialog_) {
      scoreboard_selection_dialog_->closeAfterBackendCompletion();
      scoreboard_selection_dialog_ = nullptr;
    }
    auto* dialog =
        new ScoreboardSelectionDialog(QUrl(url_text), QDir(game_dir).filePath("s.png"), initial_points, this);
    dialog->cancellationFailed = [this](const QString& reason) {
      appendLog(QString("scoreboard selector cancellation failed; stopping pipeline: %1").arg(reason));
      if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning)
        stopPipeline();
    };
    scoreboard_selection_dialog_ = dialog;
    connect(dialog, &QObject::destroyed, this, [this, dialog]() {
      if (scoreboard_selection_dialog_ == dialog)
        scoreboard_selection_dialog_ = nullptr;
    });
    if (!dialog->loadError().isEmpty())
      appendLog(dialog->loadError());
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    appendLog(QString("scoreboard selector opened in Qt: %1").arg(url_text));
    return;
  }

  if (!scoreboard_selector_url_.isEmpty() &&
      (line.contains("Loaded scoreboard perspective polygon") || line.contains("Scoreboard overlay disabled"))) {
    if (preview_status_) {
      const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
      preview_status_->setText(
          active_run_is_calibration_
              ? (render_video ? "Continuous stitched preview running"
                              : "Stitching pipeline running without video rendering")
              : (render_video ? "Program pipeline running" : "Program pipeline running without video rendering"));
    }
    appendLog("scoreboard selection complete; pipeline continuing");
    if (scoreboard_selection_dialog_)
      scoreboard_selection_dialog_->closeAfterBackendCompletion();
  }
}

QWidget* HStreamWindow::previewSurfaceForChannel(const QString& channel) const {
  if (channel == "program")
    return preview_surface_;
  if (channel == "stitched")
    return stitched_surface_;
  if (!channel.startsWith("source"))
    return nullptr;
  bool valid_index = false;
  const int source_index = channel.mid(6).toInt(&valid_index);
  return valid_index && source_index >= 0 && source_index < static_cast<int>(camera_preview_surfaces_.size())
      ? camera_preview_surfaces_[source_index]
      : nullptr;
}

QWidget* HStreamWindow::previewTargetForChannel(const QString& channel) const {
  if (channel == "program")
    return preview_render_target_;
  if (channel == "stitched")
    return stitched_render_target_;
  if (!channel.startsWith("source"))
    return nullptr;
  bool valid_index = false;
  const int source_index = channel.mid(6).toInt(&valid_index);
  return valid_index && source_index >= 0 && source_index < static_cast<int>(camera_preview_render_targets_.size())
      ? camera_preview_render_targets_[source_index]
      : nullptr;
}

bool HStreamWindow::handleGpuPreviewStatus(const QString& line) {
  static const QRegularExpression runtime_ready_pattern(
      R"(^HSTREAM_PREVIEW_RUNTIME status=ready channel=(\S+) generation=(\d+)$)");
  const QRegularExpressionMatch runtime_ready = runtime_ready_pattern.match(line);
  if (runtime_ready.hasMatch()) {
    if (!pipeline_render_embedded_ || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning)
      return true;
    bool generation_valid = false;
    const QString backend_channel = runtime_ready.captured(1);
    const quint64 generation = runtime_ready.captured(2).toULongLong(&generation_valid);
    if (!generation_valid || generation < preview_generation_)
      return true;
    preview_runtime_ready_ = true;
    preview_generation_ = generation;
    const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
    const QString selected_channel = selectedPipelinePreviewChannel();
    if (!render_video) {
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      if (backend_channel != "none") {
        requestPipelinePreviewChannel("none", PreviewRequestReason::kRenderToggle);
      } else {
        if (preview_status_)
          preview_status_->setText("Pipeline running without video rendering");
        appendLog(QString("GPU preview backend ready with rendering disabled generation=%1").arg(generation));
      }
    } else if (selected_channel.isEmpty()) {
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      if (backend_channel != "none") {
        requestPipelinePreviewChannel("none", PreviewRequestReason::kStartup);
      } else {
        active_preview_channel_.clear();
        if (preview_status_)
          preview_status_->setText("Pipeline inspector active; GPU preview is idle");
        appendLog(QString("GPU preview backend ready with inspector selected generation=%1").arg(generation));
      }
    } else if (backend_channel != "none" && selected_channel == backend_channel) {
      pending_preview_channel_ = backend_channel;
      pending_preview_generation_ = generation;
      preview_recovery_attempts_ = 0;
      setPreviewFocusAvailable(backend_channel, false);
      if (QWidget* surface = previewSurfaceForChannel(backend_channel)) {
        surface->setProperty("previewRendererState", "activating");
        surface->show();
      }
      if (QWidget* target = previewTargetForChannel(backend_channel)) {
        target->setProperty("previewRendererState", "activating");
        target->show();
      }
      appendLog(QString("GPU preview backend ready channel=%1 generation=%2").arg(backend_channel).arg(generation));
      schedulePreviewReadyTimeout(backend_channel, generation, previewReadyTimeoutMs());
    } else if (!requestPipelinePreviewChannel(selected_channel, PreviewRequestReason::kStartup)) {
      appendLog(
          QString("could not activate selected GPU preview channel %1 after backend startup").arg(selected_channel));
    }
    return true;
  }
  static const QRegularExpression pattern(
      R"(^HSTREAM_PREVIEW channel=(\S+) status=(\S+) generation=(\d+) message=(.*)$)");
  const QRegularExpressionMatch match = pattern.match(line);
  if (!match.hasMatch())
    return false;

  const QString channel = match.captured(1);
  const QString status = match.captured(2);
  bool generation_valid = false;
  const quint64 generation = match.captured(3).toULongLong(&generation_valid);
  const QString message = match.captured(4);
  if (!generation_valid || generation < preview_generation_)
    return true;

  QWidget* surface = previewSurfaceForChannel(channel);
  QWidget* target = previewTargetForChannel(channel);
  if (surface) {
    surface->setProperty("previewRendererState", status);
    surface->setProperty("previewRendererGeneration", generation);
  }
  if (target) {
    target->setProperty("previewRendererState", status);
    target->setProperty("previewRendererGeneration", generation);
  }
  QLabel* notice = nullptr;
  if (channel == "program") {
    notice = preview_external_notice_;
  } else if (channel == "stitched") {
    notice = stitched_external_notice_;
  } else if (channel.startsWith("source")) {
    bool valid_index = false;
    const int source_index = channel.mid(6).toInt(&valid_index);
    if (valid_index && source_index >= 0 && source_index < static_cast<int>(camera_preview_notices_.size()))
      notice = camera_preview_notices_[source_index];
  }
  const bool matches_pending = channel == pending_preview_channel_ && generation == pending_preview_generation_;
  if (status == "activated") {
    setPreviewFocusAvailable(channel, false);
    if (matches_pending) {
      if (preview_status_)
        preview_status_->setText(QString("Waiting for first GPU frame from %1").arg(channel));
    }
    return true;
  }
  if (status == "ready") {
    if (matches_pending) {
      active_preview_channel_ = channel;
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      preview_recovery_attempts_ = 0;
      if (preview_status_)
        preview_status_->setText(QString("%1 GPU preview ready").arg(channel == "program" ? "Program" : channel));
    }
    if (surface)
      surface->show();
    if (target)
      target->show();
    if (notice)
      notice->hide();
    setPreviewFocusAvailable(channel, true);
    setPreviewRenderingLayout(true);
    if (preview_frame_channels_received_.insert(channel).second)
      appendLog(QString("GPU preview ready channel=%1 generation=%2").arg(channel).arg(generation));
  } else if (status == "failed" || status == "unavailable") {
    const bool affected_active = channel == active_preview_channel_;
    const bool affected_selected = matches_pending || affected_active;
    if (matches_pending) {
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      preview_recovery_attempts_ = 0;
    }
    if (affected_active)
      active_preview_channel_.clear();
    setPreviewFocusAvailable(channel, false);
    if (target)
      target->hide();
    if (affected_selected && preview_focus_mode_)
      setPreviewFocusMode(false, focused_preview_tab_ >= 0 ? focused_preview_tab_ : 0);
    if (surface)
      surface->hide();
    if (affected_selected)
      setPreviewRenderingLayout(false);
    if (notice) {
      notice->setText(
          status == "failed" ? QString("GPU preview failed\n%1").arg(message)
                             : QString("GPU preview unavailable\n%1").arg(message));
      notice->show();
    }
    if ((matches_pending || affected_active) && preview_status_)
      preview_status_->setText(
          status == "failed" ? "GPU preview failed; pipeline continues"
                             : "GPU preview unavailable; pipeline continues");
    appendLog(QString("GPU preview %1 channel=%2 generation=%3 message=%4")
                  .arg(status, channel)
                  .arg(generation)
                  .arg(message));
  } else if (status == "deactivated") {
    setPreviewFocusAvailable(channel, false);
    if (channel == active_preview_channel_)
      active_preview_channel_.clear();
    if (channel == "none" && matches_pending) {
      const bool inspector_idle =
          (!render_video_toggle_ || render_video_toggle_->isChecked()) && selectedPipelinePreviewChannel().isEmpty();
      active_preview_channel_.clear();
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      preview_disable_attempts_ = 0;
      std::vector<QWidget*> surfaces = {preview_surface_, stitched_surface_};
      surfaces.insert(surfaces.end(), camera_preview_surfaces_.begin(), camera_preview_surfaces_.end());
      std::vector<QWidget*> targets = {preview_render_target_, stitched_render_target_};
      targets.insert(targets.end(), camera_preview_render_targets_.begin(), camera_preview_render_targets_.end());
      for (QWidget* item : surfaces) {
        if (item)
          item->hide();
      }
      for (QWidget* item : targets) {
        if (item)
          item->hide();
      }
      for (QLabel* disabled_notice : {preview_external_notice_, stitched_external_notice_}) {
        if (disabled_notice) {
          disabled_notice->setText(
              inspector_idle ? "GPU preview is idle while the Pipeline inspector is selected"
                             : "Video rendering is disabled");
          disabled_notice->show();
        }
      }
      for (QLabel* disabled_notice : camera_preview_notices_) {
        if (disabled_notice) {
          disabled_notice->setText(
              inspector_idle ? "GPU preview is idle while the Pipeline inspector is selected"
                             : "Video rendering is disabled");
          disabled_notice->show();
        }
      }
      if (preview_status_)
        preview_status_->setText(
            inspector_idle ? "Pipeline inspector active; GPU preview is idle"
                           : "Pipeline running without video rendering");
      appendLog(
          inspector_idle ? QString("GPU preview idle for Pipeline inspector generation=%1; display branch is quiescent")
                               .arg(generation)
                         : QString("GPU preview disabled generation=%1; display branch is quiescent").arg(generation));
      return true;
    }
    // A deactivation for a channel being superseded must not hide the newly
    // pending selected target. Keeping it mapped lets the replacement GLX
    // renderer present without requiring another tab change.
    if (!matches_pending) {
      if (target)
        target->hide();
      if (surface)
        surface->hide();
    }
  }
  return true;
}

bool HStreamWindow::handlePreviewOverlayResponse(const QString& line) {
  static const QRegularExpression pattern(
      R"(^HSTREAM_PREVIEW_OVERLAYS status=(applied|failed) generation=(\d+) players=([01]) play=([01]) rink=([01])(?: reason=(.*))?$)");
  const QRegularExpressionMatch match = pattern.match(line);
  if (!match.hasMatch())
    return false;

  bool generation_valid = false;
  const quint64 generation = match.captured(2).toULongLong(&generation_valid);
  if (!generation_valid || generation == 0 || generation > preview_overlay_generation_)
    return true;
  if (match.captured(1) == "applied") {
    const bool players = match.captured(3) == "1";
    const bool play = match.captured(4) == "1";
    const bool rink = match.captured(5) == "1";
    if (generation != pending_preview_overlay_generation_) {
      const bool resolves_reconciliation = generation == unresolved_preview_overlay_reconciliation_generation_;
      if (resolves_reconciliation)
        unresolved_preview_overlay_reconciliation_generation_ = 0;
      // A newer request still in flight will supersede this state in command
      // order. If the latest request already timed out, however, the backend
      // has just applied a choice that the UI rolled back. Re-send the
      // confirmed checkbox state with a new generation so the two sides
      // converge without presenting the stale acknowledgement as success.
      if (pending_preview_overlay_generation_ == 0 &&
          (generation == preview_overlay_generation_ || resolves_reconciliation)) {
        preview_overlay_stale_apply_observed_ = false;
        const bool matches_confirmed = players == confirmed_show_player_tracking_ &&
            play == confirmed_show_play_tracking_ && rink == confirmed_show_rink_mask_;
        if (matches_confirmed) {
          resetPreviewOverlayReconciliationState();
          appendLog(QString("late preview overlay acknowledgement matches confirmed state generation=%1; settled")
                        .arg(generation));
        } else {
          preview_overlay_reconciliation_fallback_valid_ = true;
          preview_overlay_reconciliation_fallback_players_ = players;
          preview_overlay_reconciliation_fallback_play_ = play;
          preview_overlay_reconciliation_fallback_rink_ = rink;
          appendLog(QString("preview overlay acknowledgement arrived after rollback generation=%1; reconciling")
                        .arg(generation));
          setRuntimePreviewOverlays(true);
        }
      } else if (pending_preview_overlay_generation_ != 0) {
        // The older generation did change backend state. Remember that fact
        // until the newest transaction either confirms it has superseded the
        // change or fails and requires an explicit restore.
        preview_overlay_stale_apply_observed_ = true;
        preview_overlay_reconciliation_fallback_valid_ = true;
        preview_overlay_reconciliation_fallback_players_ = players;
        preview_overlay_reconciliation_fallback_play_ = play;
        preview_overlay_reconciliation_fallback_rink_ = rink;
      }
      return true;
    }
    pending_preview_overlay_generation_ = 0;
    setConfirmedPreviewOverlays(players, play, rink);
    resetPreviewOverlayReconciliationState();
    appendLog(QString("preview overlays players=%1 play=%2 rink=%3 apply=live")
                  .arg(confirmed_show_player_tracking_ ? 1 : 0)
                  .arg(confirmed_show_play_tracking_ ? 1 : 0)
                  .arg(confirmed_show_rink_mask_ ? 1 : 0));
    return true;
  }
  if (generation == pending_preview_overlay_generation_) {
    const bool reconcile_stale_apply = preview_overlay_stale_apply_observed_;
    const bool failed_reconciliation = pending_preview_overlay_is_reconciliation_;
    preview_overlay_stale_apply_observed_ = false;
    pending_preview_overlay_generation_ = 0;
    pending_preview_overlay_is_reconciliation_ = false;
    const QString reason = match.captured(6).isEmpty() ? "backend-rejected" : match.captured(6);
    if (failed_reconciliation && adoptPreviewOverlayReconciliationFallback(reason))
      return true;
    restoreConfirmedPreviewOverlays(reason);
    if (reconcile_stale_apply) {
      setRuntimePreviewOverlays(true);
    } else if (unresolved_preview_overlay_reconciliation_generation_ == 0) {
      resetPreviewOverlayReconciliationState();
    }
  } else if (
      generation == unresolved_preview_overlay_reconciliation_generation_ &&
      preview_overlay_reconciliation_fallback_valid_) {
    unresolved_preview_overlay_reconciliation_generation_ = 0;
    if (pending_preview_overlay_generation_ == 0) {
      adoptPreviewOverlayReconciliationFallback(
          match.captured(6).isEmpty() ? "late-backend-rejected" : "late-" + match.captured(6));
    } else {
      preview_overlay_stale_apply_observed_ = true;
      appendLog(QString("late failed preview overlay reconciliation generation=%1; preserving backend state")
                    .arg(generation));
    }
  }
  return true;
}

void HStreamWindow::switchPipelineRenderTarget(int tab_index) {
  if (!pipeline_render_embedded_ || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning ||
      tab_index < 0 || (render_video_toggle_ && !render_video_toggle_->isChecked())) {
    return;
  }
  const QString channel =
      hm::ui_internal::preview_channel_for_tab(tab_index, static_cast<int>(camera_preview_render_targets_.size()));
  requestPipelinePreviewChannel(channel, PreviewRequestReason::kTabChange);
}

QString HStreamWindow::selectedPipelinePreviewChannel() const {
  if (!preview_tabs_)
    return "program";
  return hm::ui_internal::preview_channel_for_tab(
      preview_tabs_->currentIndex(), static_cast<int>(camera_preview_render_targets_.size()));
}

int HStreamWindow::previewReadyTimeoutMs() const {
  bool test_timeout_valid = false;
  const int test_timeout = qEnvironmentVariableIntValue("HSTREAM_UI_TEST_PREVIEW_TIMEOUT_MS", &test_timeout_valid);
  if (test_timeout_valid && test_timeout > 0)
    return test_timeout;
  constexpr int kFirstFrameWaitMs = 30 * 1000;
  constexpr int kBackgroundRecoveryWaitMs = 60 * 1000;
  constexpr int kRapidRecoveryAttempts = 3;
  return preview_recovery_attempts_ < kRapidRecoveryAttempts ? kFirstFrameWaitMs : kBackgroundRecoveryWaitMs;
}

bool HStreamWindow::requestPipelinePreviewChannel(const QString& channel, PreviewRequestReason reason) {
  if (!pipeline_render_embedded_ || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning ||
      channel.isEmpty() || !preview_runtime_ready_) {
    return false;
  }
  const bool force = reason != PreviewRequestReason::kTabChange;
  if (!force &&
      (channel == pending_preview_channel_ ||
       (pending_preview_channel_.isEmpty() && channel == active_preview_channel_))) {
    return true;
  }
  const quint64 generation = preview_generation_ + 1;
  const QByteArray command = QString("@set-preview-active %1 %2\n").arg(channel).arg(generation).toUtf8();
  if (pipeline_process_->write(command) != command.size()) {
    appendLog(QString("could not activate GPU preview channel %1").arg(channel));
    return false;
  }
  preview_generation_ = generation;
  pending_preview_channel_ = channel;
  pending_preview_generation_ = generation;
  setAllPreviewFocusAvailable(false);
  if (reason != PreviewRequestReason::kRecovery)
    preview_recovery_attempts_ = 0;
  if (channel != "none") {
    if (QWidget* surface = previewSurfaceForChannel(channel)) {
      surface->setProperty("previewRendererState", "activating");
      surface->show();
    }
    if (QWidget* target = previewTargetForChannel(channel)) {
      target->setProperty("previewRendererState", "activating");
      target->show();
    }
  }
  appendLog(QString("GPU preview requested channel=%1 generation=%2 reason=%3")
                .arg(channel)
                .arg(generation)
                .arg(
                    reason == PreviewRequestReason::kStartup
                        ? "startup"
                        : (reason == PreviewRequestReason::kRecovery
                               ? "recovery"
                               : (reason == PreviewRequestReason::kRenderToggle ? "render-toggle" : "tab-change"))));
  if (channel != "none")
    schedulePreviewReadyTimeout(channel, generation, previewReadyTimeoutMs());
  else
    schedulePreviewDisableTimeout(generation, previewDisableTimeoutMs());
  return true;
}

void HStreamWindow::schedulePreviewReadyTimeout(const QString& channel, quint64 generation, int timeout_ms) {
  QTimer::singleShot(timeout_ms, this, [this, channel, generation]() {
    if (pending_preview_channel_ != channel || pending_preview_generation_ != generation || !pipeline_process_ ||
        pipeline_process_->state() == QProcess::NotRunning) {
      return;
    }
    if (selectedPipelinePreviewChannel() != channel) {
      return;
    }
    constexpr int kRapidRecoveryAttempts = 3;
    if (preview_recovery_attempts_ < kRapidRecoveryAttempts + 1)
      ++preview_recovery_attempts_;
    appendLog(QString("GPU preview first-frame wait exceeded channel=%1 generation=%2 recovery-attempt=%3")
                  .arg(channel)
                  .arg(generation)
                  .arg(preview_recovery_attempts_));
    if (preview_status_) {
      preview_status_->setText(
          preview_recovery_attempts_ <= kRapidRecoveryAttempts
              ? QString("GPU preview delayed; retrying %1 (%2/%3)")
                    .arg(channel)
                    .arg(preview_recovery_attempts_)
                    .arg(kRapidRecoveryAttempts)
              : QString("GPU preview delayed; automatic recovery continues for %1").arg(channel));
    }
    if (!requestPipelinePreviewChannel(channel, PreviewRequestReason::kRecovery)) {
      // A full process pipe must not turn a renderer watchdog into a terminal
      // failure. Keep the same generation pending and try again later.
      schedulePreviewReadyTimeout(channel, generation, previewReadyTimeoutMs());
    }
  });
}

int HStreamWindow::previewDisableTimeoutMs() const {
  bool test_timeout_valid = false;
  const int test_timeout = qEnvironmentVariableIntValue("HSTREAM_UI_TEST_PREVIEW_TIMEOUT_MS", &test_timeout_valid);
  return test_timeout_valid && test_timeout > 0 ? test_timeout : 5000;
}

void HStreamWindow::schedulePreviewDisableTimeout(quint64 generation, int timeout_ms) {
  QTimer::singleShot(timeout_ms, this, [this, generation, timeout_ms]() {
    if (pending_preview_channel_ != "none" || pending_preview_generation_ != generation || !pipeline_process_ ||
        pipeline_process_->state() == QProcess::NotRunning) {
      return;
    }
    const bool render_disabled = render_video_toggle_ && !render_video_toggle_->isChecked();
    const bool inspector_idle =
        (!render_video_toggle_ || render_video_toggle_->isChecked()) && selectedPipelinePreviewChannel().isEmpty();
    if (!render_disabled && !inspector_idle)
      return;
    if (pipeline_paused_) {
      const QString paused_status = inspector_idle
          ? "Pipeline inspector will finish quiescing GPU preview when playback resumes"
          : "GPU preview will finish disabling when the paused pipeline resumes";
      if (!preview_status_ || preview_status_->text() != paused_status) {
        appendLog(paused_status);
      }
      if (preview_status_)
        preview_status_->setText(paused_status);
      schedulePreviewDisableTimeout(generation, std::max(timeout_ms, 250));
      return;
    }
    constexpr int kDisableRetryLimit = 3;
    if (preview_disable_attempts_ >= kDisableRetryLimit) {
      if (render_disabled) {
        recoverPreviewDisableFailure("the backend did not acknowledge the render-off request");
        return;
      }
      if (preview_status_)
        preview_status_->setText("Pipeline inspector is waiting for GPU preview to quiesce");
      appendLog("Pipeline inspector GPU idle acknowledgement remains delayed; continuing bounded-rate retries");
      if (!requestPipelinePreviewChannel("none", PreviewRequestReason::kRecovery))
        schedulePreviewDisableTimeout(generation, timeout_ms);
      return;
    }
    ++preview_disable_attempts_;
    appendLog(QString("GPU preview %1 acknowledgement delayed; retrying (%2/%3)")
                  .arg(inspector_idle ? "inspector idle" : "disable")
                  .arg(preview_disable_attempts_)
                  .arg(kDisableRetryLimit));
    if (!requestPipelinePreviewChannel(
            "none", inspector_idle ? PreviewRequestReason::kRecovery : PreviewRequestReason::kRenderToggle)) {
      schedulePreviewDisableTimeout(generation, timeout_ms);
    }
  });
}

void HStreamWindow::scheduleInspectorPreviewIdleRetry(int timeout_ms) {
  QTimer::singleShot(timeout_ms, this, [this, timeout_ms]() {
    if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning || !preview_tabs_ ||
        preview_tabs_->currentIndex() != pipeline_inspector_tab_index_ ||
        (render_video_toggle_ && !render_video_toggle_->isChecked())) {
      return;
    }
    if (!requestPipelinePreviewChannel("none", PreviewRequestReason::kRecovery))
      scheduleInspectorPreviewIdleRetry(timeout_ms);
  });
}

void HStreamWindow::recoverPreviewDisableFailure(const QString& reason, bool force) {
  if (!render_video_toggle_ || render_video_toggle_->isChecked() || !pipeline_process_ ||
      pipeline_process_->state() == QProcess::NotRunning ||
      (!force && (pending_preview_channel_ != "none" || pending_preview_generation_ == 0))) {
    return;
  }
  QString channel = selectedPipelinePreviewChannel();
  const bool inspector_idle = channel.isEmpty();
  if (channel.isEmpty())
    channel = active_preview_channel_;
  pending_preview_channel_.clear();
  pending_preview_generation_ = 0;
  preview_disable_attempts_ = 0;
  if (render_video_toggle_) {
    const QSignalBlocker blocker(render_video_toggle_);
    render_video_toggle_->setChecked(true);
  }
  for (QCheckBox* toggle : {show_player_tracking_toggle_, show_play_tracking_toggle_, show_rink_mask_toggle_}) {
    if (toggle)
      toggle->setEnabled(!isCalibrationRun());
  }
  setRuntimeRenderAudioMuted(false);
  updatePlaybackSeekControls();
  if (inspector_idle) {
    setPreviewRenderingLayout(false);
    if (preview_status_)
      preview_status_->setText("Pipeline inspector is waiting for GPU preview to quiesce");
    appendLog(QString("GPU preview disable failed (%1) while Pipeline inspector is selected; continuing idle retries")
                  .arg(reason));
    if (!requestPipelinePreviewChannel("none", PreviewRequestReason::kRecovery))
      scheduleInspectorPreviewIdleRetry(previewDisableTimeoutMs());
    return;
  }
  setPreviewRenderingLayout(true);
  if (QWidget* surface = previewSurfaceForChannel(channel))
    surface->show();
  if (QWidget* target = previewTargetForChannel(channel)) {
    target->show();
    setPreviewFocusAvailable(channel, target->property("previewRendererState").toString() == "ready");
  }
  if (preview_status_)
    preview_status_->setText("Could not disable GPU preview; rendering remains enabled");
  appendLog(QString("GPU preview disable failed (%1); restoring rendering").arg(reason));
  if (preview_runtime_ready_ && !channel.isEmpty() &&
      !requestPipelinePreviewChannel(channel, PreviewRequestReason::kRenderToggle)) {
    appendLog(QString("could not reconcile restored GPU preview channel %1").arg(channel));
  }
}

bool HStreamWindow::setRuntimeRenderAudioMuted(bool muted) {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    return false;
  }
  const QByteArray command = QString("@set-render-audio-muted %1\n").arg(muted ? 1 : 0).toUtf8();
  if (pipeline_process_->write(command) != command.size()) {
    appendLog(muted ? "could not mute local render audio" : "could not restore local render audio");
    return false;
  }
  appendLog(
      muted ? "muting local render audio with video rendering" : "restoring local render audio with video rendering");
  return true;
}

void HStreamWindow::setRuntimePreviewOverlays(bool reconciliation) {
  constexpr int kMaxReconciliationAttempts = 1;
  if (reconciliation) {
    if (preview_overlay_reconciliation_attempts_ >= kMaxReconciliationAttempts) {
      appendLog("preview overlay reconciliation stopped after bounded retry");
      adoptPreviewOverlayReconciliationFallback("bounded-retry-limit");
      return;
    }
    ++preview_overlay_reconciliation_attempts_;
  }
  const bool players = show_player_tracking_toggle_ && show_player_tracking_toggle_->isChecked();
  const bool play = show_play_tracking_toggle_ && show_play_tracking_toggle_->isChecked();
  const bool rink = show_rink_mask_toggle_ && show_rink_mask_toggle_->isChecked();
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    confirmed_show_player_tracking_ = players;
    confirmed_show_play_tracking_ = play;
    confirmed_show_rink_mask_ = rink;
    pending_preview_overlay_generation_ = 0;
    resetPreviewOverlayReconciliationState();
    appendLog(QString("preview overlays players=%1 play=%2 rink=%3 apply=next-start")
                  .arg(players ? 1 : 0)
                  .arg(play ? 1 : 0)
                  .arg(rink ? 1 : 0));
    return;
  }
  if (!pipeline_render_embedded_) {
    if (!reconciliation) {
      resetPreviewOverlayReconciliationState();
      restoreConfirmedPreviewOverlays("embedded-preview-unavailable");
    } else if (!adoptPreviewOverlayReconciliationFallback("embedded-preview-unavailable")) {
      restoreConfirmedPreviewOverlays("embedded-preview-unavailable");
    }
    return;
  }
  const quint64 generation = ++preview_overlay_generation_;
  const QByteArray command = QString("@set-preview-overlays %1 %2 %3 %4\n")
                                 .arg(generation)
                                 .arg(players ? 1 : 0)
                                 .arg(play ? 1 : 0)
                                 .arg(rink ? 1 : 0)
                                 .toUtf8();
  if (pipeline_process_->write(command) != command.size()) {
    if (!reconciliation || !adoptPreviewOverlayReconciliationFallback("pipeline-command-write"))
      restoreConfirmedPreviewOverlays("pipeline-command-write");
    return;
  }
  if (!reconciliation)
    preparePreviewOverlayUserRequest();
  pending_preview_overlay_generation_ = generation;
  pending_preview_overlay_is_reconciliation_ = reconciliation;
  if (reconciliation)
    unresolved_preview_overlay_reconciliation_generation_ = 0;
  appendLog(QString("preview overlays players=%1 play=%2 rink=%3 apply=pending")
                .arg(players ? 1 : 0)
                .arg(play ? 1 : 0)
                .arg(rink ? 1 : 0));
  QTimer::singleShot(
      runtimeControlAckTimeoutMs(), this, [this, generation]() { timeoutPreviewOverlayRequest(generation); });
}

void HStreamWindow::setConfirmedPreviewOverlays(bool players, bool play, bool rink) {
  confirmed_show_player_tracking_ = players;
  confirmed_show_play_tracking_ = play;
  confirmed_show_rink_mask_ = rink;
  const QSignalBlocker player_blocker(show_player_tracking_toggle_);
  const QSignalBlocker play_blocker(show_play_tracking_toggle_);
  const QSignalBlocker rink_blocker(show_rink_mask_toggle_);
  if (show_player_tracking_toggle_)
    show_player_tracking_toggle_->setChecked(players);
  if (show_play_tracking_toggle_)
    show_play_tracking_toggle_->setChecked(play);
  if (show_rink_mask_toggle_)
    show_rink_mask_toggle_->setChecked(rink);
}

void HStreamWindow::preparePreviewOverlayUserRequest() {
  if (pending_preview_overlay_is_reconciliation_ && pending_preview_overlay_generation_ != 0 &&
      preview_overlay_reconciliation_fallback_valid_) {
    unresolved_preview_overlay_reconciliation_generation_ = pending_preview_overlay_generation_;
  }
  pending_preview_overlay_is_reconciliation_ = false;
  if (!preview_overlay_reconciliation_fallback_valid_)
    unresolved_preview_overlay_reconciliation_generation_ = 0;
  preview_overlay_reconciliation_attempts_ = 0;
}

void HStreamWindow::resetPreviewOverlayReconciliationState() {
  preview_overlay_stale_apply_observed_ = false;
  pending_preview_overlay_is_reconciliation_ = false;
  unresolved_preview_overlay_reconciliation_generation_ = 0;
  preview_overlay_reconciliation_fallback_valid_ = false;
  preview_overlay_reconciliation_fallback_players_ = false;
  preview_overlay_reconciliation_fallback_play_ = false;
  preview_overlay_reconciliation_fallback_rink_ = false;
  preview_overlay_reconciliation_attempts_ = 0;
}

bool HStreamWindow::adoptPreviewOverlayReconciliationFallback(const QString& reason) {
  if (!preview_overlay_reconciliation_fallback_valid_)
    return false;
  const bool players = preview_overlay_reconciliation_fallback_players_;
  const bool play = preview_overlay_reconciliation_fallback_play_;
  const bool rink = preview_overlay_reconciliation_fallback_rink_;
  setConfirmedPreviewOverlays(players, play, rink);
  resetPreviewOverlayReconciliationState();
  appendLog(QString("preview overlays players=%1 play=%2 rink=%3 apply=backend-state reason=%4")
                .arg(players ? 1 : 0)
                .arg(play ? 1 : 0)
                .arg(rink ? 1 : 0)
                .arg(reason));
  return true;
}

void HStreamWindow::restoreConfirmedPreviewOverlays(const QString& reason) {
  const QSignalBlocker player_blocker(show_player_tracking_toggle_);
  const QSignalBlocker play_blocker(show_play_tracking_toggle_);
  const QSignalBlocker rink_blocker(show_rink_mask_toggle_);
  if (show_player_tracking_toggle_)
    show_player_tracking_toggle_->setChecked(confirmed_show_player_tracking_);
  if (show_play_tracking_toggle_)
    show_play_tracking_toggle_->setChecked(confirmed_show_play_tracking_);
  if (show_rink_mask_toggle_)
    show_rink_mask_toggle_->setChecked(confirmed_show_rink_mask_);
  appendLog(QString("preview overlays players=%1 play=%2 rink=%3 apply=failed reason=%4")
                .arg(confirmed_show_player_tracking_ ? 1 : 0)
                .arg(confirmed_show_play_tracking_ ? 1 : 0)
                .arg(confirmed_show_rink_mask_ ? 1 : 0)
                .arg(reason));
}

void HStreamWindow::timeoutPreviewOverlayRequest(quint64 generation) {
  if (generation != pending_preview_overlay_generation_)
    return;
  const bool reconcile_stale_apply = preview_overlay_stale_apply_observed_;
  const bool timed_out_reconciliation = pending_preview_overlay_is_reconciliation_;
  preview_overlay_stale_apply_observed_ = false;
  pending_preview_overlay_generation_ = 0;
  pending_preview_overlay_is_reconciliation_ = false;
  if (timed_out_reconciliation)
    unresolved_preview_overlay_reconciliation_generation_ = generation;
  restoreConfirmedPreviewOverlays("acknowledgement-timeout");
  if (reconcile_stale_apply) {
    setRuntimePreviewOverlays(true);
  } else if (!timed_out_reconciliation && unresolved_preview_overlay_reconciliation_generation_ == 0) {
    resetPreviewOverlayReconciliationState();
  }
}

void HStreamWindow::setRuntimeVideoRendering(bool enabled) {
  updatePlaybackSeekControls();
  const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  if (!running) {
    appendLog(
        enabled ? "video rendering enabled for the next pipeline start"
                : "video rendering disabled for the next pipeline start");
    return;
  }
  setRuntimeRenderAudioMuted(!enabled);
  if (!pipeline_render_embedded_) {
    appendLog("runtime video rendering changes are unavailable for the active external display sink");
    return;
  }

  if (!enabled) {
    preview_disable_attempts_ = 0;
    if (preview_focus_mode_)
      setPreviewFocusMode(false, focused_preview_tab_ >= 0 ? focused_preview_tab_ : 0);
    setAllPreviewFocusAvailable(false);
    // Unmap native X11 children before splitters and tab contents move. The
    // backend acknowledgement may be delayed while SIGSTOP-paused, but a
    // frozen native child must never obscure the restored Qt layout.
    std::vector<QWidget*> surfaces = {preview_surface_, stitched_surface_};
    surfaces.insert(surfaces.end(), camera_preview_surfaces_.begin(), camera_preview_surfaces_.end());
    std::vector<QWidget*> targets = {preview_render_target_, stitched_render_target_};
    targets.insert(targets.end(), camera_preview_render_targets_.begin(), camera_preview_render_targets_.end());
    for (QWidget* target : targets) {
      if (target)
        target->hide();
    }
    for (QWidget* surface : surfaces) {
      if (surface)
        surface->hide();
    }
    for (QLabel* notice : {preview_external_notice_, stitched_external_notice_}) {
      if (notice) {
        notice->setText("Video rendering is disabled");
        notice->show();
      }
    }
    for (QLabel* notice : camera_preview_notices_) {
      if (notice) {
        notice->setText("Video rendering is disabled");
        notice->show();
      }
    }
    setPreviewRenderingLayout(false);
    if (preview_status_)
      preview_status_->setText("Disabling GPU preview…");
    if (preview_runtime_ready_) {
      if (!requestPipelinePreviewChannel("none", PreviewRequestReason::kRenderToggle))
        recoverPreviewDisableFailure("the render-off command could not be written", true);
    } else {
      appendLog("video rendering will be disabled when the GPU preview backend becomes ready");
    }
    return;
  }

  preview_disable_attempts_ = 0;
  setPreviewRenderingLayout(true);
  for (QLabel* notice : {preview_external_notice_, stitched_external_notice_}) {
    if (notice) {
      notice->setText("Starting GPU preview…");
      notice->show();
    }
  }
  for (QLabel* notice : camera_preview_notices_) {
    if (notice) {
      notice->setText("Starting GPU preview…");
      notice->show();
    }
  }
  if (preview_status_)
    preview_status_->setText("Enabling GPU preview…");
  if (preview_runtime_ready_) {
    const QString channel = selectedPipelinePreviewChannel();
    // An empty selected channel means the Pipeline inspector is active. Keep
    // every hidden video branch quiescent instead of silently waking Program.
    const QString requested_channel = channel.isEmpty() ? "none" : channel;
    if (!requestPipelinePreviewChannel(requested_channel, PreviewRequestReason::kRenderToggle))
      appendLog("could not enable the selected GPU preview display branch");
  } else {
    appendLog("video rendering will start when the GPU preview backend becomes ready");
  }
  // A pipeline started with Render disabled did not enable preview-only
  // metadata or sink overlays from the environment. Reapply the checked layers
  // whenever rendering becomes active so UI state and output agree.
  setRuntimePreviewOverlays();
  updatePlaybackSeekControls();
}

void HStreamWindow::setPreviewRenderingLayout(bool rendering) {
  if (rendering == preview_layout_compacted_)
    return;
  if (rendering) {
    if (main_log_splitter_)
      normal_main_log_sizes_ = main_log_splitter_->sizes();
    if (setup_preview_splitter_)
      normal_setup_preview_sizes_ = setup_preview_splitter_->sizes();
    normal_associated_controls_visible_.clear();
    for (size_t index = 0; index < associated_control_panels_.size(); ++index) {
      QWidget* panel = associated_control_panels_[index];
      normal_associated_controls_visible_.push_back(panel && !panel->isHidden());
      if (index < associated_control_toggles_.size() && associated_control_toggles_[index])
        associated_control_toggles_[index]->setChecked(false);
    }
    if (setup_preview_splitter_)
      setup_preview_splitter_->setSizes({0, std::max(1, setup_preview_splitter_->height())});
    if (main_log_splitter_) {
      const QList<int> sizes = main_log_splitter_->sizes();
      const int total = std::max(main_log_splitter_->height(), sizes.value(0) + sizes.value(1));
      const int compact_log_height = std::min(130, std::max(90, total / 6));
      main_log_splitter_->setSizes({std::max(1, total - compact_log_height), compact_log_height});
    }
    preview_layout_compacted_ = true;
    return;
  }

  if (main_log_splitter_ && normal_main_log_sizes_.size() == 2)
    main_log_splitter_->setSizes(normal_main_log_sizes_);
  if (setup_preview_splitter_ && normal_setup_preview_sizes_.size() == 2)
    setup_preview_splitter_->setSizes(normal_setup_preview_sizes_);
  for (size_t index = 0;
       index < normal_associated_controls_visible_.size() && index < associated_control_toggles_.size();
       ++index) {
    if (associated_control_toggles_[index])
      associated_control_toggles_[index]->setChecked(normal_associated_controls_visible_[index]);
  }
  normal_main_log_sizes_.clear();
  normal_setup_preview_sizes_.clear();
  normal_associated_controls_visible_.clear();
  preview_layout_compacted_ = false;
}

void HStreamWindow::setPreviewFocusAvailable(const QString& channel, bool available) {
  int tab_index = -1;
  if (channel == "program") {
    tab_index = 0;
  } else if (channel == "stitched") {
    tab_index = 1;
  } else if (channel.startsWith("source")) {
    bool valid = false;
    const int source_index = channel.mid(6).toInt(&valid);
    if (valid)
      tab_index = source_index + 2;
  }
  if (tab_index < 0 || tab_index >= static_cast<int>(preview_hosts_.size()))
    return;
  auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[tab_index]);
  if (host)
    host->setFocusAvailable(available);
}

void HStreamWindow::setAllPreviewFocusAvailable(bool available) {
  for (QWidget* widget : preview_hosts_) {
    auto* host = static_cast<LetterboxRenderHost*>(widget);
    if (host)
      host->setFocusAvailable(available);
  }
}

bool HStreamWindow::canFocusPreview(int tab_index) const {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning || !pipeline_render_embedded_ ||
      (render_video_toggle_ && !render_video_toggle_->isChecked()) || tab_index < 0 ||
      tab_index >= static_cast<int>(preview_hosts_.size())) {
    return false;
  }
  auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[tab_index]);
  const QString channel =
      hm::ui_internal::preview_channel_for_tab(tab_index, static_cast<int>(camera_preview_render_targets_.size()));
  QWidget* target = previewTargetForChannel(channel);
  return host && host->focusAvailable() && target && !target->isHidden() &&
      target->property("previewRendererState").toString() == "ready";
}

void HStreamWindow::togglePreviewFocus(int tab_index) {
  const bool restore = preview_focus_mode_ && focused_preview_tab_ == tab_index;
  if (!restore && !canFocusPreview(tab_index)) {
    appendLog(QString("preview focus ignored because tab %1 has no ready GPU frame").arg(tab_index));
    return;
  }
  setPreviewFocusMode(!restore, tab_index);
}

void HStreamWindow::setPreviewFocusMode(bool focused, int tab_index) {
  if (!preview_tabs_ || tab_index < 0 || tab_index >= preview_tabs_->count())
    return;
  if (focused && !canFocusPreview(tab_index))
    return;
  const QString transitioning_channel =
      hm::ui_internal::preview_channel_for_tab(tab_index, static_cast<int>(camera_preview_render_targets_.size()));
  QWidget* transitioning_target = previewTargetForChannel(transitioning_channel);
  auto* transitioning_host = tab_index < static_cast<int>(preview_hosts_.size())
      ? static_cast<LetterboxRenderHost*>(preview_hosts_[tab_index])
      : nullptr;
  const bool remap_target = transitioning_target && !transitioning_target->isHidden();
  const bool restore_focus_button = transitioning_host && transitioning_host->focusAvailable();
  // A mapped native X11 child punches through Qt's backing store. Unmap it
  // while the surrounding layouts change size so neither Qt nor the
  // compositor can retain pieces of its old geometry. The XID and renderer
  // remain intact and are remapped after the new layout has settled.
  if (remap_target)
    transitioning_target->hide();
  if (transitioning_host)
    transitioning_host->focusButton()->hide();
  preview_tabs_->setCurrentIndex(tab_index);
  preview_focus_mode_ = focused;
  focused_preview_tab_ = focused ? tab_index : -1;
  if (focused) {
    focus_hidden_widgets_.clear();
    auto hide_for_focus = [this](QWidget* widget) {
      if (!widget)
        return;
      if (!widget->isHidden())
        focus_hidden_widgets_.push_back(widget);
      widget->hide();
    };
    hide_for_focus(top_bar_);
    hide_for_focus(log_panel_);
    if (setup_panel_)
      hide_for_focus(setup_panel_->findChild<QWidget*>("setupControlsRow"));
    hide_for_focus(preview_tabs_->tabBar());
    for (int page_index = 0; page_index < preview_tabs_->count(); ++page_index) {
      QWidget* page = preview_tabs_->widget(page_index);
      QWidget* host = page_index < static_cast<int>(preview_hosts_.size()) ? preview_hosts_[page_index] : nullptr;
      for (QWidget* child : page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (child != host)
          hide_for_focus(child);
      }
    }
  } else {
    for (QWidget* widget : focus_hidden_widgets_) {
      if (widget)
        widget->show();
    }
    focus_hidden_widgets_.clear();
  }
  for (size_t index = 0; index < preview_hosts_.size(); ++index) {
    auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[index]);
    if (host)
      host->setFocused(focused && static_cast<int>(index) == tab_index);
  }
  if (centralWidget() && centralWidget()->layout()) {
    centralWidget()->layout()->invalidate();
    centralWidget()->layout()->activate();
  }
  QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  if (centralWidget()) {
    centralWidget()->repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
  if (remap_target) {
    transitioning_target->show();
    transitioning_target->raise();
  }
  if (transitioning_host && restore_focus_button) {
    transitioning_host->focusButton()->show();
    transitioning_host->focusButton()->raise();
  }
  appendLog(focused ? QString("preview focus mode tab=%1").arg(tab_index) : "preview restored to normal layout");
}

void HStreamWindow::updateRunControls() {
  const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  const bool finalizing = archive_finalize_process_ && archive_finalize_process_->state() != QProcess::NotRunning;
  if (!archive_finalize_blocked_source_path_.isEmpty() && !QFileInfo::exists(archive_finalize_blocked_source_path_)) {
    archive_finalize_blocked_source_path_.clear();
    releaseArchiveFinalizerOwnership(true);
  }
  const auto archive_toggle = output_toggles_.find("archive-file");
  const bool archive_enabled =
      archive_toggle != output_toggles_.end() && archive_toggle->second && archive_toggle->second->isChecked();
  const bool archive_recovery_blocked =
      !isCalibrationRun() && archive_enabled && !archive_finalize_blocked_source_path_.isEmpty();
  if (!pipeline_state_) {
    return;
  }
  if (!running && pipeline_state_->text().isEmpty()) {
    pipeline_state_->setText("STOPPED");
  }
  if (playback_progress_) {
    const bool terminal_result = playback_progress_state_ == PlaybackProgressState::kCompleted ||
        playback_progress_state_ == PlaybackProgressState::kError;
    playback_progress_->setVisible(running || terminal_result);
    updatePlaybackProgressPresentation();
  }
  if (start_button_) {
    start_button_->setEnabled(!running && !finalizing && !archive_recovery_blocked);
  }
  if (pause_button_) {
    pause_button_->setEnabled(
        running && pending_playback_seek_generation_ == 0 && playback_seek_recovery_generation_ == 0);
    pause_button_->setText(pipeline_paused_ ? "Resume" : "Pause");
  }
  if (stop_button_) {
    stop_button_->setEnabled(running);
  }
  if (run_mode_selector_) {
    run_mode_selector_->setEnabled(!running && !finalizing);
  }
  if (control_points_spin_) {
    control_points_spin_->setEnabled(!running && !finalizing);
  }
  if (calibration_frame_count_spin_) {
    calibration_frame_count_spin_->setEnabled(!running && !finalizing);
  }
  if (control_point_matcher_combo_) {
    control_point_matcher_combo_->setEnabled(!running && !finalizing);
  }
  if (mapping_backend_combo_) {
    mapping_backend_combo_->setEnabled(!running && !finalizing);
  }
  if (stitch_max_output_width_spin_) {
    stitch_max_output_width_spin_->setEnabled(!running && !finalizing);
  }
  if (stitch_frame_time_edit_) {
    stitch_frame_time_edit_->setEnabled(!running && !finalizing);
  }
  if (render_video_toggle_) {
    render_video_toggle_->setEnabled(!running || pipeline_render_embedded_);
  }
  if (drivegpt_csv_toggle_) {
    drivegpt_csv_toggle_->setEnabled(!running && !finalizing);
  }
  if (game_controls_) {
    game_controls_->setEnabled(!running && !finalizing);
  }
  if (video_controls_) {
    video_controls_->setEnabled(!running && !finalizing);
  }
  updatePlaybackSeekControls();
}

void HStreamWindow::restartStage() {
  appendLog("stage restart requested");
  calibration_restart_requested_ = isCalibrationRun();
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    stopPipeline();
  }
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("restart skipped because pipeline is still stopping");
    return;
  }
  startPipeline();
}

void HStreamWindow::savePreset() {
  if (!ensureGameDirectory()) {
    return;
  }
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock preset config: %1").arg(config_lock.status().ToString().c_str()));
    return;
  }
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not save preset: %1").arg(exc.what()));
      return;
    }
  }
  const QString game_dir = QString::fromStdString(config_path.parent_path().string());
  const QString previous_active_sidecar =
      resolve_ui_persistent_playtracker_config(config, game_dir, pipelineWorkingDirectory());

  bool invalidate_rink_masks = false;
  int invalidated_config_artifacts = 0;
  QString published_playtracker_sidecar;
  if (!applySavedControlConfig(
          config, &invalidate_rink_masks, &invalidated_config_artifacts, &published_playtracker_sidecar)) {
    if (!published_playtracker_sidecar.isEmpty()) {
      QFile::remove(published_playtracker_sidecar);
    }
    return;
  }
  const QString intended_active_sidecar =
      resolve_ui_persistent_playtracker_config(config, game_dir, pipelineWorkingDirectory());
  if (!previous_active_sidecar.isEmpty() && !same_file_path(previous_active_sidecar, intended_active_sidecar)) {
    const QString retirement_marker = playtracker_sidecar_retirement_marker(previous_active_sidecar);
    const absl::Status retirement_publish = qEnvironmentVariableIsSet("HSTREAM_UI_TEST_FAIL_PRESET_RETIREMENT_PUBLISH")
        ? absl::InternalError("sidecar retirement publication failure requested by test")
        : hm::stitching::publish_named_file(
              fs::path(retirement_marker.toStdString()), previous_active_sidecar.toStdString() + "\n");
    if (!retirement_publish.ok()) {
      if (!published_playtracker_sidecar.isEmpty()) {
        QFile::remove(published_playtracker_sidecar);
      }
      appendLog(QString("could not durably retire playtracker config %1: %2")
                    .arg(previous_active_sidecar, retirement_publish.ToString().c_str()));
      return;
    }
  }
  absl::Status publish;
  size_t invalidated_masks = 0;
  const bool fail_before_config_publish = qEnvironmentVariableIsSet("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_PUBLISH");
  const bool fail_after_config_publish = qEnvironmentVariableIsSet("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_POST_COMMIT");
  if (fail_before_config_publish) {
    publish = absl::InternalError("preset config publication failure requested by test");
  } else if (invalidate_rink_masks) {
    auto transaction =
        hm::stitching::publish_game_config_without_rink_masks(config_path.parent_path(), YAML::Dump(config) + "\n");
    if (transaction.ok()) {
      invalidated_masks = *transaction;
      publish = absl::OkStatus();
    } else {
      publish = transaction.status();
    }
  } else {
    publish = publish_yaml_config(config_path, config);
  }
  if (fail_after_config_publish && publish.ok()) {
    publish = absl::InternalError("post-commit preset config failure requested by test");
  }
  if (!publish.ok()) {
    bool published_sidecar_may_be_referenced = published_playtracker_sidecar.isEmpty();
    YAML::Node visible_config;
    bool visible_config_loaded = false;
    try {
      if (fs::is_regular_file(config_path)) {
        visible_config = YAML::LoadFile(config_path.string());
        visible_config_loaded = true;
      }
    } catch (const std::exception&) {
      // The transaction lock is already held. If the just-published config
      // cannot be inspected directly, conservatively retain its sidecar.
      published_sidecar_may_be_referenced = true;
    }
    if (visible_config_loaded && !published_playtracker_sidecar.isEmpty()) {
      YAML::Node configured_sidecar;
      if (lookup_yaml_path(visible_config, "pipeline.ds-playtracker.config-file", &configured_sidecar) &&
          configured_sidecar.IsScalar()) {
        const QString configured = QString::fromStdString(configured_sidecar.as<std::string>());
        for (const QString& candidate :
             playtracker_config_candidates(configured, game_dir, pipelineWorkingDirectory())) {
          if (same_file_path(candidate, published_playtracker_sidecar)) {
            published_sidecar_may_be_referenced = true;
            break;
          }
        }
      }
    }
    const bool visible_generation_matches = visible_config_loaded && YAML::Dump(visible_config) == YAML::Dump(config);
    if (!published_sidecar_may_be_referenced) {
      QFile::remove(published_playtracker_sidecar);
    }
    if (visible_generation_matches) {
      // The rename became visible but durability confirmation failed. Track
      // that visible generation as the baseline while keeping Save enabled
      // so the user can retry the durability operation.
      if (game_id_edit_ && !game_id_edit_->text().trimmed().isEmpty()) {
        preset_save_retry_game_ids_.insert(game_id_edit_->text().trimmed());
      }
      captureSavedControlState();
    }
    appendLog(QString("failed to write preset %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
    return;
  }
  const QString active_sidecar = resolve_ui_persistent_playtracker_config(config, game_dir, pipelineWorkingDirectory());
  const fs::path runtime_dir = config_path.parent_path() / ".hstream-ui";
  std::error_code cleanup_error;
  const auto stale_before = fs::file_time_type::clock::now() - std::chrono::hours(24);
  for (fs::directory_iterator it(runtime_dir, cleanup_error), end; !cleanup_error && it != end;
       it.increment(cleanup_error)) {
    const std::string filename = it->path().filename().string();
    if (filename.rfind("play_tracker_config_", 0) != 0 || it->path().extension() != ".yaml" ||
        same_file_path(QString::fromStdString(it->path().string()), active_sidecar)) {
      continue;
    }
    const fs::path retirement_marker =
        fs::path(playtracker_sidecar_retirement_marker(QString::fromStdString(it->path().string())).toStdString());
    if (!fs::is_regular_file(retirement_marker, cleanup_error) || cleanup_error) {
      cleanup_error.clear();
      continue;
    }
    const fs::file_time_type modified = fs::last_write_time(retirement_marker, cleanup_error);
    if (cleanup_error || modified > stale_before) {
      cleanup_error.clear();
      continue;
    }
    fs::remove(it->path(), cleanup_error);
    if (cleanup_error) {
      appendLog(QString("could not remove stale playtracker config %1: %2")
                    .arg(QString::fromStdString(it->path().string()), QString::fromStdString(cleanup_error.message())));
      cleanup_error.clear();
      continue;
    }
    fs::remove(retirement_marker, cleanup_error);
    cleanup_error.clear();
  }
  if (invalidate_rink_masks) {
    appendLog(QString("stitch rotation saved; invalidated %1 scoreboard/ice-mask artifact(s)")
                  .arg(invalidated_config_artifacts + static_cast<int>(invalidated_masks)));
  }
  appendLog(QString("preset saved %1").arg(QString::fromStdString(config_path.string())));
  if (game_id_edit_) {
    preset_save_retry_game_ids_.erase(game_id_edit_->text().trimmed());
  }
  captureSavedControlState();
}

void HStreamWindow::resetCameraControls() {
  const bool pipeline_running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  for (const auto& [id, value] : camera_defaults_) {
    const auto it = camera_sliders_.find(id);
    if (it != camera_sliders_.end()) {
      it->second->setValue(value);
      continue;
    }
    const auto checkbox = camera_checkboxes_.find(id);
    if (checkbox != camera_checkboxes_.end() && checkbox->second) {
      checkbox->second->setChecked(value != 0);
    }
  }
  if (!pipeline_running && stitch_frame_time_edit_) {
    const auto parsed = parse_stitch_frame_time(default_stitch_frame_time_);
    if (parsed.has_value())
      stitch_frame_time_edit_->setTime(*parsed);
  }
  if (!pipeline_running && control_point_matcher_combo_) {
    set_combo_to_data(control_point_matcher_combo_, default_control_point_matcher_);
  }
  if (!pipeline_running && mapping_backend_combo_) {
    set_combo_to_data(mapping_backend_combo_, default_mapping_backend_);
  }
  if (!pipeline_running && stitch_max_output_width_spin_) {
    stitch_max_output_width_spin_->setValue(default_stitch_max_output_width_);
  }
  if (pipeline_running) {
    // Reset every runtime-tunable field on both boxes, including a box that
    // was tuned before its target selector was returned to the default.
    const QStringList playtracker_reset_controls = {
        "Zoom_In_Aggressiveness",
        "Stop_Direction_Change_Delay_Frames",
        "Cancel_Stop_On_Opposite_Direction",
        "Stop_Cancel_Hysteresis_Frames",
        "Stop_Delay_Cooldown_Frames",
        "Time_To_Dest_Speed_Limit_Frames",
        "Overshoot_Stop_Delay_Frames",
        "Post_Nonstop_Stop_Delay_Frames",
        "Overshoot_Speed_Ratio_x100",
        "Max_Speed_X_x10",
        "Max_Speed_Y_x10",
        "Max_Accel_X_x10",
        "Max_Accel_Y_x10",
    };
    for (const QString& id : playtracker_reset_controls) {
      schedulePlaytrackerRuntimeControl(id, cameraControlValue(id));
    }
    scheduled_playtracker_force_all_targets_ = true;
  }
  appendLog("camera controls reset to defaults");
  updatePresetDirtyState();
}

void HStreamWindow::captureSavedControlState() {
  saved_camera_controls_.clear();
  for (const auto& [id, default_value] : camera_defaults_) {
    Q_UNUSED(default_value);
    saved_camera_controls_[id] = cameraControlValue(id);
  }
  saved_stitch_frame_time_ = stitchFrameTime();
  saved_stitching_control_points_ = stitchingCalibrationControlPoints();
  saved_stitching_calibration_frame_count_ = stitchingCalibrationFrameCount();
  saved_stitch_max_output_width_ = stitchingMaxOutputWidth();
  saved_control_point_matcher_ = controlPointMatcher();
  saved_mapping_backend_ = mappingBackend();
  updatePresetDirtyState();
}

void HStreamWindow::updatePresetDirtyState() {
  if (!save_preset_button_)
    return;
  const QString game_id = game_id_edit_ ? game_id_edit_->text().trimmed() : QString();
  const bool retry_required = !game_id.isEmpty() && preset_save_retry_game_ids_.count(game_id) != 0;
  bool dirty = retry_required || saved_camera_controls_.size() != camera_defaults_.size() ||
      saved_stitch_frame_time_ != stitchFrameTime() ||
      saved_stitching_control_points_ != stitchingCalibrationControlPoints() ||
      saved_stitching_calibration_frame_count_ != stitchingCalibrationFrameCount() ||
      saved_stitch_max_output_width_ != stitchingMaxOutputWidth() ||
      saved_control_point_matcher_ != controlPointMatcher() || saved_mapping_backend_ != mappingBackend();
  if (!dirty) {
    for (const auto& [id, default_value] : camera_defaults_) {
      Q_UNUSED(default_value);
      const auto saved = saved_camera_controls_.find(id);
      if (saved == saved_camera_controls_.end() || saved->second != cameraControlValue(id)) {
        dirty = true;
        break;
      }
    }
  }
  const bool has_game = !game_id.isEmpty();
  const bool can_save = dirty && has_game;
  if (save_preset_button_->isEnabled() != can_save)
    save_preset_button_->setEnabled(can_save);
  const QString description = !has_game ? "Select or create a game before saving camera-control changes."
      : retry_required                  ? "Retry saving the current preset because its last durability check failed."
      : dirty ? "Save the changed stitching and camera controls into this game's config.yaml for future runs."
              : "No stitching or camera-control changes need saving. Adjust a control to enable Save Preset.";
  if (save_preset_button_->toolTip() != description)
    set_control_help(save_preset_button_, description);
}

void HStreamWindow::loadSavedControlConfig() {
  if (!game_id_edit_ || game_id_edit_->text().isEmpty()) {
    captureSavedControlState();
    return;
  }
  if (control_points_spin_) {
    const bool blocked = control_points_spin_->blockSignals(true);
    control_points_spin_->setValue(kDefaultStitchCalibrationControlPoints);
    control_points_spin_->blockSignals(blocked);
  }
  if (calibration_frame_count_spin_) {
    const bool blocked = calibration_frame_count_spin_->blockSignals(true);
    calibration_frame_count_spin_->setValue(kDefaultStitchCalibrationFrameCount);
    calibration_frame_count_spin_->blockSignals(blocked);
  }
  if (control_point_matcher_combo_) {
    const bool blocked = control_point_matcher_combo_->blockSignals(true);
    set_combo_to_data(control_point_matcher_combo_, default_control_point_matcher_);
    control_point_matcher_combo_->blockSignals(blocked);
  }
  if (mapping_backend_combo_) {
    const bool blocked = mapping_backend_combo_->blockSignals(true);
    set_combo_to_data(mapping_backend_combo_, default_mapping_backend_);
    mapping_backend_combo_->blockSignals(blocked);
  }
  if (stitch_max_output_width_spin_) {
    const bool blocked = stitch_max_output_width_spin_->blockSignals(true);
    stitch_max_output_width_spin_->setValue(default_stitch_max_output_width_);
    stitch_max_output_width_spin_->blockSignals(blocked);
  }
  if (stitch_frame_time_edit_) {
    const bool blocked = stitch_frame_time_edit_->blockSignals(true);
    const QTime default_time = *parse_stitch_frame_time(default_stitch_frame_time_);
    stitch_frame_time_edit_->setTime(default_time);
    stitch_frame_time_edit_->setDisplayFormat(
        default_time.msec() == 0 ? kStitchFrameTimeFormat : kStitchFrameTimeFractionalFormat);
    stitch_frame_time_edit_->blockSignals(blocked);
  }
  for (const auto& [id, value] : camera_defaults_) {
    const auto slider_it = camera_sliders_.find(id);
    if (slider_it == camera_sliders_.end()) {
      const auto checkbox = camera_checkboxes_.find(id);
      if (checkbox != camera_checkboxes_.end() && checkbox->second) {
        const bool blocked = checkbox->second->blockSignals(true);
        checkbox->second->setChecked(value != 0);
        checkbox->second->blockSignals(blocked);
      }
      continue;
    }
    const bool blocked = slider_it->second->blockSignals(true);
    slider_it->second->setValue(value);
    slider_it->second->blockSignals(blocked);
    const auto label_it = camera_value_labels_.find(id);
    if (label_it != camera_value_labels_.end()) {
      label_it->second->setText(QString::number(value));
    }
  }

  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  auto loaded_config = hm::stitching::load_game_config_file(config_path);
  if (!loaded_config.ok()) {
    appendLog(QString("could not load saved controls: %1").arg(loaded_config.status().ToString().c_str()));
    saved_camera_controls_.clear();
    updatePresetDirtyState();
    return;
  }
  if (!loaded_config->has_value()) {
    captureSavedControlState();
    return;
  }
  try {
    YAML::Node config = **loaded_config;
    std::map<QString, int> staged_controls;
    for (const auto& [id, default_value] : camera_defaults_) {
      Q_UNUSED(default_value);
      staged_controls[id] = cameraControlValue(id);
    }
    auto stage_control = [this, &staged_controls](const QString& id, int value) {
      const auto slider = camera_sliders_.find(id);
      const auto checkbox = camera_checkboxes_.find(id);
      if ((slider == camera_sliders_.end() || !slider->second) &&
          (checkbox == camera_checkboxes_.end() || !checkbox->second)) {
        return false;
      }
      staged_controls[id] = value;
      return true;
    };
    auto rounded_control = [](const QString& path, double value) {
      if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) ||
          value > static_cast<double>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(
            QString("%1 is non-finite or outside the UI integer range").arg(path).toStdString());
      }
      return static_cast<int>(std::lround(value));
    };
    auto bounded_integer_control = [](const QString& path, const YAML::Node& value, int minimum, int maximum) {
      const double numeric_value = value.as<double>();
      if (!std::isfinite(numeric_value) || std::trunc(numeric_value) != numeric_value ||
          numeric_value < static_cast<double>(minimum) || numeric_value > static_cast<double>(maximum)) {
        throw std::invalid_argument(
            QString("%1 must be a whole number from %2 through %3").arg(path).arg(minimum).arg(maximum).toStdString());
      }
      return static_cast<int>(numeric_value);
    };
    auto stage_integer_path = [&config, &stage_control](const QString& path, const QString& id) {
      YAML::Node value;
      if (lookup_yaml_path(config, path, &value))
        stage_control(id, value.as<int>());
    };
    auto strict_boolean_control = [](const QString& path, const YAML::Node& value) {
      if (!value.IsScalar()) {
        throw std::invalid_argument(QString("%1 must be true, false, 1, or 0").arg(path).toStdString());
      }
      const QString normalized = QString::fromStdString(value.as<std::string>()).trimmed().toLower();
      if (normalized == "true" || normalized == "1") {
        return 1;
      }
      if (normalized == "false" || normalized == "0") {
        return 0;
      }
      throw std::invalid_argument(QString("%1 must be true, false, 1, or 0").arg(path).toStdString());
    };
    auto stage_boolean_path = [&config, &stage_control, &strict_boolean_control](
                                  const QString& path, const QString& id) {
      YAML::Node value;
      if (lookup_yaml_path(config, path, &value))
        stage_control(id, strict_boolean_control(path, value));
    };
    stage_integer_path("rink.camera.stop_on_dir_change_delay", "Stop_Direction_Change_Delay_Frames");
    stage_boolean_path("rink.camera.cancel_stop_on_opposite_dir", "Cancel_Stop_On_Opposite_Direction");
    stage_integer_path("rink.camera.stop_cancel_hysteresis_frames", "Stop_Cancel_Hysteresis_Frames");
    stage_integer_path("rink.camera.stop_delay_cooldown_frames", "Stop_Delay_Cooldown_Frames");
    stage_integer_path("rink.camera.time_to_dest_speed_limit_frames", "Time_To_Dest_Speed_Limit_Frames");
    YAML::Node zoom_in_aggressiveness;
    if (lookup_yaml_path(config, "rink.camera.zoom_in_aggressiveness", &zoom_in_aggressiveness)) {
      stage_control(
          "Zoom_In_Aggressiveness",
          bounded_integer_control("rink.camera.zoom_in_aggressiveness", zoom_in_aggressiveness, 0, 100));
    }
    stage_integer_path("rink.camera.breakaway_detection.overshoot_stop_delay_count", "Overshoot_Stop_Delay_Frames");
    stage_integer_path(
        "rink.camera.breakaway_detection.post_nonstop_stop_delay_count", "Post_Nonstop_Stop_Delay_Frames");
    YAML::Node overshoot_ratio;
    if (lookup_yaml_path(config, "rink.camera.breakaway_detection.overshoot_scale_speed_ratio", &overshoot_ratio)) {
      stage_control(
          "Overshoot_Speed_Ratio_x100",
          rounded_control(
              "rink.camera.breakaway_detection.overshoot_scale_speed_ratio", overshoot_ratio.as<double>() * 100.0));
    }
    YAML::Node stitch_rotation;
    if (lookup_yaml_path(config, "stitching.post_stitch_rotate_degrees", &stitch_rotation)) {
      if (stitch_rotation.IsNull()) {
        stage_control("Stitch_Rotate_Degrees", 90);
      } else {
        stage_control(
            "Stitch_Rotate_Degrees",
            rounded_control("stitching.post_stitch_rotate_degrees", 90.0 - stitch_rotation.as<double>()));
      }
    }
    int staged_control_points =
        control_points_spin_ ? control_points_spin_->value() : kDefaultStitchCalibrationControlPoints;
    int staged_frame_count =
        calibration_frame_count_spin_ ? calibration_frame_count_spin_->value() : kDefaultStitchCalibrationFrameCount;
    int staged_max_output_width = default_stitch_max_output_width_;
    QTime staged_stitch_frame_time = *parse_stitch_frame_time(default_stitch_frame_time_);
    QString staged_control_point_matcher = default_control_point_matcher_;
    QString staged_mapping_backend = default_mapping_backend_;
    YAML::Node control_points;
    if (control_points_spin_ &&
        lookup_yaml_path(config, "hstream_ui.stitching_calibration.control_points", &control_points) &&
        control_points.IsScalar()) {
      staged_control_points = control_points.as<int>();
    }
    YAML::Node frame_count;
    if (calibration_frame_count_spin_) {
      if (lookup_yaml_path(config, "hstream_ui.stitching_calibration.frame_count", &frame_count) &&
          frame_count.IsScalar()) {
        staged_frame_count = frame_count.as<int>();
      } else if (
          lookup_yaml_path(config, "stitching.calibration_frame_count", &frame_count) && frame_count.IsScalar()) {
        staged_frame_count = frame_count.as<int>();
      }
    }
    if (stitch_max_output_width_spin_) {
      staged_max_output_width = read_stitch_max_output_width_from_config(
          config, default_stitch_max_output_width_, stitch_max_output_width_spin_->maximum());
    }
    QString configured_stitch_frame_time;
    bool stitch_frame_time_present = false;
    if (stitch_frame_time_edit_ &&
        !read_stitch_frame_time(
            config, &configured_stitch_frame_time, &stitch_frame_time_present, default_stitch_frame_time_)) {
      throw std::invalid_argument("stitching.stitch_frame_time must be HH:MM:SS or HH:MM:SS.mmm");
    }
    if (stitch_frame_time_edit_ && stitch_frame_time_present) {
      const auto parsed = parse_stitch_frame_time(configured_stitch_frame_time);
      if (!parsed.has_value()) {
        throw std::invalid_argument("stitching.stitch_frame_time could not be normalized");
      }
      staged_stitch_frame_time = *parsed;
    }
    QString previous_control_point_matcher;
    QString previous_mapping_backend;
    const bool generated_backend_choices = generated_stitching_backend_choices_match_private(
        config, &previous_control_point_matcher, &previous_mapping_backend);
    if (generated_backend_choices) {
      if (!previous_control_point_matcher.isEmpty()) {
        staged_control_point_matcher = previous_control_point_matcher;
      }
      if (!previous_mapping_backend.isEmpty()) {
        staged_mapping_backend = previous_mapping_backend;
      }
    } else {
      YAML::Node control_point_matcher;
      if (lookup_yaml_path(config, "stitching.control_point_matcher", &control_point_matcher) &&
          control_point_matcher.IsScalar()) {
        const QString configured = QString::fromStdString(control_point_matcher.as<std::string>());
        const auto canonical = canonical_control_point_matcher_choice(configured);
        if (canonical.has_value()) {
          staged_control_point_matcher = *canonical;
        } else {
          appendLog(QString("ignored unsupported stitching.control_point_matcher=%1").arg(configured));
        }
      }
      YAML::Node mapping_backend;
      if (lookup_yaml_path(config, "stitching.mapping_backend", &mapping_backend) && mapping_backend.IsScalar()) {
        const QString configured = QString::fromStdString(mapping_backend.as<std::string>());
        const auto canonical = canonical_mapping_backend_choice(configured);
        if (canonical.has_value()) {
          staged_mapping_backend = *canonical;
        } else {
          appendLog(QString("ignored unsupported stitching.mapping_backend=%1").arg(configured));
        }
      }
    }
    YAML::Node fixed_edge_rotation;
    if (lookup_yaml_path(config, "rink.camera.fixed_edge_rotation_angle", &fixed_edge_rotation)) {
      auto angle_x10 = [&rounded_control](const YAML::Node& value, const QString& path) {
        return rounded_control(path, value.as<double>() * 10.0);
      };
      if (fixed_edge_rotation.IsNull()) {
        stage_control("Link_Fixed_Edge_Rotation_Left_Right", 1);
        stage_control("Left_Fixed_Edge_Rotation_Angle_x10", 0);
        stage_control("Right_Fixed_Edge_Rotation_Angle_x10", 0);
      } else if (fixed_edge_rotation.IsSequence() && fixed_edge_rotation.size() == 2) {
        stage_control("Link_Fixed_Edge_Rotation_Left_Right", 0);
        stage_control(
            "Left_Fixed_Edge_Rotation_Angle_x10",
            angle_x10(fixed_edge_rotation[0], "rink.camera.fixed_edge_rotation_angle[0]"));
        stage_control(
            "Right_Fixed_Edge_Rotation_Angle_x10",
            angle_x10(fixed_edge_rotation[1], "rink.camera.fixed_edge_rotation_angle[1]"));
      } else if (fixed_edge_rotation.IsScalar()) {
        const int value = angle_x10(fixed_edge_rotation, "rink.camera.fixed_edge_rotation_angle");
        stage_control("Link_Fixed_Edge_Rotation_Left_Right", 1);
        stage_control("Left_Fixed_Edge_Rotation_Angle_x10", value);
        stage_control("Right_Fixed_Edge_Rotation_Angle_x10", value);
      } else {
        appendLog("ignored invalid rink.camera.fixed_edge_rotation_angle; expected null, one value, or [left, right]");
      }
    }
    stage_boolean_path("pipeline.hmstitcher.properties.high-bit-depth", "Use_10_Bit_Grading");
    const QString tone_property_owner = staged_controls["Use_10_Bit_Grading"] != 0
        ? QStringLiteral("pipeline.hmstitcher.properties")
        : QStringLiteral("pipeline.hmplaycropper.properties");
    const QString shadow_lift_path = tone_property_owner + ".shadow-lift";
    YAML::Node shadow_lift;
    if (lookup_yaml_path(config, shadow_lift_path, &shadow_lift)) {
      stage_control("Bring_Up_Shadows", bounded_integer_control(shadow_lift_path, shadow_lift, 0, 100));
    }
    stage_boolean_path(tone_property_owner + ".shadow-lift-black-point", "Lift_Shadow_Black_Point");
    const QString exposure_path = tone_property_owner + ".exposure";
    YAML::Node exposure;
    if (lookup_yaml_path(config, exposure_path, &exposure)) {
      const double setting = exposure.as<double>();
      const int setting_x100 = rounded_control(exposure_path, setting * 100.0);
      if (!std::isfinite(setting) || setting < 0.0 || setting > 1.3 ||
          std::abs(setting * 100.0 - setting_x100) > 1e-6) {
        throw std::invalid_argument(
            QString("%1 must be from 0.00 through 1.30 in hundredths").arg(exposure_path).toStdString());
      }
      stage_control("Exposure_x100", setting_x100);
    }
    YAML::Node controls = config["hstream_ui"]["camera_controls"];
    int loaded = 0;
    if (controls && controls.IsMap()) {
      for (const auto& entry : controls) {
        const QString id = QString::fromStdString(entry.first.as<std::string>());
        int value = entry.second.as<int>();
        if (id == "Exposure_x100") {
          value = bounded_integer_control("hstream_ui.camera_controls." + id, entry.second, 0, 130);
        } else if (id == "Bring_Up_Shadows" || id == "Zoom_In_Aggressiveness") {
          value = bounded_integer_control("hstream_ui.camera_controls." + id, entry.second, 0, 100);
        }
        if ((id == "Link_Fixed_Edge_Rotation_Left_Right" || id == "Apply_To_Fast_Box" ||
             id == "Apply_To_Follower_Box" || id == "Lift_Shadow_Black_Point" || id == "Use_10_Bit_Grading") &&
            value != 0 && value != 1) {
          throw std::invalid_argument(QString("%1 must be 0 or 1").arg(id).toStdString());
        }
        if ((camera_sliders_.find(id) != camera_sliders_.end() ||
             camera_checkboxes_.find(id) != camera_checkboxes_.end()) &&
            stage_control(id, value)) {
          ++loaded;
        }
      }
    }
    if (staged_controls["Link_Fixed_Edge_Rotation_Left_Right"] != 0) {
      staged_controls["Right_Fixed_Edge_Rotation_Angle_x10"] = staged_controls["Left_Fixed_Edge_Rotation_Angle_x10"];
    }
    if (control_points_spin_) {
      const bool blocked = control_points_spin_->blockSignals(true);
      control_points_spin_->setValue(staged_control_points);
      control_points_spin_->blockSignals(blocked);
    }
    if (calibration_frame_count_spin_) {
      const bool blocked = calibration_frame_count_spin_->blockSignals(true);
      calibration_frame_count_spin_->setValue(staged_frame_count);
      calibration_frame_count_spin_->blockSignals(blocked);
    }
    if (stitch_frame_time_edit_) {
      const bool blocked = stitch_frame_time_edit_->blockSignals(true);
      stitch_frame_time_edit_->setTime(staged_stitch_frame_time);
      stitch_frame_time_edit_->setDisplayFormat(
          staged_stitch_frame_time.msec() == 0 ? kStitchFrameTimeFormat : kStitchFrameTimeFractionalFormat);
      stitch_frame_time_edit_->blockSignals(blocked);
    }
    if (control_point_matcher_combo_) {
      const bool blocked = control_point_matcher_combo_->blockSignals(true);
      set_combo_to_data(control_point_matcher_combo_, staged_control_point_matcher);
      control_point_matcher_combo_->blockSignals(blocked);
    }
    if (mapping_backend_combo_) {
      const bool blocked = mapping_backend_combo_->blockSignals(true);
      set_combo_to_data(mapping_backend_combo_, staged_mapping_backend);
      mapping_backend_combo_->blockSignals(blocked);
    }
    if (stitch_max_output_width_spin_) {
      const bool blocked = stitch_max_output_width_spin_->blockSignals(true);
      stitch_max_output_width_spin_->setValue(staged_max_output_width);
      stitch_max_output_width_spin_->blockSignals(blocked);
    }
    for (const auto& [id, value] : staged_controls) {
      const auto slider_it = camera_sliders_.find(id);
      if (slider_it == camera_sliders_.end() || !slider_it->second) {
        const auto checkbox = camera_checkboxes_.find(id);
        if (checkbox != camera_checkboxes_.end() && checkbox->second) {
          const bool blocked = checkbox->second->blockSignals(true);
          checkbox->second->setChecked(value != 0);
          checkbox->second->blockSignals(blocked);
        }
        continue;
      }
      const bool blocked = slider_it->second->blockSignals(true);
      slider_it->second->setRange(
          std::min(slider_it->second->minimum(), value), std::max(slider_it->second->maximum(), value));
      slider_it->second->setValue(value);
      slider_it->second->blockSignals(blocked);
      const auto label_it = camera_value_labels_.find(id);
      if (label_it != camera_value_labels_.end()) {
        label_it->second->setText(QString::number(slider_it->second->value()));
      }
    }
    appendLog(QString("loaded %1 saved camera controls").arg(loaded));
    captureSavedControlState();
  } catch (const std::exception& exc) {
    appendLog(QString("could not load saved camera controls: %1").arg(exc.what()));
    saved_camera_controls_.clear();
    updatePresetDirtyState();
  }
}

bool HStreamWindow::applySavedControlConfig(
    YAML::Node& config,
    bool* invalidate_rink_masks,
    int* invalidated_config_artifacts,
    QString* published_playtracker_sidecar) {
  if (invalidate_rink_masks) {
    *invalidate_rink_masks = false;
  }
  if (invalidated_config_artifacts) {
    *invalidated_config_artifacts = 0;
  }
  if (published_playtracker_sidecar) {
    published_playtracker_sidecar->clear();
  }
  if (!yaml_defined(config) || config.IsNull()) {
    config = YAML::Node(YAML::NodeType::Map);
  }
  YAML::Node previous_hstream_ui = map_value(config, "hstream_ui");
  YAML::Node previous_generated = map_value(previous_hstream_ui, "generated_runtime_keys");
  YAML::Node previous_generated_values = map_value(previous_hstream_ui, "generated_runtime_values");
  YAML::Node previous_playtracker_config_base = map_value(previous_hstream_ui, "playtracker_config_base");
  YAML::Node previous_stitch_rotation;
  const bool previous_stitch_rotation_found =
      lookup_yaml_path(config, "stitching.post_stitch_rotate_degrees", &previous_stitch_rotation);
  const bool previous_stitch_rotation_was_null = previous_stitch_rotation_found && previous_stitch_rotation.IsNull();
  YAML::Node previous_fixed_edge_rotation;
  const bool previous_fixed_edge_rotation_was_null =
      lookup_yaml_path(config, "rink.camera.fixed_edge_rotation_angle", &previous_fixed_edge_rotation) &&
      previous_fixed_edge_rotation.IsNull();
  if (yaml_defined(previous_generated) && previous_generated.IsSequence()) {
    for (const auto& item : previous_generated) {
      const QString path = QString::fromStdString(item.as<std::string>());
      YAML::Node current;
      const bool current_found = lookup_yaml_path(config, path, &current);
      const std::string previous_key = path.toStdString();
      YAML::Node previous_value;
      const bool previous_found = lookup_yaml_key(previous_generated_values, previous_key.c_str(), &previous_value);
      bool generated_value_matches =
          previous_found && current_found && YAML::Dump(current) == previous_value.as<std::string>();
      if (!generated_value_matches && path == "pipeline.ds-playtracker.config-file" && current_found &&
          current.IsScalar() && previous_found && previous_value.IsScalar() && game_id_edit_) {
        const QString game_dir = gameDirectory(game_id_edit_->text());
        const QString current_path = QString::fromStdString(current.as<std::string>());
        const QString previous_path = QString::fromStdString(previous_value.as<std::string>());
        const QStringList current_candidates = {
            current_path,
            QDir(game_dir).filePath(current_path),
            QDir(pipelineWorkingDirectory()).filePath(current_path),
            QDir(QDir(pipelineWorkingDirectory()).filePath("configs")).filePath(current_path),
        };
        for (const QString& current_candidate : current_candidates) {
          if (same_file_path(current_candidate, previous_path)) {
            generated_value_matches = true;
            break;
          }
        }
      }
      if (!previous_found || !current_found || generated_value_matches) {
        remove_yaml_path(config, path);
      }
    }
  }
  remove_yaml_path(config, {"hstream_ui", "generated_runtime_keys"});
  remove_yaml_path(config, {"hstream_ui", "generated_runtime_values"});
  remove_yaml_path(config, {"hstream_ui", "playtracker_config_base"});
  remove_yaml_path(config, {"hstream_ui", "generated_stitching_backend_choices"});
  YAML::Node current_playtracker_config;
  if (previous_playtracker_config_base && previous_playtracker_config_base.IsScalar() &&
      !lookup_yaml_path(config, "pipeline.ds-playtracker.config-file", &current_playtracker_config)) {
    config["pipeline"]["ds-playtracker"]["config-file"] = previous_playtracker_config_base.as<std::string>();
  }

  int previous_max_output_width = std::numeric_limits<int>::min();
  try {
    previous_max_output_width = read_stitch_max_output_width_from_config(
        config,
        default_stitch_max_output_width_,
        stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->maximum() : std::numeric_limits<int>::max());
  } catch (const std::exception& ex) {
    qWarning() << "Ignoring malformed existing stitch max output width while saving preset:" << ex.what();
  }
  QString previous_stitch_frame_time = default_stitch_frame_time_;
  const bool previous_stitch_frame_time_valid =
      read_stitch_frame_time(config, &previous_stitch_frame_time, nullptr, default_stitch_frame_time_);
  const int selected_max_output_width = stitchingMaxOutputWidth();
  const bool had_conflicting_max_output_width_native_alias = has_conflicting_stitch_max_output_width_native_alias(
      config,
      selected_max_output_width,
      stitch_max_output_width_spin_ ? stitch_max_output_width_spin_->maximum() : std::numeric_limits<int>::max());

  // Every canonical key represented by a UI control is owned by the current
  // slider state on Save, including keys written directly by an operator.
  // Remove the old values first, then serialize only non-default controls.
  for (const char* path : {
           "stitching.post_stitch_rotate_degrees",
           "stitching.control_point_matcher",
           "stitching.mapping_backend",
           "stitching.max_output_width",
           "pipeline.hmplaycropper.properties.shadow-lift",
           "pipeline.hmplaycropper.properties.shadow-lift-black-point",
           "pipeline.hmplaycropper.properties.exposure",
           "pipeline.hmstitcher.properties.high-bit-depth",
           "pipeline.hmstitcher.properties.shadow-lift",
           "pipeline.hmstitcher.properties.shadow-lift-black-point",
           "pipeline.hmstitcher.properties.exposure",
           "rink.camera.fixed_edge_rotation_angle",
           "rink.camera.stop_on_dir_change_delay",
           "rink.camera.cancel_stop_on_opposite_dir",
           "rink.camera.stop_cancel_hysteresis_frames",
           "rink.camera.stop_delay_cooldown_frames",
           "rink.camera.time_to_dest_speed_limit_frames",
           "rink.camera.zoom_in_aggressiveness",
           "rink.camera.breakaway_detection.overshoot_stop_delay_count",
           "rink.camera.breakaway_detection.post_nonstop_stop_delay_count",
           "rink.camera.breakaway_detection.overshoot_scale_speed_ratio",
           "hstream_ui.camera_control_targets.apply_to_fast_box",
           "hstream_ui.camera_control_targets.apply_to_follower_box",
       }) {
    remove_yaml_path(config, QString::fromLatin1(path));
  }
  remove_stitch_max_output_width_native_aliases(config);
  YAML::Node generated_runtime_keys(YAML::NodeType::Sequence);
  YAML::Node generated_runtime_values(YAML::NodeType::Map);
  std::set<std::string> generated_key_set;
  auto mark_runtime_key = [&](const QString& path) {
    const std::string key = path.toStdString();
    if (generated_key_set.insert(key).second) {
      generated_runtime_keys.push_back(key);
    }
  };

  YAML::Node controls(YAML::NodeType::Map);
  int changed = 0;
  for (const auto& [id, default_value] : camera_defaults_) {
    const int value = cameraControlValue(id);
    if (value == default_value) {
      continue;
    }
    const std::string key = id.toStdString();
    controls[key.c_str()] = value;
    ++changed;
  }
  config["hstream_ui"]["camera_controls"] = controls;

  const QString stitch_frame_time = stitchFrameTime();
  const int selected_control_points = stitchingCalibrationControlPoints();
  const int selected_frame_count = stitchingCalibrationFrameCount();
  const bool stitch_frame_time_changed =
      !previous_stitch_frame_time_valid || previous_stitch_frame_time != stitch_frame_time;
  const bool control_points_changed =
      saved_stitching_control_points_ != 0 && saved_stitching_control_points_ != selected_control_points;
  const bool frame_count_changed =
      saved_stitching_calibration_frame_count_ != 0 && saved_stitching_calibration_frame_count_ != selected_frame_count;
  const bool max_output_width_changed =
      previous_max_output_width != selected_max_output_width || had_conflicting_max_output_width_native_alias;
  const QString selected_control_point_matcher = controlPointMatcher();
  const QString selected_mapping_backend = mappingBackend();
  const QString previous_control_point_matcher =
      saved_control_point_matcher_.isEmpty() ? default_control_point_matcher_ : saved_control_point_matcher_;
  const QString previous_mapping_backend =
      saved_mapping_backend_.isEmpty() ? default_mapping_backend_ : saved_mapping_backend_;
  const bool control_point_matcher_changed = previous_control_point_matcher != selected_control_point_matcher;
  const bool mapping_backend_changed = previous_mapping_backend != selected_mapping_backend;
  remove_yaml_path(config, {"stitching", "stitch_frame_time"});
  remove_yaml_path(config, {"stitching", "calibration_frame_count"});
  if (stitch_frame_time != default_stitch_frame_time_) {
    config["stitching"]["stitch_frame_time"] = stitch_frame_time.toStdString();
  }
  if (selected_frame_count != kDefaultStitchCalibrationFrameCount) {
    config["stitching"]["calibration_frame_count"] = selected_frame_count;
  }
  config["stitching"]["control_point_matcher"] = selected_control_point_matcher.toStdString();
  config["stitching"]["mapping_backend"] = selected_mapping_backend.toStdString();
  config["stitching"]["max_output_width"] =
      selected_max_output_width > 0 ? YAML::Node(selected_max_output_width) : YAML::Node(YAML::NodeType::Null);
  if (stitch_frame_time_changed || control_points_changed || frame_count_changed || control_point_matcher_changed ||
      mapping_backend_changed || max_output_width_changed) {
    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = selected_control_points;
    calibration["frame_count"] = selected_frame_count;
    calibration["status"] = "pending";
    calibration["rink_mask_status"] = "pending";
    calibration["stale_from"] = stitch_frame_time_changed || frame_count_changed
        ? "input"
        : ((control_points_changed || control_point_matcher_changed) ? "features" : "canvas");
    calibration["artifacts_invalidated"] = false;
    calibration["invalidation_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    appendLog("stitching calibration settings changed; stitching calibration marked stale");
  }

  auto slider_value = [this](const QString& id) -> int { return cameraControlValue(id); };
  const auto saved_stitch_rotation = saved_camera_controls_.find("Stitch_Rotate_Degrees");
  const auto stitch_rotation_slider = camera_sliders_.find("Stitch_Rotate_Degrees");
  const bool preserve_stitch_rotation_null = previous_stitch_rotation_was_null &&
      saved_stitch_rotation != saved_camera_controls_.end() && stitch_rotation_slider != camera_sliders_.end() &&
      stitch_rotation_slider->second && stitch_rotation_slider->second->value() == saved_stitch_rotation->second;
  if (preserve_stitch_rotation_null) {
    config["stitching"]["post_stitch_rotate_degrees"] = YAML::Node(YAML::NodeType::Null);
  } else if (has_control(controls, "Stitch_Rotate_Degrees")) {
    config["stitching"]["post_stitch_rotate_degrees"] = 90 - slider_value("Stitch_Rotate_Degrees");
    mark_runtime_key("stitching.post_stitch_rotate_degrees");
  }
  const bool use_high_bit_grading = slider_value("Use_10_Bit_Grading") != 0;
  const char* tone_element = use_high_bit_grading ? "hmstitcher" : "hmplaycropper";
  const QString tone_path_prefix = QString("pipeline.%1.properties").arg(tone_element);
  if (use_high_bit_grading) {
    config["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = true;
    mark_runtime_key("pipeline.hmstitcher.properties.high-bit-depth");
  }
  if (has_control(controls, "Bring_Up_Shadows")) {
    config["pipeline"][tone_element]["properties"]["shadow-lift"] = slider_value("Bring_Up_Shadows");
    mark_runtime_key(tone_path_prefix + ".shadow-lift");
  }
  if (has_control(controls, "Lift_Shadow_Black_Point")) {
    config["pipeline"][tone_element]["properties"]["shadow-lift-black-point"] =
        slider_value("Lift_Shadow_Black_Point") != 0;
    mark_runtime_key(tone_path_prefix + ".shadow-lift-black-point");
  }
  if (has_control(controls, "Exposure_x100")) {
    config["pipeline"][tone_element]["properties"]["exposure"] = slider_value("Exposure_x100") / 100.0;
    mark_runtime_key(tone_path_prefix + ".exposure");
  }
  const bool fixed_edge_rotation_changed = has_control(controls, "Link_Fixed_Edge_Rotation_Left_Right") ||
      has_control(controls, "Left_Fixed_Edge_Rotation_Angle_x10") ||
      has_control(controls, "Right_Fixed_Edge_Rotation_Angle_x10");
  auto fixed_edge_control_matches_saved = [this](const QString& id) {
    const auto slider = camera_sliders_.find(id);
    const auto saved = saved_camera_controls_.find(id);
    return slider != camera_sliders_.end() && slider->second && saved != saved_camera_controls_.end() &&
        slider->second->value() == saved->second;
  };
  const bool preserve_fixed_edge_null = previous_fixed_edge_rotation_was_null &&
      fixed_edge_control_matches_saved("Link_Fixed_Edge_Rotation_Left_Right") &&
      fixed_edge_control_matches_saved("Left_Fixed_Edge_Rotation_Angle_x10") &&
      fixed_edge_control_matches_saved("Right_Fixed_Edge_Rotation_Angle_x10");
  if (preserve_fixed_edge_null) {
    config["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Node(YAML::NodeType::Null);
  } else if (fixed_edge_rotation_changed) {
    const double left_angle = slider_value("Left_Fixed_Edge_Rotation_Angle_x10") / 10.0;
    const double right_angle = slider_value("Right_Fixed_Edge_Rotation_Angle_x10") / 10.0;
    if (slider_value("Link_Fixed_Edge_Rotation_Left_Right") != 0) {
      config["rink"]["camera"]["fixed_edge_rotation_angle"] = left_angle;
    } else {
      YAML::Node angles(YAML::NodeType::Sequence);
      angles.push_back(left_angle);
      angles.push_back(right_angle);
      config["rink"]["camera"]["fixed_edge_rotation_angle"] = angles;
    }
    mark_runtime_key("rink.camera.fixed_edge_rotation_angle");
  }
  YAML::Node current_stitch_rotation;
  const bool current_stitch_rotation_found =
      lookup_yaml_path(config, "stitching.post_stitch_rotate_degrees", &current_stitch_rotation);
  const bool rotation_changed_for_artifacts = previous_stitch_rotation_found != current_stitch_rotation_found ||
      (previous_stitch_rotation_found && current_stitch_rotation_found &&
       YAML::Dump(previous_stitch_rotation) != YAML::Dump(current_stitch_rotation));
  if (rotation_changed_for_artifacts) {
    const ArtifactInvalidationResult invalidation = invalidate_rotation_dependent_artifacts(config);
    config["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "pending";
    if (invalidate_rink_masks) {
      *invalidate_rink_masks = true;
    }
    if (invalidated_config_artifacts) {
      *invalidated_config_artifacts = invalidation.invalidated;
    }
  }
  if (has_control(controls, "Stop_Direction_Change_Delay_Frames")) {
    config["rink"]["camera"]["stop_on_dir_change_delay"] = slider_value("Stop_Direction_Change_Delay_Frames");
    mark_runtime_key("rink.camera.stop_on_dir_change_delay");
  }
  if (has_control(controls, "Cancel_Stop_On_Opposite_Direction")) {
    config["rink"]["camera"]["cancel_stop_on_opposite_dir"] = slider_value("Cancel_Stop_On_Opposite_Direction") != 0;
    mark_runtime_key("rink.camera.cancel_stop_on_opposite_dir");
  }
  if (has_control(controls, "Stop_Cancel_Hysteresis_Frames")) {
    config["rink"]["camera"]["stop_cancel_hysteresis_frames"] = slider_value("Stop_Cancel_Hysteresis_Frames");
    mark_runtime_key("rink.camera.stop_cancel_hysteresis_frames");
  }
  if (has_control(controls, "Stop_Delay_Cooldown_Frames")) {
    config["rink"]["camera"]["stop_delay_cooldown_frames"] = slider_value("Stop_Delay_Cooldown_Frames");
    mark_runtime_key("rink.camera.stop_delay_cooldown_frames");
  }
  if (has_control(controls, "Overshoot_Stop_Delay_Frames")) {
    config["rink"]["camera"]["breakaway_detection"]["overshoot_stop_delay_count"] =
        slider_value("Overshoot_Stop_Delay_Frames");
    mark_runtime_key("rink.camera.breakaway_detection.overshoot_stop_delay_count");
  }
  if (has_control(controls, "Post_Nonstop_Stop_Delay_Frames")) {
    config["rink"]["camera"]["breakaway_detection"]["post_nonstop_stop_delay_count"] =
        slider_value("Post_Nonstop_Stop_Delay_Frames");
    mark_runtime_key("rink.camera.breakaway_detection.post_nonstop_stop_delay_count");
  }
  if (has_control(controls, "Overshoot_Speed_Ratio_x100")) {
    config["rink"]["camera"]["breakaway_detection"]["overshoot_scale_speed_ratio"] =
        ratio_x100(slider_value("Overshoot_Speed_Ratio_x100"));
    mark_runtime_key("rink.camera.breakaway_detection.overshoot_scale_speed_ratio");
  }
  if (has_control(controls, "Time_To_Dest_Speed_Limit_Frames")) {
    config["rink"]["camera"]["time_to_dest_speed_limit_frames"] = slider_value("Time_To_Dest_Speed_Limit_Frames");
    mark_runtime_key("rink.camera.time_to_dest_speed_limit_frames");
  }
  if (has_control(controls, "Zoom_In_Aggressiveness")) {
    config["rink"]["camera"]["zoom_in_aggressiveness"] = slider_value("Zoom_In_Aggressiveness");
    mark_runtime_key("rink.camera.zoom_in_aggressiveness");
  }
  const bool has_playtracker_runtime_controls = has_control(controls, "Stop_Direction_Change_Delay_Frames") ||
      has_control(controls, "Zoom_In_Aggressiveness") || has_control(controls, "Cancel_Stop_On_Opposite_Direction") ||
      has_control(controls, "Stop_Cancel_Hysteresis_Frames") || has_control(controls, "Stop_Delay_Cooldown_Frames") ||
      has_control(controls, "Time_To_Dest_Speed_Limit_Frames") ||
      has_control(controls, "Overshoot_Stop_Delay_Frames") || has_control(controls, "Post_Nonstop_Stop_Delay_Frames") ||
      has_control(controls, "Overshoot_Speed_Ratio_x100") || has_control(controls, "Max_Speed_X_x10") ||
      has_control(controls, "Max_Speed_Y_x10") || has_control(controls, "Max_Accel_X_x10") ||
      has_control(controls, "Max_Accel_Y_x10");
  if (has_playtracker_runtime_controls && game_id_edit_) {
    const QString game_dir = gameDirectory(game_id_edit_->text());
    QDir runtime_dir(QDir(game_dir).filePath(".hstream-ui"));
    if (!runtime_dir.exists() && !runtime_dir.mkpath(".")) {
      appendLog(QString("could not create playtracker runtime config directory %1").arg(runtime_dir.path()));
      return false;
    } else {
      const QString runtime_config_path = runtime_dir.filePath(
          QString("play_tracker_config_%1.yaml").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
      try {
        QString base_playtracker_config = pipelineConfigPath("play_tracker_config.yaml");
        QString configured_playtracker_config;
        YAML::Node configured_config_file;
        if (lookup_yaml_path(config, "pipeline.ds-playtracker.config-file", &configured_config_file) &&
            configured_config_file.IsScalar()) {
          QString configured = QString::fromStdString(configured_config_file.as<std::string>());
          const QString working_dir = pipelineWorkingDirectory();
          const QStringList candidates = playtracker_config_candidates(configured, game_dir, working_dir);
          for (const QString& candidate : candidates) {
            if (same_file_path(candidate, runtime_config_path) ||
                is_ui_persistent_playtracker_config(candidate, game_dir)) {
              if (previous_playtracker_config_base && previous_playtracker_config_base.IsScalar()) {
                configured = QString::fromStdString(previous_playtracker_config_base.as<std::string>());
                const QStringList base_candidates = playtracker_config_candidates(configured, game_dir, working_dir);
                for (const QString& base_candidate : base_candidates) {
                  if (QFileInfo::exists(base_candidate)) {
                    base_playtracker_config = base_candidate;
                    configured_playtracker_config = configured;
                    break;
                  }
                }
              }
              break;
            }
            if (QFileInfo::exists(candidate)) {
              base_playtracker_config = candidate;
              configured_playtracker_config = configured;
              break;
            }
          }
        }
        YAML::Node play_tracker_config = QFileInfo::exists(base_playtracker_config)
            ? YAML::LoadFile(base_playtracker_config.toStdString())
            : YAML::Node(YAML::NodeType::Map);
        if (!play_tracker_config["play-tracker"] || !play_tracker_config["play-tracker"].IsMap()) {
          play_tracker_config["play-tracker"] = YAML::Node(YAML::NodeType::Map);
        }
        if (!play_tracker_config["play-tracker"]["live-boxes"] ||
            !play_tracker_config["play-tracker"]["live-boxes"].IsSequence()) {
          play_tracker_config["play-tracker"]["live-boxes"] = YAML::Node(YAML::NodeType::Sequence);
        }
        YAML::Node live_boxes = play_tracker_config["play-tracker"]["live-boxes"];
        if (live_boxes.size() == 0) {
          for (const char* name : {"current_roi", "current_roi_aspect"}) {
            YAML::Node box(YAML::NodeType::Map);
            box["name"] = name;
            live_boxes.push_back(box);
          }
        }
        const auto live_box_roles = hm::resolve_playtracker_live_box_roles(live_boxes);
        if (!live_box_roles.ok())
          throw std::invalid_argument(live_box_roles.status().ToString());

        YAML::Node play_tracker = play_tracker_config["play-tracker"];
        if (has_control(controls, "Zoom_In_Aggressiveness")) {
          play_tracker["zoom-in-aggressiveness"] = slider_value("Zoom_In_Aggressiveness");
        }
        if (has_control(controls, "Overshoot_Stop_Delay_Frames")) {
          play_tracker["overshoot-stop-delay-count"] = slider_value("Overshoot_Stop_Delay_Frames");
        }
        if (has_control(controls, "Overshoot_Speed_Ratio_x100")) {
          play_tracker["overshoot-scale-speed-ratio"] = ratio_x100(slider_value("Overshoot_Speed_Ratio_x100"));
        }

        auto apply_live_box = [&](size_t index) {
          if (has_control(controls, "Stop_Direction_Change_Delay_Frames")) {
            live_boxes[index]["stop-translation-on-dir-change-delay"] =
                slider_value("Stop_Direction_Change_Delay_Frames");
          }
          if (has_control(controls, "Cancel_Stop_On_Opposite_Direction")) {
            live_boxes[index]["cancel-stop-on-opposite-dir"] = slider_value("Cancel_Stop_On_Opposite_Direction") != 0;
          }
          if (has_control(controls, "Stop_Cancel_Hysteresis_Frames")) {
            live_boxes[index]["cancel-stop-hysteresis-frames"] = slider_value("Stop_Cancel_Hysteresis_Frames");
          }
          if (has_control(controls, "Stop_Delay_Cooldown_Frames")) {
            live_boxes[index]["stop-delay-cooldown-frames"] = slider_value("Stop_Delay_Cooldown_Frames");
          }
          if (has_control(controls, "Time_To_Dest_Speed_Limit_Frames")) {
            live_boxes[index]["time-to-dest-speed-limit-frames"] = slider_value("Time_To_Dest_Speed_Limit_Frames");
          }
          if (has_control(controls, "Post_Nonstop_Stop_Delay_Frames")) {
            live_boxes[index]["post-nonstop-stop-delay-count"] = slider_value("Post_Nonstop_Stop_Delay_Frames");
          }
          if (has_control(controls, "Max_Speed_X_x10")) {
            live_boxes[index]["max-speed-x"] = static_cast<double>(slider_value("Max_Speed_X_x10")) / 10.0;
          }
          if (has_control(controls, "Max_Speed_Y_x10")) {
            live_boxes[index]["max-speed-y"] = static_cast<double>(slider_value("Max_Speed_Y_x10")) / 10.0;
          }
          if (has_control(controls, "Max_Accel_X_x10")) {
            live_boxes[index]["max-accel-x"] = static_cast<double>(slider_value("Max_Accel_X_x10")) / 10.0;
          }
          if (has_control(controls, "Max_Accel_Y_x10")) {
            live_boxes[index]["max-accel-y"] = static_cast<double>(slider_value("Max_Accel_Y_x10")) / 10.0;
          }
        };
        if (slider_value("Apply_To_Fast_Box") != 0) {
          apply_live_box(live_box_roles->fast_index);
        }
        if (slider_value("Apply_To_Follower_Box") != 0) {
          apply_live_box(live_box_roles->follower_index);
        }

        const absl::Status runtime_publish = hm::stitching::publish_named_file(
            fs::path(runtime_config_path.toStdString()), YAML::Dump(play_tracker_config) + "\n");
        if (!runtime_publish.ok()) {
          // The atomic rename can succeed before a later directory fsync
          // reports failure. This UUID generation is not referenced by the
          // still-locked game config, so it is safe to remove here.
          QFile::remove(runtime_config_path);
          appendLog(QString("could not atomically write playtracker runtime config %1: %2")
                        .arg(runtime_config_path, runtime_publish.ToString().c_str()));
          return false;
        }
        if (published_playtracker_sidecar) {
          *published_playtracker_sidecar = QFileInfo(runtime_config_path).absoluteFilePath();
        }
        config["pipeline"]["ds-playtracker"]["config-file"] = runtime_config_path.toStdString();
        if (!configured_playtracker_config.isEmpty() && configured_playtracker_config != runtime_config_path) {
          config["hstream_ui"]["playtracker_config_base"] = configured_playtracker_config.toStdString();
        }
        mark_runtime_key("pipeline.ds-playtracker.config-file");
        appendLog(QString("playtracker runtime config saved %1").arg(runtime_config_path));
      } catch (const std::exception& exc) {
        appendLog(QString("could not save playtracker runtime config: %1").arg(exc.what()));
        return false;
      }
    }
  }
  if (has_control(controls, "Apply_To_Fast_Box")) {
    config["hstream_ui"]["camera_control_targets"]["apply_to_fast_box"] = slider_value("Apply_To_Fast_Box") != 0;
    mark_runtime_key("hstream_ui.camera_control_targets.apply_to_fast_box");
  }
  if (has_control(controls, "Apply_To_Follower_Box")) {
    config["hstream_ui"]["camera_control_targets"]["apply_to_follower_box"] =
        slider_value("Apply_To_Follower_Box") != 0;
    mark_runtime_key("hstream_ui.camera_control_targets.apply_to_follower_box");
  }
  if (generated_runtime_keys.size() > 0) {
    for (const auto& path_node : generated_runtime_keys) {
      const std::string key = path_node.as<std::string>();
      YAML::Node value;
      if (lookup_yaml_path(config, QString::fromStdString(key), &value)) {
        generated_runtime_values[key.c_str()] = YAML::Dump(value);
      }
    }
    config["hstream_ui"]["generated_runtime_keys"] = generated_runtime_keys;
    config["hstream_ui"]["generated_runtime_values"] = generated_runtime_values;
  }
  appendLog(QString("preset captured %1 non-default camera controls").arg(changed));
  return true;
}

void HStreamWindow::refreshGames() {
  if (!game_selector_) {
    return;
  }
  const QString current = game_id_edit_ ? game_id_edit_->text() : QString();
  const bool blocked = game_selector_->blockSignals(true);
  game_selector_->clear();

  QDir root(gameRoot());
  if (!root.exists()) {
    root.mkpath(".");
  }
  const QFileInfoList dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& dir : dirs) {
    game_selector_->addItem(dir.fileName());
  }
  game_selector_->blockSignals(blocked);

  if (!current.isEmpty()) {
    const int index = game_selector_->findText(current);
    if (index >= 0) {
      game_selector_->setCurrentIndex(index);
    }
  } else if (game_selector_->count() > 0) {
    selectGame(game_selector_->currentText());
  } else if (game_path_label_) {
    game_path_label_->setText(gameRoot());
    if (video_sets_path_label_) {
      video_sets_path_label_->setText(gameRoot());
    }
  }
}

void HStreamWindow::selectGame(const QString& game_id) {
  if (!game_id_edit_) {
    return;
  }
  game_id_edit_->setText(sanitized_game_id(game_id));
  if (game_path_label_) {
    game_path_label_->setText(gameDirectory(game_id_edit_->text()));
  }
  if (video_sets_path_label_) {
    video_sets_path_label_->setText(gameDirectory(game_id_edit_->text()));
  }
  refreshVideoSets();
  loadSavedControlConfig();
  appendLog(QString("game selected %1").arg(game_id_edit_->text()));
}

void HStreamWindow::createOrLoadGame() {
  if (!ensureGameDirectory()) {
    return;
  }
  refreshGames();
  refreshVideoSets();
  loadSavedControlConfig();
  appendLog(QString("game ready %1").arg(game_id_edit_->text()));
}

void HStreamWindow::addVideoPath() {
  if (!video_path_edit_) {
    return;
  }
  const QString role = selectedVideoRole();
  if (!ensureGameDirectory()) {
    return;
  }
  const fs::path game_dir_path(gameDirectory(game_id_edit_->text()).toStdString());
  auto config_transaction = hm::stitching::GameConfigTransactionLock::Acquire(game_dir_path);
  if (!config_transaction.ok()) {
    appendLog(
        QString("could not lock video import transaction: %1").arg(config_transaction.status().ToString().c_str()));
    return;
  }

  QString imported_relative_path;
  bool imported_path_created = false;
  if (!importVideoPath(video_path_edit_->text(), &imported_relative_path, &imported_path_created)) {
    return;
  }

  if (const char* delay = std::getenv("HM_TEST_VIDEO_ADD_PRE_CONFIG_SAVE_DELAY_MS")) {
    const long delay_ms = std::strtol(delay, nullptr, 10);
    if (delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  QByteArray original_config;
  QByteArray published_config;
  bool had_config = false;
  if (!savePrivateConfigForRole(role, imported_relative_path, &original_config, &had_config, &published_config)) {
    if (imported_path_created) {
      rollbackImportedVideoPath(imported_relative_path);
    }
    config_transaction->reset();
    refreshVideoSets();
    return;
  }
  if (role == "auto") {
    const CopiedImportCleanupResult cleanup =
        removeClearedCopiedExplicitImports(original_config, had_config, true, published_config);
    if (cleanup == CopiedImportCleanupResult::kRolledBack) {
      if (imported_path_created) {
        rollbackImportedVideoPath(imported_relative_path);
      }
      config_transaction->reset();
      refreshVideoSets();
      return;
    }
    if (cleanup == CopiedImportCleanupResult::kCommittedWithCleanupFailure) {
      appendLog("video set added, but one or more unreferenced copied imports could not be cleaned");
    }
  }
  config_transaction->reset();
  refreshVideoSets();
  appendLog(QString("video set added role=%1 path=%2").arg(role_label(role), imported_relative_path));
}

void HStreamWindow::browseVideoPath() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  const QString path = QFileDialog::getOpenFileName(
      this,
      "Add Video",
      start_dir.isEmpty() ? QDir::homePath() : start_dir,
      "Videos (*.mp4 *.MP4 *.mkv *.MKV *.mov *.MOV *.avi *.AVI)");
  if (!path.isEmpty() && video_path_edit_) {
    video_path_edit_->setText(path);
  }
}

void HStreamWindow::removeSelectedVideoSet() {
  if (!video_set_list_) {
    return;
  }
  const int row = video_set_list_->currentRow();
  if (row < 0) {
    appendLog("select a video set before removing");
    return;
  }
  auto* item = video_set_list_->takeItem(row);
  if (!item) {
    return;
  }
  const QString role = item->data(Qt::UserRole).toString();
  const QString relative_path = item->data(Qt::UserRole + 1).toString();
  const QString config_file = QDir(gameDirectory(game_id_edit_->text())).filePath("config.yaml");
  QByteArray original_config;
  bool had_config = false;
  bool copied_import = false;
  if (const char* delay = std::getenv("HM_TEST_VIDEO_REMOVE_PRE_TRANSACTION_DELAY_MS")) {
    const long delay_ms = std::strtol(delay, nullptr, 10);
    if (delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }

  auto config_transaction =
      hm::stitching::GameConfigTransactionLock::Acquire(fs::path(gameDirectory(game_id_edit_->text()).toStdString()));
  if (!config_transaction.ok()) {
    appendLog(
        QString("could not lock video removal transaction: %1").arg(config_transaction.status().ToString().c_str()));
    video_set_list_->insertItem(row, item);
    return;
  }

  QByteArray removed_config;
  if (!removePrivateConfigForRole(
          role, relative_path, &original_config, &had_config, &copied_import, &removed_config)) {
    video_set_list_->insertItem(row, item);
    return;
  }
  if (const char* delay = std::getenv("HM_TEST_VIDEO_REMOVE_POST_TRANSACTION_DELAY_MS")) {
    const long delay_ms = std::strtol(delay, nullptr, 10);
    if (delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  if (!removeImportedVideoPath(relative_path, copied_import)) {
    if (!restorePrivateConfigAfterRemoveFailure(original_config, had_config, removed_config)) {
      appendLog(QString("failed to restore private config after remove failure %1").arg(config_file));
    }
    video_set_list_->insertItem(row, item);
    config_transaction->reset();
    refreshVideoSets();
    return;
  }
  if (role == "auto" &&
      removeClearedCopiedExplicitImports(original_config, had_config, false) != CopiedImportCleanupResult::kSuccess) {
    appendLog("video set removed, but one or more unreferenced copied imports could not be cleaned");
  }

  appendLog(QString("video set removed role=%1 path=%2").arg(role_label(role), relative_path));
  delete item;
  config_transaction->reset();
  refreshVideoSets();
}

void HStreamWindow::refreshVideoSets() {
  if (!video_set_list_ || !game_id_edit_) {
    return;
  }
  video_set_list_->clear();
  const QString dir = gameDirectory(game_id_edit_->text());
  if (game_path_label_) {
    game_path_label_->setText(dir);
  }
  if (video_sets_path_label_) {
    video_sets_path_label_->setText(dir);
  }
  if (game_id_edit_->text().isEmpty() || !QDir(dir).exists()) {
    return;
  }

  std::set<QString> seen;
  std::set<QString> configured_paths;
  auto add_item = [&](const QString& role, const QString& path) {
    const QString key = role + "\n" + path;
    if (seen.count(key)) {
      return;
    }
    seen.insert(key);
    auto* item = new QListWidgetItem(QString("%1  %2").arg(role_label(role), path));
    item->setData(Qt::UserRole, role);
    item->setData(Qt::UserRole + 1, path);
    video_set_list_->addItem(item);
  };

  QDir game_dir(dir);
  const QFileInfoList dirs = game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  std::vector<QFileInfo> cam_dirs;
  const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
  for (const QFileInfo& dir_info : dirs) {
    if (cam_pattern.match(dir_info.fileName()).hasMatch() && !dir_info.isSymLink()) {
      cam_dirs.push_back(dir_info);
    }
  }
  std::sort(cam_dirs.begin(), cam_dirs.end(), [](const QFileInfo& a, const QFileInfo& b) {
    const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
    return cam_pattern.match(a.fileName()).captured(1).toInt() < cam_pattern.match(b.fileName()).captured(1).toInt();
  });

  bool listed_cam_video = false;
  for (const QFileInfo& cam_dir : cam_dirs) {
    const QFileInfoList files =
        QDir(cam_dir.filePath()).entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      if (is_auto_chapter_file(file.fileName())) {
        listed_cam_video = true;
        break;
      }
    }
    if (listed_cam_video) {
      break;
    }
  }

  const fs::path config_path = fs::path(dir.toStdString()) / "config.yaml";
  auto loaded_config = hm::stitching::load_game_config_file(config_path);
  if (!loaded_config.ok()) {
    appendLog(QString("could not list configured videos: %1").arg(loaded_config.status().ToString().c_str()));
  } else if (loaded_config->has_value()) {
    try {
      YAML::Node config = **loaded_config;
      YAML::Node explicit_roles = config["hstream_ui"]["video_roles"];
      for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
        YAML::Node role_videos = explicit_roles[role.toStdString()];
        if (role_videos && role_videos.IsSequence()) {
          for (const auto& item : role_videos) {
            const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
            configured_paths.insert(path);
            add_item(role, path);
          }
        }
      }
      YAML::Node videos = config["game"]["videos"];
      for (const QString& role : {QString("left"), QString("right")}) {
        YAML::Node role_videos = videos[role.toStdString()];
        if (role_videos && role_videos.IsSequence()) {
          for (const auto& item : role_videos) {
            const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
            if (!configured_paths.count(path) && (!listed_cam_video || is_cam_relative_path(path))) {
              add_item("auto", path);
            }
          }
        }
      }
      YAML::Node copied_imports = config["hstream_ui"]["copied_imports"];
      if (copied_imports && copied_imports.IsSequence()) {
        for (const auto& item : copied_imports) {
          if (!item.IsScalar())
            continue;
          const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
          if (configured_paths.count(path) || !QFileInfo::exists(game_dir.filePath(path)))
            continue;
          for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
            if (path.startsWith(QString(".hstream-ui/%1/").arg(role))) {
              configured_paths.insert(path);
              add_item(role, path);
              break;
            }
          }
        }
      }
    } catch (const std::exception& exc) {
      appendLog(QString("could not read private config: %1").arg(exc.what()));
    }
  }

  for (const QFileInfo& cam_dir : cam_dirs) {
    const QFileInfoList files =
        QDir(cam_dir.filePath()).entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      const QString relative_path = game_dir.relativeFilePath(file.filePath());
      if (is_auto_chapter_file(file.fileName())) {
        if (!configured_paths.count(relative_path)) {
          add_item("auto", relative_path);
        }
      }
    }
  }

  if (!listed_cam_video) {
    const QFileInfoList files = game_dir.entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      const QString relative_path = game_dir.relativeFilePath(file.filePath());
      if (is_root_auto_file(file.fileName()) && !configured_paths.count(relative_path)) {
        add_item("auto", relative_path);
      }
    }
  }
}

QString HStreamWindow::selectedVideoRole() const {
  if (role_left_ && role_left_->isChecked()) {
    return "left";
  }
  if (role_center_ && role_center_->isChecked()) {
    return "center";
  }
  if (role_right_ && role_right_->isChecked()) {
    return "right";
  }
  return "auto";
}

QString HStreamWindow::gameRoot() const {
  const QByteArray env = qgetenv("HM_GAME_DIR");
  if (!env.isEmpty()) {
    return QString::fromLocal8Bit(env);
  }
  auto user_overlay = hm::user_config::load_or_create();
  if (user_overlay.ok()) {
    auto configured = hm::user_config::game_root(*user_overlay);
    if (configured.ok())
      return QString::fromStdString(configured->string());
  }
  return QDir::home().filePath("Videos");
}

QString HStreamWindow::gameDirectory(const QString& game_id) const {
  if (game_id.isEmpty()) {
    return gameRoot();
  }
  return QDir(gameRoot()).filePath(game_id);
}

QString HStreamWindow::relativeToGameDir(const QString& path) const {
  const QDir dir(gameDirectory(game_id_edit_->text()));
  return dir.relativeFilePath(path);
}

bool HStreamWindow::ensureGameDirectory() {
  if (!game_id_edit_) {
    return false;
  }
  const QString game_id = sanitized_game_id(game_id_edit_->text());
  game_id_edit_->setText(game_id);
  if (game_id.isEmpty()) {
    appendLog("game id is required");
    return false;
  }
  QDir root(gameRoot());
  if (!root.exists() && !root.mkpath(".")) {
    appendLog(QString("failed to create game root %1").arg(root.path()));
    return false;
  }
  if (!root.exists(game_id) && !root.mkdir(game_id)) {
    appendLog(QString("failed to create game %1").arg(game_id));
    return false;
  }
  if (game_path_label_) {
    game_path_label_->setText(gameDirectory(game_id));
  }
  return true;
}

bool HStreamWindow::importVideoPath(const QString& source_path, QString* imported_relative_path, bool* created) {
  if (!imported_relative_path || !created) {
    return false;
  }
  *created = false;
  if (!ensureGameDirectory()) {
    return false;
  }
  const QFileInfo source(source_path);
  if (!source.exists() || !source.isFile()) {
    appendLog(QString("video file not found %1").arg(source_path));
    return false;
  }
  if (!is_video_file(source.fileName())) {
    appendLog(QString("unsupported video extension %1").arg(source.fileName()));
    return false;
  }
  const QString role = selectedVideoRole();
  if (role == "auto" && !is_auto_chapter_file(source.fileName())) {
    appendLog(QString("auto video file name is not discoverable by the pipeline %1").arg(source.fileName()));
    return false;
  }

  const QString game_dir = gameDirectory(game_id_edit_->text());
  QDir target_dir(game_dir);
  int max_cam_index = 0;
  if (role == "auto") {
    const QFileInfoList dirs = target_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
    for (const QFileInfo& dir : dirs) {
      const QRegularExpressionMatch match = cam_pattern.match(dir.fileName());
      if (match.hasMatch()) {
        max_cam_index = std::max(max_cam_index, match.captured(1).toInt());
      }
    }
    QString cam_dir = existing_auto_cam_dir_for_source(target_dir, source);
    if (cam_dir.isEmpty()) {
      cam_dir = QString("cam%1").arg(max_cam_index + 1);
    }
    if (!target_dir.exists(cam_dir) && !target_dir.mkdir(cam_dir)) {
      appendLog(QString("failed to create video set directory %1").arg(cam_dir));
      return false;
    }
    if (QFileInfo(target_dir.filePath(cam_dir)).isSymLink()) {
      appendLog(QString("refusing to import through symlinked video set directory %1").arg(cam_dir));
      return false;
    }
    target_dir.cd(cam_dir);
  } else if (is_explicit_role(role)) {
    const QString ui_dir = ".hstream-ui";
    const QString role_dir = ui_dir + "/" + role;
    if (!target_dir.exists(ui_dir) && !target_dir.mkdir(ui_dir)) {
      appendLog(QString("failed to create UI metadata directory %1").arg(ui_dir));
      return false;
    }
    if (QFileInfo(target_dir.filePath(ui_dir)).isSymLink()) {
      appendLog(QString("refusing to import through symlinked UI metadata directory %1").arg(ui_dir));
      return false;
    }
    if (!target_dir.exists(role_dir) && !target_dir.mkpath(role_dir)) {
      appendLog(QString("failed to create %1 video directory %2").arg(role_label(role), role_dir));
      return false;
    }
    if (QFileInfo(target_dir.filePath(role_dir)).isSymLink()) {
      appendLog(QString("refusing to import through symlinked %1 video directory %2").arg(role_label(role), role_dir));
      return false;
    }
    target_dir.cd(role_dir);
  }
  QString dest_name = source.fileName();
  QString dest_path = target_dir.filePath(dest_name);
  int suffix = 2;
  const QString auto_group_family = role == "auto" ? auto_file_family(source.fileName()) : QString();
  const QString source_parent = role == "auto" ? canonical_dir_path(source.absolutePath()) : QString();
  while (QFileInfo::exists(dest_path) && QFileInfo(dest_path).canonicalFilePath() != source.canonicalFilePath() &&
         role == "auto") {
    const QString relative_path = QDir(game_dir).relativeFilePath(dest_path);
    if (copied_auto_import_matches(QDir(game_dir), relative_path, auto_group_family, source_parent)) {
      *imported_relative_path = relative_path;
      return true;
    }
    target_dir = QDir(game_dir);
    const QString cam_dir = QString("cam%1").arg(++max_cam_index);
    if (!target_dir.exists(cam_dir) && !target_dir.mkdir(cam_dir)) {
      appendLog(QString("failed to create video set directory %1").arg(cam_dir));
      return false;
    }
    target_dir.cd(cam_dir);
    dest_name = source.fileName();
    dest_path = target_dir.filePath(dest_name);
  }
  while (QFileInfo::exists(dest_path) && QFileInfo(dest_path).canonicalFilePath() != source.canonicalFilePath() &&
         role != "auto") {
    dest_name = QString("%1-%2.%3").arg(source.completeBaseName()).arg(suffix++).arg(source.suffix());
    dest_path = target_dir.filePath(dest_name);
  }

  if (!QFileInfo::exists(dest_path)) {
    try {
      if (std::getenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY") != nullptr) {
        throw std::runtime_error("injected symlink failure");
      }
      fs::create_symlink(fs::path(source.absoluteFilePath().toStdString()), fs::path(dest_path.toStdString()));
    } catch (const std::exception& exc) {
      if (!QFile::copy(source.absoluteFilePath(), dest_path)) {
        appendLog(QString("failed to import video link or copy: %1").arg(exc.what()));
        return false;
      }
      if (!saveCopiedImport(relativeToGameDir(dest_path), auto_group_family, source_parent)) {
        QFile::remove(dest_path);
        return false;
      }
      appendLog(QString("video symlink unavailable; copied import to %1").arg(dest_name));
    }
    *created = true;
  }

  *imported_relative_path = relativeToGameDir(dest_path);
  return true;
}

bool HStreamWindow::saveCopiedImport(
    const QString& relative_path,
    const QString& auto_group_family,
    const QString& source_parent) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not update copied import metadata: %1").arg(exc.what()));
      return false;
    }
  }

  YAML::Node list = config["hstream_ui"]["copied_imports"];
  if (!list || !list.IsSequence()) {
    config["hstream_ui"]["copied_imports"] = YAML::Node(YAML::NodeType::Sequence);
    list = config["hstream_ui"]["copied_imports"];
  }
  bool copied_exists = false;
  for (const auto& item : list) {
    if (QString::fromStdString(item.as<std::string>()) == relative_path) {
      copied_exists = true;
      break;
    }
  }
  if (!copied_exists) {
    list.push_back(relative_path.toStdString());
  }

  if (!auto_group_family.isEmpty() && !source_parent.isEmpty()) {
    YAML::Node sources = config["hstream_ui"]["auto_import_sources"];
    if (!sources || !sources.IsSequence()) {
      config["hstream_ui"]["auto_import_sources"] = YAML::Node(YAML::NodeType::Sequence);
      sources = config["hstream_ui"]["auto_import_sources"];
    }
    bool source_exists = false;
    for (const auto& item : sources) {
      if (item["path"] && QString::fromStdString(item["path"].as<std::string>()) == relative_path) {
        source_exists = true;
        break;
      }
    }
    if (!source_exists) {
      YAML::Node entry(YAML::NodeType::Map);
      entry["path"] = relative_path.toStdString();
      entry["family"] = auto_group_family.toStdString();
      entry["source_parent"] = source_parent.toStdString();
      sources.push_back(entry);
    }
  }

  const auto publish = publish_yaml_config(config_path, config);
  if (!publish.ok())
    appendLog(QString("failed to write copied import metadata %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
  return publish.ok();
}

bool HStreamWindow::rollbackImportedVideoPath(const QString& relative_path) {
  const QDir game_dir(gameDirectory(game_id_edit_->text()));
  const fs::path config_path = fs::path(game_dir.absolutePath().toStdString()) / "config.yaml";
  bool copied_import = false;
  YAML::Node config(YAML::NodeType::Map);
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
      copied_import = is_copied_import_in_config(config, game_dir, relative_path);
    } catch (const std::exception& exc) {
      appendLog(QString("could not read copied import metadata during rollback: %1").arg(exc.what()));
      return false;
    }
  }

  if (config_references_video_path(config, game_dir, relative_path)) {
    appendLog(QString("imported video was adopted while rollback was pending; preserving %1").arg(relative_path));
    return true;
  }
  const YAML::Node original_config = YAML::Clone(config);

  QString imported_path;
  if (!resolveImportedVideoPath(relative_path, copied_import, &imported_path))
    return false;
  if (const char* fail_path = std::getenv("HM_TEST_VIDEO_REMOVE_FAIL");
      !imported_path.isEmpty() && fail_path != nullptr && relative_path == QString::fromUtf8(fail_path)) {
    appendLog(QString("injected failure removing imported video %1").arg(relative_path));
    return false;
  }

  QString staged_path;
  if (!imported_path.isEmpty()) {
    const QFileInfo imported(imported_path);
    staged_path =
        QDir(imported.absolutePath())
            .filePath(QString(".hstream-rollback-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QFile::rename(imported_path, staged_path)) {
      appendLog(QString("failed to stage imported video rollback %1").arg(relative_path));
      return false;
    }
  }

  if (copied_import) {
    auto matches_path = [&](const QString& value) {
      return normalized_config_video_path(game_dir, value) == normalized_config_video_path(game_dir, relative_path);
    };
    YAML::Node copied_imports = config["hstream_ui"]["copied_imports"];
    YAML::Node copied_replacement(YAML::NodeType::Sequence);
    if (copied_imports && copied_imports.IsSequence()) {
      for (const auto& item : copied_imports) {
        const QString path = QString::fromStdString(item.as<std::string>());
        if (!matches_path(path))
          copied_replacement.push_back(item.as<std::string>());
      }
    }
    config["hstream_ui"]["copied_imports"] = copied_replacement;

    YAML::Node sources = config["hstream_ui"]["auto_import_sources"];
    if (sources && sources.IsSequence()) {
      YAML::Node source_replacement(YAML::NodeType::Sequence);
      for (const auto& item : sources) {
        if (!item["path"] || !matches_path(QString::fromStdString(item["path"].as<std::string>())))
          source_replacement.push_back(item);
      }
      config["hstream_ui"]["auto_import_sources"] = source_replacement;
    }

    const auto publish = publish_yaml_config(config_path, config);
    if (!publish.ok()) {
      if (!staged_path.isEmpty() && !QFile::rename(staged_path, imported_path))
        appendLog(QString("failed to restore staged imported video %1").arg(relative_path));
      appendLog(QString("failed to remove copied import metadata during rollback: %1").arg(publish.ToString().c_str()));
      return false;
    }
  }

  const char* staged_remove_fail_path = std::getenv("HM_TEST_VIDEO_STAGED_REMOVE_FAIL");
  const bool injected_staged_remove_failure =
      staged_remove_fail_path != nullptr && relative_path == QString::fromUtf8(staged_remove_fail_path);
  if (!staged_path.isEmpty() && (injected_staged_remove_failure || !QFile::remove(staged_path))) {
    if (injected_staged_remove_failure)
      appendLog(QString("injected failure removing staged imported video %1").arg(relative_path));
    if (!QFile::rename(staged_path, imported_path))
      appendLog(QString("failed to restore staged imported video %1 after deletion failure").arg(relative_path));
    if (copied_import) {
      const auto restore_metadata = publish_yaml_config(config_path, original_config);
      if (!restore_metadata.ok()) {
        appendLog(QString("failed to restore copied import metadata after deletion failure: %1")
                      .arg(restore_metadata.ToString().c_str()));
      }
    }
    appendLog(QString("failed to remove staged imported video rollback %1").arg(relative_path));
    return false;
  }
  return true;
}

HStreamWindow::CopiedImportCleanupResult HStreamWindow::removeClearedCopiedExplicitImports(
    const QByteArray& original_config,
    bool had_config,
    bool restore_auto_selection_on_failure,
    const QByteArray& published_auto_config) {
  if (!had_config || original_config.isEmpty()) {
    return CopiedImportCleanupResult::kSuccess;
  }
  YAML::Node old_config;
  YAML::Node current_config;
  YAML::Node auto_config;
  const QString config_file = QDir(gameDirectory(game_id_edit_->text())).filePath("config.yaml");
  const fs::path config_path(config_file.toStdString());
  try {
    old_config = YAML::Load(original_config.toStdString());
    current_config = YAML::LoadFile(config_file.toStdString());
    if (!published_auto_config.isEmpty())
      auto_config = YAML::Load(published_auto_config.toStdString());
  } catch (const std::exception&) {
    return CopiedImportCleanupResult::kCommittedWithCleanupFailure;
  }

  std::set<QString> current_references;
  const QDir game_dir(gameDirectory(game_id_edit_->text()));
  auto collect_current = [&](YAML::Node list) {
    if (!list || !list.IsSequence()) {
      return;
    }
    for (const auto& item : list) {
      current_references.insert(normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>())));
    }
  };
  YAML::Node current_roles = current_config["hstream_ui"]["video_roles"];
  for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
    collect_current(current_roles[role.toStdString()]);
  }

  std::set<QString> cleanup_paths;
  YAML::Node old_roles = old_config["hstream_ui"]["video_roles"];
  for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
    YAML::Node role_videos = old_roles[role.toStdString()];
    if (!role_videos || !role_videos.IsSequence()) {
      continue;
    }
    for (const auto& item : role_videos) {
      const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
      if (!current_references.count(path) && is_copied_import_in_config(current_config, game_dir, path)) {
        cleanup_paths.insert(path);
      }
    }
  }

  auto restore_original_config = [&]() {
    current_config = !published_auto_config.isEmpty()
        ? hm::stitching::merge_game_config_rollback(auto_config, old_config, current_config)
        : YAML::Clone(old_config);
    const auto status = publish_yaml_config(config_path, current_config);
    if (!status.ok()) {
      appendLog(QString("failed to restore private config after cleanup failure %1").arg(config_file));
    }
    return status.ok();
  };
  auto remove_cleanup_metadata = [&](const std::set<QString>& removed_paths) {
    YAML::Node copied_imports = current_config["hstream_ui"]["copied_imports"];
    YAML::Node copied_replacement(YAML::NodeType::Sequence);
    if (copied_imports && copied_imports.IsSequence()) {
      for (const auto& item : copied_imports) {
        const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
        if (!removed_paths.count(path)) {
          copied_replacement.push_back(item.as<std::string>());
        }
      }
    }
    current_config["hstream_ui"]["copied_imports"] = copied_replacement;

    YAML::Node sources = current_config["hstream_ui"]["auto_import_sources"];
    if (sources && sources.IsSequence()) {
      YAML::Node source_replacement(YAML::NodeType::Sequence);
      for (const auto& item : sources) {
        if (item["path"]) {
          const QString path =
              normalized_config_video_path(game_dir, QString::fromStdString(item["path"].as<std::string>()));
          if (removed_paths.count(path)) {
            continue;
          }
        }
        source_replacement.push_back(item);
      }
      current_config["hstream_ui"]["auto_import_sources"] = source_replacement;
    }
  };

  std::set<QString> removed_paths;
  for (const QString& path : cleanup_paths) {
    if (!removeImportedVideoPath(path, true)) {
      if (removed_paths.empty()) {
        if (restore_auto_selection_on_failure) {
          return restore_original_config() ? CopiedImportCleanupResult::kRolledBack
                                           : CopiedImportCleanupResult::kCommittedWithCleanupFailure;
        }
      } else {
        remove_cleanup_metadata(removed_paths);
        if (!publish_yaml_config(config_path, current_config).ok()) {
          appendLog(QString("failed to update copied import metadata %1").arg(config_file));
        }
      }
      return CopiedImportCleanupResult::kCommittedWithCleanupFailure;
    }
    removed_paths.insert(path);
  }
  if (!removed_paths.empty()) {
    remove_cleanup_metadata(removed_paths);
    if (!publish_yaml_config(config_path, current_config).ok()) {
      appendLog(QString("failed to update copied import metadata %1").arg(config_file));
      return CopiedImportCleanupResult::kCommittedWithCleanupFailure;
    }
  }
  return CopiedImportCleanupResult::kSuccess;
}

bool HStreamWindow::syncRuntimeExplicitVideoConfig(YAML::Node& config) {
  bool changed = false;
  YAML::Node explicit_left = config["hstream_ui"]["video_roles"]["left"];
  YAML::Node explicit_right = config["hstream_ui"]["video_roles"]["right"];
  const bool has_left = explicit_left && explicit_left.IsSequence() && explicit_left.size() > 0;
  const bool has_right = explicit_right && explicit_right.IsSequence() && explicit_right.size() > 0;
  if (has_left && has_right) {
    auto normalized_playlist = [](const YAML::Node& explicit_videos, YAML::Node& playlist) {
      std::map<QString, QString> by_chapter;
      std::set<QString> schemes;
      std::set<QString> unique_paths;
      for (const auto& item : explicit_videos) {
        const QString path = QString::fromStdString(item.as<std::string>());
        if (!unique_paths.insert(path).second) {
          return false;
        }
        const std::optional<QString> chapter = explicit_chapter_key(path);
        if (chapter && !by_chapter.emplace(*chapter, path).second) {
          return false;
        }
        if (chapter) {
          schemes.insert(chapter->section(':', 0, 0));
        }
      }
      playlist = YAML::Node(YAML::NodeType::Sequence);
      if (by_chapter.size() == explicit_videos.size() && schemes.size() == 1) {
        for (const auto& [_, path] : by_chapter) {
          playlist.push_back(path.toStdString());
        }
      } else {
        for (const auto& item : explicit_videos) {
          playlist.push_back(item.as<std::string>());
        }
      }
      return playlist.size() > 0;
    };

    YAML::Node left_list;
    YAML::Node right_list;
    if (normalized_playlist(explicit_left, left_list) && normalized_playlist(explicit_right, right_list)) {
      // Each camera's chapter labels describe only that camera's physical file boundaries. Preserve and sort the two
      // playlists independently; requiring equal labels or counts drops valid footage when cameras roll files apart.
      config["game"]["videos"]["left"] = left_list;
      config["game"]["videos"]["right"] = right_list;
      return true;
    }
  }

  changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
  changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
  if (has_left && has_right) {
    appendLog("explicit Left/Right runtime config contains incompatible chapter names");
  } else {
    appendLog("explicit Left/Right selection will apply after both sides are assigned");
  }
  return changed;
}

bool HStreamWindow::savePrivateConfigForRole(
    const QString& role,
    const QString& relative_path,
    QByteArray* original_config,
    bool* had_config,
    QByteArray* published_config) {
  if (original_config)
    original_config->clear();
  if (had_config)
    *had_config = false;
  if (published_config)
    published_config->clear();
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
      if (original_config)
        *original_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
      if (had_config)
        *had_config = true;
    } catch (const std::exception& exc) {
      appendLog(QString("could not update private config: %1").arg(exc.what()));
      return false;
    }
  }

  bool changed = false;
  bool video_inputs_changed = false;
  if (is_explicit_role(role)) {
    YAML::Node list = config["hstream_ui"]["video_roles"][role.toStdString()];
    if (!list || !list.IsSequence()) {
      config["hstream_ui"]["video_roles"][role.toStdString()] = YAML::Node(YAML::NodeType::Sequence);
      list = config["hstream_ui"]["video_roles"][role.toStdString()];
      changed = true;
    }
    bool exists = false;
    for (const auto& item : list) {
      if (QString::fromStdString(item.as<std::string>()) == relative_path) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      list.push_back(relative_path.toStdString());
      changed = true;
      video_inputs_changed = true;
    }
    changed = clear_stitching_frame_offsets(config) || changed;
  }

  if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
  }

  if (role == "auto") {
    video_inputs_changed = true;
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
    appendLog("auto video set will be discovered from the game directory");
  }

  if (video_inputs_changed) {
    changed = invalidate_stitching_calibration(config, "input") || changed;
  }

  if (!changed) {
    if (published_config && config.IsDefined())
      *published_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
    return true;
  }

  if (std::getenv("HM_TEST_PRIVATE_CONFIG_SAVE_FAIL") != nullptr) {
    appendLog("injected private config save failure");
    return false;
  }

  const auto publish = publish_yaml_config(config_path, config);
  if (!publish.ok())
    appendLog(QString("failed to write private config %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
  if (publish.ok() && published_config)
    *published_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
  return publish.ok();
}

bool HStreamWindow::removePrivateConfigForRole(
    const QString& role,
    const QString& relative_path,
    QByteArray* original_config,
    bool* had_config,
    bool* copied_import,
    QByteArray* published_config) {
  if (original_config)
    original_config->clear();
  if (had_config)
    *had_config = false;
  if (copied_import)
    *copied_import = false;
  if (published_config)
    published_config->clear();
  if (!is_explicit_role(role) && role != "auto") {
    return true;
  }

  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  if (!fs::exists(config_path)) {
    return true;
  }
  YAML::Node config;
  try {
    config = YAML::LoadFile(config_path.string());
    if (original_config)
      *original_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
    if (had_config)
      *had_config = true;
    if (copied_import) {
      *copied_import = is_copied_import_in_config(config, QDir(gameDirectory(game_id_edit_->text())), relative_path);
    }
  } catch (const std::exception& exc) {
    appendLog(QString("could not update private config: %1").arg(exc.what()));
    return false;
  }

  bool changed = false;
  bool video_inputs_changed = role == "auto";
  auto matches_path = [&](const QString& value) {
    if (value == relative_path) {
      return true;
    }
    const QString normalized = normalized_config_video_path(QDir(gameDirectory(game_id_edit_->text())), value);
    return normalized == relative_path;
  };

  auto remove_from_list = [&](YAML::Node parent, const QString& key, bool video_input) {
    YAML::Node list = parent[key.toStdString()];
    if (!list || !list.IsSequence()) {
      return;
    }

    YAML::Node replacement(YAML::NodeType::Sequence);
    for (const auto& item : list) {
      const QString value = QString::fromStdString(item.as<std::string>());
      if (matches_path(value)) {
        changed = true;
        video_inputs_changed = video_inputs_changed || video_input;
      } else {
        replacement.push_back(value.toStdString());
      }
    }
    parent[key.toStdString()] = replacement;
  };

  auto remove_auto_source_metadata = [&]() {
    YAML::Node list = config["hstream_ui"]["auto_import_sources"];
    if (!list || !list.IsSequence()) {
      return;
    }

    YAML::Node replacement(YAML::NodeType::Sequence);
    for (const auto& item : list) {
      if (item["path"] && matches_path(QString::fromStdString(item["path"].as<std::string>()))) {
        changed = true;
      } else {
        replacement.push_back(item);
      }
    }
    config["hstream_ui"]["auto_import_sources"] = replacement;
  };

  if (is_explicit_role(role)) {
    remove_from_list(config["hstream_ui"]["video_roles"], role, true);
  }
  remove_from_list(config["hstream_ui"], "copied_imports", false);
  remove_auto_source_metadata();
  if (role == "auto") {
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
  } else if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
  }
  if (video_inputs_changed) {
    changed = invalidate_stitching_calibration(config, "input") || changed;
  }
  if (!changed) {
    if (published_config) {
      *published_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
    }
    return true;
  }

  const auto publish = publish_yaml_config(config_path, config);
  if (!publish.ok())
    appendLog(QString("failed to write private config %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
  if (publish.ok() && published_config) {
    *published_config = QByteArray::fromStdString(YAML::Dump(config) + "\n");
  }
  return publish.ok();
}

bool HStreamWindow::restorePrivateConfigAfterRemoveFailure(
    const QByteArray& original_config,
    bool had_config,
    const QByteArray& removed_config) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  try {
    const YAML::Node original = had_config ? YAML::Load(original_config.toStdString()) : YAML::Node();
    const YAML::Node removed = removed_config.isEmpty() ? YAML::Node() : YAML::Load(removed_config.toStdString());
    YAML::Node latest;
    if (fs::is_regular_file(config_path)) {
      latest = YAML::LoadFile(config_path.string());
    }
    const YAML::Node restored = hm::stitching::merge_game_config_rollback(removed, original, latest);
    const auto status = publish_yaml_config(config_path, restored);
    if (!status.ok()) {
      appendLog(QString("failed to publish private config rollback: %1").arg(status.ToString().c_str()));
      return false;
    }
  } catch (const std::exception& exc) {
    appendLog(QString("could not restore private config: %1").arg(exc.what()));
    return false;
  }
  return true;
}

bool HStreamWindow::resolveImportedVideoPath(
    const QString& relative_path,
    bool allow_regular_delete,
    QString* resolved_imported_path) {
  if (!resolved_imported_path)
    return false;
  resolved_imported_path->clear();
  const QDir game_dir(gameDirectory(game_id_edit_->text()));
  const QString game_root = QDir::cleanPath(game_dir.absolutePath());
  const QString canonical_game_root = canonical_dir_path(game_root);
  const QFileInfo requested(relative_path);
  const QString imported_path =
      requested.isAbsolute() ? requested.absoluteFilePath() : game_dir.absoluteFilePath(relative_path);
  const QString normalized_path = QDir::cleanPath(QFileInfo(imported_path).absoluteFilePath());
  if (normalized_path != game_root && !normalized_path.startsWith(game_root + "/")) {
    appendLog(QString("not deleting video outside game directory %1").arg(relative_path));
    return true;
  }
  const QFileInfo imported(imported_path);
  if (!imported.exists() && !imported.isSymLink()) {
    return true;
  }
  const QString canonical_parent = canonical_dir_path(QFileInfo(imported_path).absolutePath());
  if (canonical_parent.isEmpty() ||
      (canonical_parent != canonical_game_root && !canonical_parent.startsWith(canonical_game_root + "/"))) {
    appendLog(QString("not deleting video outside real game directory %1").arg(relative_path));
    return false;
  }
  if (!imported.isSymLink()) {
    const QString canonical_imported = imported.canonicalFilePath();
    if (canonical_imported.isEmpty() ||
        (canonical_imported != canonical_game_root && !canonical_imported.startsWith(canonical_game_root + "/"))) {
      appendLog(QString("not deleting video outside real game directory %1").arg(relative_path));
      return false;
    }
  }
  if (!imported.isSymLink() && !allow_regular_delete) {
    appendLog(QString("not deleting regular video file %1").arg(relative_path));
    return false;
  }
  *resolved_imported_path = imported_path;
  return true;
}

bool HStreamWindow::removeImportedVideoPath(const QString& relative_path, bool allow_regular_delete) {
  QString imported_path;
  if (!resolveImportedVideoPath(relative_path, allow_regular_delete, &imported_path))
    return false;
  if (imported_path.isEmpty())
    return true;
  if (const char* fail_path = std::getenv("HM_TEST_VIDEO_REMOVE_FAIL");
      fail_path != nullptr && relative_path == QString::fromUtf8(fail_path)) {
    appendLog(QString("injected failure removing imported video %1").arg(relative_path));
    return false;
  }
  if (!QFile::remove(imported_path)) {
    appendLog(QString("failed to remove imported video link %1").arg(relative_path));
    return false;
  }
  return true;
}

void HStreamWindow::toggleOutput(const QString& id, bool enabled) {
  const bool pipeline_running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  output_states_[id]->setText(pipeline_running ? "NEXT RUN" : (enabled ? "ENABLED" : "STOPPED"));
  appendLog(QString("output route %1 %2").arg(id, enabled ? "enabled" : "disabled"));
  if (id == "archive-file") {
    updateArchiveOutputPathLabel();
  }
  if (pipeline_running) {
    appendLog("output route change will apply on the next pipeline start with the current runner backend");
  }
  updateRunControls();
}

void HStreamWindow::redirectYoutube() {
  QCheckBox* toggle = output_toggles_["youtube-primary"];
  const bool was_blocked = toggle->blockSignals(true);
  toggle->setChecked(true);
  toggle->blockSignals(was_blocked);
  output_states_["youtube-primary"]->setText("REDIRECTED");
  appendLog("youtube-primary RTMP route enabled for the next pipeline start");
}

void HStreamWindow::addRtspOutput() {
  ++dynamic_rtsp_count_;
  const QString id = QString("rtsp-dynamic-%1").arg(dynamic_rtsp_count_);
  auto* row = new QHBoxLayout();
  auto* toggle = new QCheckBox(QString("RTSP Mount /dynamic%1").arg(dynamic_rtsp_count_));
  toggle->setObjectName("outputToggle_" + id);
  toggle->setChecked(true);
  set_control_help(
      toggle,
      "Enable this additional local RTSP mount for the next Program run. Changes made while playing apply on the next run.");
  auto* state = make_value_label("outputState_" + id, "ENABLED");
  output_toggles_[id] = toggle;
  output_states_[id] = state;
  connect(toggle, &QCheckBox::toggled, this, [this, id](bool enabled) { toggleOutput(id, enabled); });
  row->addWidget(toggle, 1);
  row->addWidget(state);
  output_list_->insertLayout(output_list_->count() - 2, row);
  appendLog(QString("rtsp server mount /dynamic%1 enabled for the next pipeline start").arg(dynamic_rtsp_count_));
}

void HStreamWindow::appendLog(const QString& message) {
  const QString entry_timestamp = timestamp();
  const QByteArray plain_entry = QString("%1 %2\n").arg(entry_timestamp, message).toUtf8();
  QString archive_log_error;
#ifdef Q_OS_UNIX
  if (archive_job_log_enabled_ && archive_job_log_.isOpen() &&
      qEnvironmentVariableIsSet("HSTREAM_UI_TEST_ARCHIVE_FORCE_LOG_CLOSE_AND_REPLACE")) {
    qunsetenv("HSTREAM_UI_TEST_ARCHIVE_FORCE_LOG_CLOSE_AND_REPLACE");
    archive_job_log_.close();
    archive_job_log_enabled_ = false;
    const QByteArray encoded_log = QFile::encodeName(archive_job_log_path_);
    ::unlink(encoded_log.constData());
    const int replacement_fd =
        ::open(encoded_log.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (replacement_fd >= 0) {
      constexpr char kReplacement[] = "injected foreign closed log pathname";
      const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
      (void)replacement_bytes;
      ::close(replacement_fd);
    }
    archive_log_error = "archive log close and replacement requested by test";
  }
#endif
  if (archive_job_log_enabled_ && archive_job_log_.isOpen()) {
    if (archive_job_log_.write(plain_entry) != plain_entry.size() || !archive_job_log_.flush()) {
      archive_log_error = archive_job_log_.errorString();
      archive_job_log_.close();
      archive_job_log_enabled_ = false;
    }
  }
  if (capture_complete_log_) {
    complete_log_ += QString::fromUtf8(plain_entry);
    if (complete_log_.size() > kMaxCapturedLogCharacters) {
      const qsizetype overflow = complete_log_.size() - kMaxCapturedLogCharacters;
      const qsizetype next_line = complete_log_.indexOf('\n', overflow);
      complete_log_.remove(0, next_line >= 0 ? next_line + 1 : overflow);
    }
  }
  const QString html =
      QString("<span style=\"color:#667085\">%1</span> %2").arg(entry_timestamp.toHtmlEscaped(), ansi_to_html(message));
  log_->append(html);
  if (!archive_log_error.isEmpty())
    appendLog(QString("archive job log write failed; file logging stopped: %1").arg(archive_log_error));
}

QString HStreamWindow::writePlaytrackerRuntimeConfig() {
  const QString game_id = !active_run_game_id_.isEmpty()
      ? active_run_game_id_
      : (game_id_edit_ ? game_id_edit_->text().trimmed() : QString());
  if (game_id.isEmpty()) {
    return {};
  }
  const QString game_dir = gameDirectory(game_id);
  QDir runtime_dir(QDir(game_dir).filePath(".hstream-ui"));
  if (!runtime_dir.exists() && !runtime_dir.mkpath(".")) {
    appendLog(QString("could not create playtracker runtime config directory %1").arg(runtime_dir.path()));
    return {};
  }

  const QString persistent_runtime_config = runtime_dir.filePath("play_tracker_config.yaml");
  const bool live_update = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  const QString runtime_config_path = live_update
      ? runtime_dir.filePath(QString("play_tracker_runtime_%1.yaml").arg(scheduled_playtracker_control_generation_))
      : persistent_runtime_config;
  try {
    QString base_playtracker_config = pipelineConfigPath("play_tracker_config.yaml");
    const fs::path game_config_path = fs::path(game_dir.toStdString()) / "config.yaml";
    auto loaded_game_config = hm::stitching::load_game_config_file(game_config_path);
    if (!loaded_game_config.ok()) {
      appendLog(
          QString("could not read playtracker game config: %1").arg(loaded_game_config.status().ToString().c_str()));
      return {};
    }
    if (loaded_game_config->has_value()) {
      YAML::Node game_config = **loaded_game_config;
      YAML::Node configured_config_file;
      if (lookup_yaml_path(game_config, "pipeline.ds-playtracker.config-file", &configured_config_file) &&
          configured_config_file.IsScalar()) {
        QString configured = QString::fromStdString(configured_config_file.as<std::string>());
        const QString working_dir = pipelineWorkingDirectory();
        const QStringList candidates = playtracker_config_candidates(configured, game_dir, working_dir);
        for (const QString& candidate : candidates) {
          if (same_file_path(candidate, runtime_config_path) || same_file_path(candidate, persistent_runtime_config) ||
              is_ui_persistent_playtracker_config(candidate, game_dir)) {
            YAML::Node base_config_file;
            if (lookup_yaml_path(game_config, "hstream_ui.playtracker_config_base", &base_config_file) &&
                base_config_file.IsScalar()) {
              configured = QString::fromStdString(base_config_file.as<std::string>());
              const QStringList base_candidates = playtracker_config_candidates(configured, game_dir, working_dir);
              for (const QString& base_candidate : base_candidates) {
                if (QFileInfo::exists(base_candidate)) {
                  base_playtracker_config = base_candidate;
                  break;
                }
              }
            }
            break;
          }
          if (QFileInfo::exists(candidate)) {
            base_playtracker_config = candidate;
            break;
          }
        }
      }
    }
    YAML::Node play_tracker_config = QFileInfo::exists(base_playtracker_config)
        ? YAML::LoadFile(base_playtracker_config.toStdString())
        : YAML::Node(YAML::NodeType::Map);
    if (!play_tracker_config["play-tracker"] || !play_tracker_config["play-tracker"].IsMap()) {
      play_tracker_config["play-tracker"] = YAML::Node(YAML::NodeType::Map);
    }
    if (!play_tracker_config["play-tracker"]["live-boxes"] ||
        !play_tracker_config["play-tracker"]["live-boxes"].IsSequence()) {
      play_tracker_config["play-tracker"]["live-boxes"] = YAML::Node(YAML::NodeType::Sequence);
    }
    YAML::Node live_boxes = play_tracker_config["play-tracker"]["live-boxes"];
    if (live_boxes.size() == 0) {
      for (const char* name : {"current_roi", "current_roi_aspect"}) {
        YAML::Node box(YAML::NodeType::Map);
        box["name"] = name;
        live_boxes.push_back(box);
      }
    }
    const auto live_box_roles = hm::resolve_playtracker_live_box_roles(live_boxes);
    if (!live_box_roles.ok()) {
      appendLog(
          QString("could not resolve playtracker live-box roles: %1").arg(live_box_roles.status().ToString().c_str()));
      return {};
    }

    auto slider_value = [this](const QString& id) -> int {
      const auto it = camera_sliders_.find(id);
      return it == camera_sliders_.end() ? 0 : it->second->value();
    };
    auto slider_changed = [this, &slider_value](const QString& id) -> bool {
      const auto default_it = camera_defaults_.find(id);
      return default_it != camera_defaults_.end() && slider_value(id) != default_it->second;
    };
    auto apply_live_box = [&](size_t index) {
      auto set_if_changed = [&](const QString& id, const char* key) {
        if (slider_changed(id)) {
          live_boxes[index][key] = static_cast<double>(slider_value(id)) / 10.0;
        }
      };
      auto set_integer_if_changed = [&](const QString& id, const char* key) {
        if (slider_changed(id)) {
          live_boxes[index][key] = slider_value(id);
        }
      };
      set_integer_if_changed("Stop_Direction_Change_Delay_Frames", "stop-translation-on-dir-change-delay");
      if (slider_changed("Cancel_Stop_On_Opposite_Direction")) {
        live_boxes[index]["cancel-stop-on-opposite-dir"] = slider_value("Cancel_Stop_On_Opposite_Direction") != 0;
      }
      set_integer_if_changed("Stop_Cancel_Hysteresis_Frames", "cancel-stop-hysteresis-frames");
      set_integer_if_changed("Stop_Delay_Cooldown_Frames", "stop-delay-cooldown-frames");
      set_integer_if_changed("Time_To_Dest_Speed_Limit_Frames", "time-to-dest-speed-limit-frames");
      set_integer_if_changed("Post_Nonstop_Stop_Delay_Frames", "post-nonstop-stop-delay-count");
      set_if_changed("Max_Speed_X_x10", "max-speed-x");
      set_if_changed("Max_Speed_Y_x10", "max-speed-y");
      set_if_changed("Max_Accel_X_x10", "max-accel-x");
      set_if_changed("Max_Accel_Y_x10", "max-accel-y");
    };
    YAML::Node play_tracker = play_tracker_config["play-tracker"];
    play_tracker["hstream-apply-to-fast-box"] =
        publishing_playtracker_force_all_targets_ || slider_value("Apply_To_Fast_Box") != 0;
    play_tracker["hstream-apply-to-follower-box"] =
        publishing_playtracker_force_all_targets_ || slider_value("Apply_To_Follower_Box") != 0;
    // Each command is an immutable sparse delta. Do not inherit a stale delta
    // if a base file was previously generated by hstream-ui.
    play_tracker["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
    YAML::Node runtime_tuning = play_tracker["hstream-runtime-tuning"];
    const auto& publishing_controls = publishing_playtracker_controls_.has_value() ? *publishing_playtracker_controls_
                                                                                   : scheduled_playtracker_controls_;
    auto set_changed_int = [&](const char* key, const char* control_id) {
      if (publishing_controls.count(control_id))
        runtime_tuning[key] = slider_value(control_id);
    };
    set_changed_int("stop-translation-on-dir-change-delay", "Stop_Direction_Change_Delay_Frames");
    set_changed_int("zoom-in-aggressiveness", "Zoom_In_Aggressiveness");
    if (publishing_controls.count("Cancel_Stop_On_Opposite_Direction"))
      runtime_tuning["cancel-stop-on-opposite-dir"] = slider_value("Cancel_Stop_On_Opposite_Direction") != 0;
    set_changed_int("cancel-stop-hysteresis-frames", "Stop_Cancel_Hysteresis_Frames");
    set_changed_int("stop-delay-cooldown-frames", "Stop_Delay_Cooldown_Frames");
    set_changed_int("time-to-dest-speed-limit-frames", "Time_To_Dest_Speed_Limit_Frames");
    set_changed_int("post-nonstop-stop-delay-count", "Post_Nonstop_Stop_Delay_Frames");
    set_changed_int("overshoot-stop-delay-count", "Overshoot_Stop_Delay_Frames");
    if (publishing_controls.count("Overshoot_Speed_Ratio_x100"))
      runtime_tuning["overshoot-scale-speed-ratio"] = ratio_x100(slider_value("Overshoot_Speed_Ratio_x100"));
    if (publishing_controls.count("Max_Speed_X_x10"))
      runtime_tuning["max-speed-x"] = static_cast<double>(slider_value("Max_Speed_X_x10")) / 10.0;
    if (publishing_controls.count("Max_Speed_Y_x10"))
      runtime_tuning["max-speed-y"] = static_cast<double>(slider_value("Max_Speed_Y_x10")) / 10.0;
    if (publishing_controls.count("Max_Accel_X_x10"))
      runtime_tuning["max-accel-x"] = static_cast<double>(slider_value("Max_Accel_X_x10")) / 10.0;
    if (publishing_controls.count("Max_Accel_Y_x10"))
      runtime_tuning["max-accel-y"] = static_cast<double>(slider_value("Max_Accel_Y_x10")) / 10.0;
    if (slider_changed("Overshoot_Stop_Delay_Frames")) {
      play_tracker["overshoot-stop-delay-count"] = slider_value("Overshoot_Stop_Delay_Frames");
    }
    if (slider_changed("Overshoot_Speed_Ratio_x100")) {
      play_tracker["overshoot-scale-speed-ratio"] = ratio_x100(slider_value("Overshoot_Speed_Ratio_x100"));
    }
    if (slider_value("Apply_To_Fast_Box") != 0) {
      apply_live_box(live_box_roles->fast_index);
    }
    if (slider_value("Apply_To_Follower_Box") != 0) {
      apply_live_box(live_box_roles->follower_index);
    }

    const absl::Status publish = hm::stitching::publish_named_file(
        fs::path(runtime_config_path.toStdString()), YAML::Dump(play_tracker_config) + "\n");
    if (!publish.ok()) {
      appendLog(QString("could not atomically write playtracker runtime config %1: %2")
                    .arg(runtime_config_path, publish.ToString().c_str()));
      return {};
    }
    return runtime_config_path;
  } catch (const std::exception& exc) {
    appendLog(QString("could not save playtracker runtime config: %1").arg(exc.what()));
    return {};
  }
}

void HStreamWindow::handleRuntimeControlResponse(const QString& line) {
  auto acknowledge_matching = [this](const auto& matches, bool failed) {
    const auto pending = std::find_if(pending_runtime_controls_.begin(), pending_runtime_controls_.end(), matches);
    if (pending == pending_runtime_controls_.end()) {
      return;
    }
    const PendingRuntimeControl acknowledged = *pending;
    pending_runtime_controls_.erase(pending);
    if (failed) {
      for (auto other = pending_runtime_controls_.begin(); other != pending_runtime_controls_.end();) {
        if (other->batch_id != acknowledged.batch_id) {
          ++other;
          continue;
        }
        if (other->property == "runtime-tuning-config-file" &&
            other->runtime_value != last_playtracker_runtime_snapshot_) {
          QFile::remove(other->runtime_value);
        }
        other = pending_runtime_controls_.erase(other);
      }
    }
    if (acknowledged.property == "runtime-tuning-config-file") {
      if (failed) {
        QFile::remove(acknowledged.runtime_value);
      } else {
        if (!last_playtracker_runtime_snapshot_.isEmpty() &&
            last_playtracker_runtime_snapshot_ != acknowledged.runtime_value) {
          QFile::remove(last_playtracker_runtime_snapshot_);
        }
        last_playtracker_runtime_snapshot_ = acknowledged.runtime_value;
      }
    }
    const auto batch = runtime_control_batches_.find(acknowledged.batch_id);
    if (batch == runtime_control_batches_.end()) {
      return;
    }
    batch->second.failed = batch->second.failed || failed;
    if (failed) {
      batch->second.pending_commands = 0;
    } else if (batch->second.pending_commands > 0) {
      --batch->second.pending_commands;
    }
    if (batch->second.pending_commands != 0) {
      return;
    }
    const QString result = batch->second.failed ? "failed" : "live";
    for (const auto& [control_id, control_value] : batch->second.controls) {
      appendLog(QString("camera control %1=%2 apply=%3").arg(control_id).arg(control_value).arg(result));
    }
    runtime_control_batches_.erase(batch);
    flushScheduledRuntimeControls();
  };

  static const QRegularExpression success_pattern(R"(^runtime property (\S+) (\S+?)=(.*)$)");
  const QRegularExpressionMatch success = success_pattern.match(line);
  if (success.hasMatch()) {
    const QString element = success.captured(1);
    const QString property = success.captured(2);
    const QString runtime_value = success.captured(3);
    acknowledge_matching(
        [&](const PendingRuntimeControl& control) {
          return control.element == element && control.property == property && control.runtime_value == runtime_value;
        },
        false);
    return;
  }

  if (!line.startsWith("runtime command failed:") || pending_runtime_controls_.empty()) {
    return;
  }
  const auto pending = std::find_if(
      pending_runtime_controls_.begin(), pending_runtime_controls_.end(), [&](const PendingRuntimeControl& control) {
        return line.contains(control.element) && (line.contains(control.property) || !line.contains('.'));
      });
  if (pending != pending_runtime_controls_.end()) {
    const QString element = pending->element;
    const QString property = pending->property;
    const QString runtime_value = pending->runtime_value;
    acknowledge_matching(
        [&](const PendingRuntimeControl& control) {
          return control.element == element && control.property == property && control.runtime_value == runtime_value;
        },
        true);
  } else {
    const PendingRuntimeControl first = pending_runtime_controls_.front();
    acknowledge_matching(
        [&](const PendingRuntimeControl& control) {
          return control.element == first.element && control.property == first.property &&
              control.runtime_value == first.runtime_value;
        },
        true);
  }
}

void HStreamWindow::failPendingRuntimeControls(const QString& reason) {
  for (const PendingRuntimeControl& pending : pending_runtime_controls_) {
    if (pending.property == "runtime-tuning-config-file" && pending.runtime_value != last_playtracker_runtime_snapshot_)
      QFile::remove(pending.runtime_value);
  }
  for (const auto& [batch_id, batch] : runtime_control_batches_) {
    (void)batch_id;
    for (const auto& [control_id, control_value] : batch.controls) {
      appendLog(QString("camera control %1=%2 apply=failed reason=%3").arg(control_id).arg(control_value).arg(reason));
    }
  }
  pending_runtime_controls_.clear();
  runtime_control_batches_.clear();
}

int HStreamWindow::runtimeControlAckTimeoutMs() const {
  bool valid = false;
  const int test_timeout = qEnvironmentVariableIntValue("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", &valid);
  return valid && test_timeout > 0 ? test_timeout : kRuntimeControlAckTimeoutMs;
}

void HStreamWindow::timeoutRuntimeControlBatch(quint64 batch_id) {
  const auto batch = runtime_control_batches_.find(batch_id);
  if (batch == runtime_control_batches_.end()) {
    return;
  }
  for (auto pending = pending_runtime_controls_.begin(); pending != pending_runtime_controls_.end();) {
    if (pending->batch_id != batch_id) {
      ++pending;
      continue;
    }
    if (pending->property == "runtime-tuning-config-file" &&
        pending->runtime_value != last_playtracker_runtime_snapshot_) {
      QFile::remove(pending->runtime_value);
    }
    pending = pending_runtime_controls_.erase(pending);
  }
  for (const auto& [control_id, control_value] : batch->second.controls) {
    appendLog(
        QString("camera control %1=%2 apply=failed reason=acknowledgement-timeout").arg(control_id).arg(control_value));
  }
  runtime_control_batches_.erase(batch);
  flushScheduledRuntimeControls();
}

bool HStreamWindow::publishRuntimeControlBatch(
    const std::map<QString, int>& controls,
    const std::vector<RuntimePropertyCommand>& commands) {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning || controls.empty() ||
      commands.empty() || !runtime_control_batches_.empty()) {
    return false;
  }
  const quint64 batch_id = ++next_runtime_control_batch_id_;
  runtime_control_batches_.emplace(batch_id, RuntimeControlBatch{controls, commands.size(), false});
  QStringList assignments;
  for (const RuntimePropertyCommand& property_command : commands) {
    assignments.push_back(
        QString("%1 %2=%3").arg(property_command.element, property_command.property, property_command.value));
    pending_runtime_controls_.push_back(
        {property_command.element, property_command.property, property_command.value, batch_id});
  }
  const QByteArray command = QString("@set-properties %1\n").arg(assignments.join(';')).toLocal8Bit();
  if (pipeline_process_->write(command) != command.size()) {
    pending_runtime_controls_.erase(
        std::remove_if(
            pending_runtime_controls_.begin(),
            pending_runtime_controls_.end(),
            [batch_id](const PendingRuntimeControl& pending) { return pending.batch_id == batch_id; }),
        pending_runtime_controls_.end());
    runtime_control_batches_.erase(batch_id);
    for (const auto& [control_id, control_value] : controls) {
      appendLog(QString("camera control %1=%2 apply=failed reason=pipeline command write")
                    .arg(control_id)
                    .arg(control_value));
    }
    return false;
  }
  for (const auto& [control_id, control_value] : controls) {
    appendLog(QString("camera control %1=%2 apply=pending").arg(control_id).arg(control_value));
  }
  QTimer::singleShot(runtimeControlAckTimeoutMs(), this, [this, batch_id]() { timeoutRuntimeControlBatch(batch_id); });
  return true;
}

bool HStreamWindow::sendLiveCameraControl(const QString& id, int value) {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    return false;
  }
  if (active_run_is_calibration_ && id != "Stitch_Rotate_Degrees") {
    return false;
  }
  if (id == "Stitch_Rotate_Degrees") {
    scheduleRotationRuntimeControl(id, value);
    return false;
  }
  const QSet<QString> fixed_edge_rotation_controls = {
      "Link_Fixed_Edge_Rotation_Left_Right",
      "Left_Fixed_Edge_Rotation_Angle_x10",
      "Right_Fixed_Edge_Rotation_Angle_x10",
  };
  if (fixed_edge_rotation_controls.contains(id)) {
    scheduleRotationRuntimeControl(id, value);
    return false;
  }
  const QSet<QString> playtracker_live_controls = {
      "Stop_Direction_Change_Delay_Frames",
      "Zoom_In_Aggressiveness",
      "Cancel_Stop_On_Opposite_Direction",
      "Stop_Cancel_Hysteresis_Frames",
      "Stop_Delay_Cooldown_Frames",
      "Time_To_Dest_Speed_Limit_Frames",
      "Overshoot_Stop_Delay_Frames",
      "Post_Nonstop_Stop_Delay_Frames",
      "Overshoot_Speed_Ratio_x100",
      "Max_Speed_X_x10",
      "Max_Speed_Y_x10",
      "Max_Accel_X_x10",
      "Max_Accel_Y_x10",
      "Apply_To_Fast_Box",
      "Apply_To_Follower_Box",
  };
  if (playtracker_live_controls.contains(id)) {
    schedulePlaytrackerRuntimeControl(id, value);
    return false;
  }
  if (id == "Bring_Up_Shadows" || id == "Lift_Shadow_Black_Point" || id == "Exposure_x100") {
    schedulePlaycropperRuntimeControl(id, value);
    return false;
  }
  return false;
}

void HStreamWindow::scheduleRotationRuntimeControl(const QString& id, int value) {
  appendLog(QString("camera control %1=%2 apply=scheduled").arg(id).arg(value));
  scheduled_rotation_controls_[id] = value;
  scheduled_rotation_controls_ready_ = false;
  const quint64 generation = ++scheduled_rotation_control_generation_;
  QTimer::singleShot(120, this, [this, generation]() {
    if (generation != scheduled_rotation_control_generation_ || !pipeline_process_ ||
        pipeline_process_->state() == QProcess::NotRunning) {
      return;
    }
    scheduled_rotation_controls_ready_ = true;
    flushScheduledRuntimeControls();
  });
}

void HStreamWindow::schedulePlaytrackerRuntimeControl(const QString& id, int value) {
  appendLog(QString("camera control %1=%2 apply=scheduled").arg(id).arg(value));
  scheduled_playtracker_controls_[id] = value;
  scheduled_playtracker_controls_ready_ = false;
  const quint64 generation = ++scheduled_playtracker_control_generation_;
  QTimer::singleShot(120, this, [this, generation]() {
    if (generation != scheduled_playtracker_control_generation_ || !pipeline_process_ ||
        pipeline_process_->state() == QProcess::NotRunning) {
      return;
    }
    scheduled_playtracker_controls_ready_ = true;
    flushScheduledRuntimeControls();
  });
}

void HStreamWindow::schedulePlaycropperRuntimeControl(const QString& id, int value) {
  appendLog(QString("camera control %1=%2 apply=scheduled").arg(id).arg(value));
  scheduled_playcropper_controls_[id] = value;
  scheduled_playcropper_controls_ready_ = false;
  const quint64 generation = ++scheduled_playcropper_control_generation_;
  QTimer::singleShot(120, this, [this, generation]() {
    if (generation != scheduled_playcropper_control_generation_ || !pipeline_process_ ||
        pipeline_process_->state() == QProcess::NotRunning) {
      return;
    }
    scheduled_playcropper_controls_ready_ = true;
    flushScheduledRuntimeControls();
  });
}

void HStreamWindow::flushScheduledRuntimeControls() {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning || !runtime_control_batches_.empty() ||
      pending_playback_seek_generation_ != 0 || playback_seek_recovery_generation_ != 0) {
    return;
  }
  if (scheduled_rotation_controls_ready_ && !scheduled_rotation_controls_.empty()) {
    const std::map<QString, int> controls = std::move(scheduled_rotation_controls_);
    scheduled_rotation_controls_.clear();
    scheduled_rotation_controls_ready_ = false;
    std::vector<RuntimePropertyCommand> commands;
    if (controls.count("Stitch_Rotate_Degrees")) {
      commands.push_back(
          {"hmstitcher0",
           "post-stitch-rotate-degrees",
           QString::number(90 - cameraControlValue("Stitch_Rotate_Degrees"))});
    }
    const bool has_fixed_edge_change = controls.count("Link_Fixed_Edge_Rotation_Left_Right") ||
        controls.count("Left_Fixed_Edge_Rotation_Angle_x10") || controls.count("Right_Fixed_Edge_Rotation_Angle_x10");
    if (has_fixed_edge_change) {
      const bool linked = cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") != 0;
      const double left_angle = cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10") / 10.0;
      const double right_angle = cameraControlValue("Right_Fixed_Edge_Rotation_Angle_x10") / 10.0;
      auto add_both_stages = [&](const QString& property, double angle) {
        const QString runtime_value = QString::number(angle, 'f', 1);
        commands.push_back({"dsplaytracker0", property, runtime_value});
        commands.push_back({"playcropper0", property, runtime_value});
      };
      if (linked) {
        add_both_stages("fixed-edge-rotation-angle", left_angle);
      } else {
        add_both_stages("fixed-edge-rotation-angle-left", left_angle);
        add_both_stages("fixed-edge-rotation-angle-right", right_angle);
      }
    }
    publishRuntimeControlBatch(controls, commands);
    return;
  }
  if (scheduled_playcropper_controls_ready_ && !scheduled_playcropper_controls_.empty()) {
    const std::map<QString, int> controls = std::move(scheduled_playcropper_controls_);
    scheduled_playcropper_controls_.clear();
    scheduled_playcropper_controls_ready_ = false;
    std::vector<RuntimePropertyCommand> commands;
    const QString tone_element = active_run_high_bit_depth_ ? "hmstitcher0" : "playcropper0";
    if (controls.count("Bring_Up_Shadows")) {
      commands.push_back({tone_element, "shadow-lift", QString::number(cameraControlValue("Bring_Up_Shadows"))});
    }
    if (controls.count("Lift_Shadow_Black_Point")) {
      commands.push_back(
          {tone_element, "shadow-lift-black-point", QString::number(cameraControlValue("Lift_Shadow_Black_Point"))});
    }
    if (controls.count("Exposure_x100")) {
      commands.push_back(
          {tone_element, "exposure", QString::number(cameraControlValue("Exposure_x100") / 100.0, 'f', 2)});
    }
    publishRuntimeControlBatch(controls, commands);
    return;
  }
  if (scheduled_playtracker_controls_ready_ && !scheduled_playtracker_controls_.empty()) {
    publishing_playtracker_controls_ = std::move(scheduled_playtracker_controls_);
    scheduled_playtracker_controls_.clear();
    scheduled_playtracker_controls_ready_ = false;
    publishing_playtracker_force_all_targets_ = scheduled_playtracker_force_all_targets_;
    scheduled_playtracker_force_all_targets_ = false;
    const QString runtime_config_path = writePlaytrackerRuntimeConfig();
    if (runtime_config_path.isEmpty()) {
      for (const auto& [control_id, control_value] : *publishing_playtracker_controls_) {
        appendLog(QString("camera control %1=%2 apply=failed reason=runtime config publication")
                      .arg(control_id)
                      .arg(control_value));
      }
      publishing_playtracker_controls_.reset();
      publishing_playtracker_force_all_targets_ = false;
      return;
    }
    const bool published = publishRuntimeControlBatch(
        *publishing_playtracker_controls_, {{"dsplaytracker0", "runtime-tuning-config-file", runtime_config_path}});
    if (!published) {
      QFile::remove(runtime_config_path);
      publishing_playtracker_controls_.reset();
      publishing_playtracker_force_all_targets_ = false;
      return;
    }
    publishing_playtracker_controls_.reset();
    publishing_playtracker_force_all_targets_ = false;
  }
}

void HStreamWindow::synchronizeFixedEdgeRotationControls(const QString& changed_id, int value) {
  const QString link_id = "Link_Fixed_Edge_Rotation_Left_Right";
  const QString left_id = "Left_Fixed_Edge_Rotation_Angle_x10";
  const QString right_id = "Right_Fixed_Edge_Rotation_Angle_x10";
  if (changed_id != link_id && changed_id != left_id && changed_id != right_id) {
    return;
  }
  const bool linked = changed_id == link_id ? value != 0 : cameraControlValue(link_id) != 0;
  if (!linked) {
    return;
  }
  const QString target_id = changed_id == right_id ? left_id : right_id;
  const int linked_value = changed_id == link_id ? cameraControlValue(left_id) : value;
  const auto slider_it = camera_sliders_.find(target_id);
  if (slider_it == camera_sliders_.end() || slider_it->second->value() == linked_value) {
    return;
  }
  const bool blocked = slider_it->second->blockSignals(true);
  slider_it->second->setValue(linked_value);
  slider_it->second->blockSignals(blocked);
  const auto label_it = camera_value_labels_.find(target_id);
  if (label_it != camera_value_labels_.end()) {
    label_it->second->setText(QString::number(slider_it->second->value()));
  }
}

QSlider* HStreamWindow::addSlider(
    QVBoxLayout* layout,
    const QString& id,
    const QString& label,
    int minimum,
    int maximum,
    int value) {
  auto* row = new QGridLayout();
  auto* name = new QLabel(label);
  auto* value_label = make_value_label("cameraValue_" + id, QString::number(value));
  auto* slider = new WheelPassthroughSlider(Qt::Horizontal);
  slider->setObjectName("cameraSlider_" + id);
  slider->setRange(std::min(minimum, value), std::max(maximum, value));
  slider->setValue(value);
  camera_sliders_[id] = slider;
  camera_value_labels_[id] = value_label;
  camera_defaults_[id] = value;
  connect(slider, &QSlider::valueChanged, this, [this, id, value_label](int new_value) {
    value_label->setText(QString::number(new_value));
    synchronizeFixedEdgeRotationControls(id, new_value);
    const bool sent_live = sendLiveCameraControl(id, new_value);
    if (sent_live) {
      appendLog(QString("camera control %1=%2 apply=pending").arg(id).arg(new_value));
    } else if (
        (scheduled_rotation_controls_.count(id) && scheduled_rotation_controls_.at(id) == new_value) ||
        (scheduled_playtracker_controls_.count(id) && scheduled_playtracker_controls_.at(id) == new_value) ||
        (scheduled_playcropper_controls_.count(id) && scheduled_playcropper_controls_.at(id) == new_value)) {
      // The scheduler already reported the coalesced live update.
    } else {
      appendLog(QString("camera control %1=%2 apply=save/restart").arg(id).arg(new_value));
    }
    updatePresetDirtyState();
  });
  row->addWidget(name, 0, 0);
  row->addWidget(value_label, 0, 1);
  row->addWidget(slider, 1, 0, 1, 2);
  layout->addLayout(row);
  return slider;
}

QCheckBox* HStreamWindow::addCameraCheckBox(
    QVBoxLayout* layout,
    const QString& id,
    const QString& label,
    bool checked) {
  auto* checkbox = new QCheckBox(label);
  checkbox->setObjectName("cameraCheck_" + id);
  checkbox->setChecked(checked);
  camera_checkboxes_[id] = checkbox;
  camera_defaults_[id] = checked ? 1 : 0;
  connect(checkbox, &QCheckBox::toggled, this, [this, id](bool enabled) {
    const int new_value = enabled ? 1 : 0;
    const bool sent_live = sendLiveCameraControl(id, new_value);
    if (sent_live) {
      appendLog(QString("camera control %1=%2 apply=pending").arg(id).arg(new_value));
    } else if (scheduled_playcropper_controls_.count(id) && scheduled_playcropper_controls_.at(id) == new_value) {
      // The scheduler already reported the coalesced live update.
    } else {
      appendLog(QString("camera control %1=%2 apply=save/restart").arg(id).arg(new_value));
    }
    updatePresetDirtyState();
  });
  layout->addWidget(checkbox);
  return checkbox;
}
