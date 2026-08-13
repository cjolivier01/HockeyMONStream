#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/Qt>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEngine>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
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

#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"

#include <QtCore/QUuid>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kFixedEdgeRotationDefaultX10 = 100;
constexpr int kFixedEdgeRotationMaximumX10 = 900;
constexpr int kDefaultStitchCalibrationControlPoints = 1500;
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

QString development_runtime_root() {
  QString application_path = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
  if (application_path.isEmpty()) {
    application_path = QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
  }
  const QString application_name = QFileInfo(application_path).fileName();
  QDir candidate_root(QDir::currentPath());
  while (true) {
    const QString candidate_application =
        candidate_root.filePath(QString("bazel-bin/src/apps/hstream-ui/%1").arg(application_name));
    const QString candidate_path = QFileInfo(candidate_application).canonicalFilePath();
    const QString runner = candidate_root.filePath("bazel-bin/src/apps/pipeline-app/hstream-cli");
    const QString configs = candidate_root.filePath("configs");
    if (!candidate_path.isEmpty() && candidate_path == application_path && QFileInfo(runner).isExecutable() &&
        QFileInfo(configs).isDir()) {
      return candidate_root.absolutePath();
    }
    if (!candidate_root.cdUp()) {
      break;
    }
  }
  return {};
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

QLabel* make_value_label(const QString& object_name, const QString& value) {
  auto* label = new QLabel(value);
  label->setObjectName(object_name);
  label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  label->setMinimumWidth(92);
  return label;
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

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_toggle_callback_) {
      focus_toggle_callback_();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_button_) {
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
};

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
    focus_button_ = new QPushButton(render_surface_);
    focus_button_->setFixedSize(36, 36);
    focus_button_->setToolTip("Focus this video (double-click)");
    focus_button_->setText(QStringLiteral("⛶"));
    QFont focus_button_font = focus_button_->font();
    focus_button_font.setPointSize(17);
    focus_button_->setFont(focus_button_font);
    focus_button_->setStyleSheet(
        "QPushButton { background: rgba(15, 23, 42, 210); border: 1px solid rgba(255, 255, 255, 100); "
        "border-radius: 5px; color: white; padding: 0; }"
        "QPushButton:hover { background: rgba(30, 64, 175, 235); }");
    if (QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0) {
      // The video-overlay sink owns a native child window. Make the control a
      // native sibling too so the window system can stack it visibly above
      // the sink instead of hiding Qt backing-store pixels behind the video.
      focus_button_->setAttribute(Qt::WA_NativeWindow);
      focus_button_->winId();
    }
    render_target_->setFocusButton(focus_button_);
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

  void setFocused(bool focused) {
    focus_button_->setText(focused ? QStringLiteral("↙") : QStringLiteral("⛶"));
    focus_button_->setToolTip(
        focused ? "Restore the HStream controls (double-click)" : "Focus this video (double-click)");
    focus_button_->raise();
  }

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && focus_toggle_callback_) {
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
    constexpr int kButtonMargin = 10;
    focus_button_->move(width - focus_button_->width() - kButtonMargin, kButtonMargin);
    focus_button_->raise();
  }

 private:
  double aspect_ratio_;
  QWidget* render_surface_{nullptr};
  NativeVideoTarget* render_target_{nullptr};
  QPushButton* focus_button_{nullptr};
  std::function<void()> focus_toggle_callback_;
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
  return suffix == "mp4" || suffix == "mkv" || suffix == "mov" || suffix == "avi";
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
  static const QRegularExpression left_right("(left|right)(?:-([0-9]))?\\.mp4$");

  const QRegularExpressionMatch gopro_match = gopro.match(file_name);
  if (gopro_match.hasMatch()) {
    return QString("gopro:%1").arg(gopro_match.captured(1));
  }
  const QRegularExpressionMatch insta360_match = insta360.match(file_name);
  if (insta360_match.hasMatch()) {
    return QString("insta360:%1").arg(insta360_match.captured(3));
  }
  const QRegularExpressionMatch lr_match = left_right.match(file_name);
  if (lr_match.hasMatch()) {
    const QString chapter = lr_match.captured(2).isEmpty() ? "1" : lr_match.captured(2);
    return QString("lr:%1").arg(chapter);
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

void stage_bazel_gst_plugins(QProcessEnvironment& env, const QString& working_dir) {
  const QDir root(QDir(working_dir).filePath("bazel-bin/src/gst-plugins"));
  if (!root.exists()) {
    return;
  }

  const QString arch =
      QSysInfo::currentCpuArchitecture().isEmpty() ? QString("unknown") : QSysInfo::currentCpuArchitecture();
  QDir runtime_dir(QDir(working_dir).filePath(QString(".cache/gst-plugin-path/%1").arg(arch)));
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

  QFileInfo bazel_bin_info(QDir(working_dir).filePath("bazel-bin"));
  QString bazel_bin_path = bazel_bin_info.canonicalFilePath();
  if (bazel_bin_path.isEmpty()) {
    bazel_bin_path = bazel_bin_info.absoluteFilePath();
  }
  const QDir bazel_bin_dir(bazel_bin_path);
  const QFileInfoList solib_roots =
      bazel_bin_dir.entryInfoList(QStringList() << "_solib_*", QDir::Dirs | QDir::NoDotAndDotDot);
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

  QDir runtime_lib_dir(QDir(working_dir).filePath(QString(".cache/runtime-lib-path/%1").arg(arch)));
  if (runtime_lib_dir.mkpath(".")) {
    for (const QFileInfo& solib_root : solib_roots) {
      QDirIterator onnxruntime_it(
          solib_root.absoluteFilePath(), QStringList("libonnxruntime.so.1"), QDir::Files, QDirIterator::Subdirectories);
      if (!onnxruntime_it.hasNext())
        continue;
      const QFileInfo onnxruntime(onnxruntime_it.next());
      const QString link_path = runtime_lib_dir.filePath("libonnxruntime.so.1");
      QFile::remove(link_path);
      if (QFile::link(onnxruntime.canonicalFilePath(), link_path))
        prepend_env_path(env, "LD_LIBRARY_PATH", runtime_lib_dir.absolutePath());
      break;
    }
  }

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

void configure_pipeline_runtime_environment(QProcessEnvironment& env, const QString& working_dir) {
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
  QDir registry_dir(QDir(working_dir).filePath(".cache/gstreamer-1.0"));
  if (!registry_dir.mkpath(".")) {
    registry_dir = QDir(QDir::home().filePath(".cache/gstreamer-1.0"));
  }
  if (registry_dir.mkpath(".")) {
    const QString arch =
        QSysInfo::currentCpuArchitecture().isEmpty() ? QString("unknown") : QSysInfo::currentCpuArchitecture();
    env.insert("GST_REGISTRY", registry_dir.filePath(QString("registry.hstream.native-onnx-v1.%1.bin").arg(arch)));
  }

  prepend_env_path(env, "GST_PLUGIN_PATH", QDir(working_dir).filePath("lib/gst-plugins"));
  prepend_env_path(env, "GST_PLUGIN_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  prepend_env_path(env, "LD_LIBRARY_PATH", QDir(working_dir).filePath("lib"));
  prepend_env_path(env, "LD_LIBRARY_PATH", QDir(working_dir).filePath("lib/gst-plugins"));
  prepend_env_path(env, "LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib");
  prepend_env_path(env, "LD_LIBRARY_PATH", "/opt/nvidia/deepstream/deepstream/lib/gst-plugins");
  stage_bazel_gst_plugins(env, working_dir);

  const QString yolo_so =
      QDir(working_dir).filePath("bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so");
  if (QFileInfo::exists(yolo_so)) {
    QDir lib_dir(QDir(working_dir).filePath("lib"));
    if (lib_dir.mkpath(".")) {
      const QString link_path = lib_dir.filePath("libnvdsinfer_custom_impl_Yolo.so");
      const QFileInfo link_info(link_path);
      if (!link_info.exists() || link_info.isSymLink()) {
        QFile::remove(link_path);
        QFile::link(QFileInfo(yolo_so).canonicalFilePath(), link_path);
      }
    }
  }
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
  const bool had_invalidation_id = calibration["invalidation_id"] && calibration["invalidation_id"].IsScalar();
  calibration["status"] = "pending";
  calibration["stale_from"] = stale_from;
  calibration["artifacts_invalidated"] = false;
  calibration.remove("invalidation_id");
  return previous_status != "pending" || previous_stale != stale_from || previous_invalidated || had_invalidation_id;
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

bool yaml_sequence_contains(YAML::Node node, const char* value) {
  if (!node || !node.IsSequence()) {
    return false;
  }
  for (const auto& item : node) {
    if (item.IsScalar() && item.as<std::string>() == value) {
      return true;
    }
  }
  return false;
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

bool hm::ui_internal::supports_x11_embedding(const QString& platform_name, bool tegra_runtime) {
  return !tegra_runtime && platform_name.compare("xcb", Qt::CaseInsensitive) == 0;
}

QString hm::ui_internal::preview_channel_for_tab(int tab_index, int camera_count) {
  if (tab_index == 0)
    return "program";
  if (tab_index == 1)
    return "stitched";
  const int source_index = tab_index - 2;
  return source_index >= 0 && source_index < camera_count ? QString("source%1").arg(source_index) : QString();
}

HStreamWindow::HStreamWindow(QWidget* parent) : QMainWindow(parent) {
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
  buildUi();
  refreshGames();
  updateRunControls();
  appendLog("hstream-ui started with hstream-cli runner backend");
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
  const auto it = camera_sliders_.find(id);
  return it == camera_sliders_.end() ? 0 : it->second->value();
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

  auto* content_splitter = new QSplitter(Qt::Vertical);
  content_splitter->setObjectName("mainLogSplitter");
  content_splitter->setChildrenCollapsible(false);

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

  content_splitter->addWidget(setup_panel_);
  content_splitter->addWidget(log_panel_);
  content_splitter->setStretchFactor(0, 4);
  content_splitter->setStretchFactor(1, 1);
  content_splitter->setSizes({680, 170});
  root->addWidget(content_splitter, 1);

  setCentralWidget(central);
}

void HStreamWindow::buildTopBar(QVBoxLayout* root) {
  auto* status_bar = new QHBoxLayout();
  status_bar->setSpacing(8);

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
    if (stitched_status_ && isCalibrationRun()) {
      stitched_status_->setText("Stitching calibration preview");
    }
  });

  control_points_spin_ = new QSpinBox();
  control_points_spin_->setObjectName("controlPointsSpin");
  control_points_spin_->setRange(20, 5000);
  control_points_spin_->setSingleStep(25);
  control_points_spin_->setValue(kDefaultStitchCalibrationControlPoints);
  control_points_spin_->setEnabled(true);
  control_points_spin_->setPrefix("CP ");
  control_points_spin_->setToolTip(
      "Control-point limit for stitching calibration. Changing this in Program mode recalibrates stitching before "
      "the full pipeline continues.");

  render_video_toggle_ = new QCheckBox("Render video");
  render_video_toggle_->setObjectName("renderVideoCheck");
  render_video_toggle_->setChecked(true);
  render_video_toggle_->setToolTip("Embed video in the active preview tab; disable before Play to reduce GPU work");
  connect(render_video_toggle_, &QCheckBox::toggled, this, [this](bool enabled) {
    appendLog(
        enabled ? "video rendering enabled for the next pipeline start"
                : "video rendering disabled for the next pipeline start");
  });

  start_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), "Play");
  start_button_->setObjectName("startPipelineButton");
  pause_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), "Pause");
  pause_button_->setObjectName("pausePipelineButton");
  auto* restart = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), "Restart Stage");
  restart->setObjectName("restartStageButton");
  auto* save = new QPushButton(style()->standardIcon(QStyle::SP_DialogSaveButton), "Save Preset");
  save->setObjectName("savePresetButton");
  auto* reset = new QPushButton("Reset Camera");
  reset->setObjectName("resetCameraButton");
  stop_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), "Stop");
  stop_button_->setObjectName("stopPipelineButton");

  connect(start_button_, &QPushButton::clicked, this, [this]() { startPipeline(); });
  connect(pause_button_, &QPushButton::clicked, this, [this]() { pauseOrResumePipeline(); });
  connect(stop_button_, &QPushButton::clicked, this, [this]() { stopPipeline(); });
  connect(restart, &QPushButton::clicked, this, [this]() { restartStage(); });
  connect(save, &QPushButton::clicked, this, [this]() { savePreset(); });
  connect(reset, &QPushButton::clicked, this, [this]() { resetCameraControls(); });

  status_bar->addWidget(title);
  status_bar->addSpacing(16);
  status_bar->addWidget(new QLabel("Pipeline:"));
  status_bar->addWidget(pipeline_state_);
  status_bar->addWidget(backend_mode_);
  status_bar->addStretch(1);
  status_bar->addWidget(run_mode_selector_);
  status_bar->addWidget(control_points_spin_);
  status_bar->addWidget(render_video_toggle_);

  auto* action_bar = new QHBoxLayout();
  action_bar->setSpacing(8);
  action_bar->addStretch(1);
  action_bar->addWidget(start_button_);
  action_bar->addWidget(pause_button_);
  action_bar->addWidget(restart);
  action_bar->addWidget(save);
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

  auto* setup_preview_splitter = new QSplitter(Qt::Vertical);
  setup_preview_splitter->setObjectName("setupPreviewSplitter");
  setup_preview_splitter->setChildrenCollapsible(true);
  setup_preview_splitter->addWidget(setup_row);
  auto* preview_container = new QWidget();
  auto* preview_layout = new QVBoxLayout(preview_container);
  preview_layout->setContentsMargins(0, 0, 0, 0);
  buildPreviewPane(preview_layout);
  setup_preview_splitter->addWidget(preview_container);
  setup_preview_splitter->setStretchFactor(0, 0);
  setup_preview_splitter->setStretchFactor(1, 1);
  setup_preview_splitter->setCollapsible(0, true);
  setup_preview_splitter->setCollapsible(1, false);
  setup_preview_splitter->setSizes({240, 440});
  root->addWidget(setup_preview_splitter, 1);
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
  layout->addWidget(program_controls);

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
  stitched_layout->addWidget(stitched_controls);

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
  connect(preview_tabs_, &QTabWidget::currentChanged, this, [this](int tab_index) {
    if (preview_focus_mode_) {
      focused_preview_tab_ = tab_index;
      for (size_t index = 0; index < preview_hosts_.size(); ++index) {
        auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[index]);
        if (host)
          host->setFocused(static_cast<int>(index) == tab_index);
      }
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

void HStreamWindow::buildCameraControls(QVBoxLayout* parent, bool program_stage) {
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

  auto add_slider_tab = [this](const std::vector<CameraSliderSpec>& specs) {
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
    content_layout->addStretch(1);
    scroll->setWidget(content);
    page_layout->addWidget(scroll, 1);
    return page;
  };

  const std::vector<CameraSliderSpec> tracking_controls = {
      {"Stop_Direction_Change_Delay_Frames", "Stop direction-change delay frames", 0, 60, 0},
      {"Cancel_Stop_On_Opposite_Direction", "Cancel stop on opposite direction", 0, 1, 0},
      {"Stop_Cancel_Hysteresis_Frames", "Stop cancel hysteresis frames", 0, 10, 0},
      {"Stop_Delay_Cooldown_Frames", "Stop-delay cooldown frames", 0, 30, 0},
      {"Time_To_Dest_Speed_Limit_Frames", "Time-to-destination speed limit frames", 0, 120, 10},
      {"Apply_To_Fast_Box", "Apply to fast box", 0, 1, 0},
      {"Apply_To_Follower_Box", "Apply to follower box", 0, 1, 1},
  };
  const std::vector<CameraSliderSpec> motion_controls = {
      {"Overshoot_Stop_Delay_Frames", "Overshoot stop-delay frames", 0, 60, 0},
      {"Post_Nonstop_Stop_Delay_Frames", "Post-nonstop stop-delay frames", 0, 60, 0},
      {"Overshoot_Speed_Ratio_x100", "Overshoot speed ratio x100", 0, 200, 70},
      {"Max_Speed_X_x10", "Max speed X override x10 (0 = configured)", 0, 2000, 0},
      {"Max_Speed_Y_x10", "Max speed Y override x10 (0 = configured)", 0, 2000, 0},
      {"Max_Accel_X_x10", "Max accel X override x10 (0 = configured)", 0, 1000, 0},
      {"Max_Accel_Y_x10", "Max accel Y override x10 (0 = configured)", 0, 1000, 0},
  };
  const std::vector<CameraSliderSpec> stitch_controls = {
      {"Stitch_Rotate_Degrees", "Stitch rotate degrees", 0, 180, 90},
      {"Link_Fixed_Edge_Rotation_Left_Right", "Link left/right fixed-edge rotation", 0, 1, 1},
      {"Left_Fixed_Edge_Rotation_Angle_x10",
       "Left fixed-edge rotation angle x10",
       0,
       kFixedEdgeRotationMaximumX10,
       kFixedEdgeRotationDefaultX10},
      {"Right_Fixed_Edge_Rotation_Angle_x10",
       "Right fixed-edge rotation angle x10",
       0,
       kFixedEdgeRotationMaximumX10,
       kFixedEdgeRotationDefaultX10},
  };

  if (program_stage) {
    control_tabs->addTab(add_slider_tab(tracking_controls), "Tracking");
    control_tabs->addTab(add_slider_tab(motion_controls), "Motion");
    const std::vector<CameraSliderSpec> crop_controls(stitch_controls.begin() + 1, stitch_controls.end());
    control_tabs->addTab(add_slider_tab(crop_controls), "Crop Rotation");
  } else {
    const std::vector<CameraSliderSpec> rotation_controls = {stitch_controls.front()};
    control_tabs->addTab(add_slider_tab(rotation_controls), "Rotation");
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
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    return QDir(development_root).filePath("bazel-bin/src/apps/pipeline-app/hstream-cli");
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
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    const QString development_config = QDir(QDir(development_root).filePath("configs")).filePath(config_name);
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
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    return development_root;
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
    const int current_control_points = calibration["control_points"] && calibration["control_points"].IsScalar()
        ? calibration["control_points"].as<int>()
        : -1;
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
    const bool expected_invalidated = status != "pending";
    if (current_control_points != control_points || current_status != "pending" || current_stale != stale_from ||
        current_invalidation_id != expected_invalidation_id || current_invalidated != expected_invalidated) {
      appendLog(
          QString("stitching calibration state transition to %1 skipped because dependency state changed concurrently")
              .arg(status));
      return true;
    }
  }

  calibration["control_points"] = control_points;
  calibration["status"] = status.toStdString();
  if (!expected_invalidation_id.isEmpty())
    calibration["invalidation_id"] = expected_invalidation_id.toStdString();
  if (status == "complete") {
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
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
  const fs::path config_path = fs::path(gameDirectory(active_run_game_id_).toStdString()) / "config.yaml";
  bool saved_found = false;
  int saved_control_points = 0;
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
    const bool needs_calibration = active_force_reconfigure_ || control_points_changed || saved_status != "complete";
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
    if (!calibration_stage_index(stale_from).has_value())
      stale_from = "input";
    const size_t features_index = *calibration_stage_index("features");
    if (control_points_changed && saved_status == "complete") {
      stale_from = "features";
    } else if (control_points_changed && saved_found && features_index < *calibration_stage_index(stale_from)) {
      stale_from = "features";
    }
    if (active_force_reconfigure_)
      stale_from = "input";
    active_calibration_start_stage_ = stale_from;

    clean_from_control_points = !active_force_reconfigure_ && stale_from == "features" &&
        (control_points_changed || !saved_artifacts_invalidated);
    clean_all = active_force_reconfigure_ ||
        (stale_from != "features" && (!saved_artifacts_invalidated || control_points_changed));

    active_calibration_invalidation_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points;
    calibration["status"] = "pending";
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
    calibration_ok_button_ = new QPushButton("OK", dialog);
    calibration_ok_button_->setObjectName("stitchCalibrationOkButton");
    calibration_ok_button_->setDefault(true);
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
  for (const auto& [id, label] : calibration_stage_labels_) {
    (void)id;
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
    calibration["status"] = "pending";
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
  if (!calibration_pending_ && !beginObservedStitchingCalibration(stage))
    return;
  if (status == "started" && calibration_dialog_)
    calibration_dialog_->show();
  if (stage == "calibration") {
    if (status == "complete")
      completeStitchingCalibration();
    else if (status == "failed")
      failStitchingCalibration(message.isEmpty() ? "The native stitching calibration failed." : message);
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
  calibration_pending_ = false;
  for (const CalibrationStageSpec& spec : kCalibrationStages)
    setStitchingCalibrationStage(QString::fromLatin1(spec.id), "complete", {});
  if (calibration_icon_) {
    calibration_icon_->setPixmap(style()->standardIcon(QStyle::SP_DialogApplyButton).pixmap(32, 32));
    calibration_icon_->setProperty("calibrationState", "complete");
  }
  if (calibration_headline_) {
    calibration_headline_->setText("Stitching calibration complete");
    calibration_headline_->setProperty("calibrationState", "complete");
  }
  if (calibration_detail_)
    calibration_detail_->setText("The stitched panorama and ice-surface calibration are ready.");
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
  const bool embed_render_window = render_video &&
      (hm::ui_internal::supports_x11_embedding(QGuiApplication::platformName(), is_tegra_runtime()) ||
       test_embedded_preview);
  QStringList args;
  args << "-g" << game_id << "--enable-sources=URI-MULTIPLE";
  if (active_force_reconfigure_)
    args << "--force-reconfigure";
  if (!active_calibration_invalidation_id_.isEmpty())
    args << QString("--clean-expected-invalidation-id=%1").arg(active_calibration_invalidation_id_);
  if (isCalibrationRun()) {
    args << "-c" << pipelineConfigPath("ds_hockey_app_config.yaml");
    args << QString("--enable-sinks=%1").arg(render_video ? "RENDER" : "FAKE");
    if (render_video) {
      args << "--show";
    }
  } else {
    args << "-c" << pipelineConfigPath("ds_hockey_app_config.yaml");
    args << QString("--enable-sinks=%1").arg(enabledSinkNames().join(","));
    if (render_video) {
      args << "--show";
    }
  }
  if (isCalibrationRun() || calibration_pending_) {
    args << QString("--options=%1").arg(kStitchedPreviewPipelineOptions);
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
    const bool have_required_windows =
        add_window("program", preview_render_target_) && add_window("stitched", stitched_render_target_);
    for (int camera_index = 0; camera_index < static_cast<int>(camera_preview_render_targets_.size()); ++camera_index) {
      add_window(QString("source%1").arg(camera_index), camera_preview_render_targets_[camera_index]);
    }
    if (have_required_windows) {
      const QString initial_channel = preview_tabs_
          ? hm::ui_internal::preview_channel_for_tab(
                preview_tabs_->currentIndex(), static_cast<int>(camera_preview_render_targets_.size()))
          : QString("program");
      args << QString("--ui-preview-windows=%1").arg(preview_windows.join(','));
      args << QString("--ui-preview-active=%1").arg(initial_channel.isEmpty() ? "program" : initial_channel);
    }
  }
  args << "--options=pipeline.hmaudio.enable=1";
  return args;
}

void HStreamWindow::startPipeline() {
  if (!pipeline_process_ || pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("pipeline already running");
    return;
  }
  clearPreviewFrames();
  if (!ensureGameDirectory()) {
    updateRunControls();
    return;
  }
  active_run_game_id_ = game_id_edit_->text().trimmed();
  active_run_is_calibration_ = isCalibrationRun();
  active_calibration_control_points_ = 0;
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
    appendLog(QString("pipeline process error=missing runner %1").arg(runner));
    updateRunControls();
    return;
  }
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString working_dir = pipelineWorkingDirectory();
  configure_pipeline_runtime_environment(env, working_dir);
  active_calibration_control_points_ = stitchingCalibrationControlPoints();
  bool calibration_required = false;
  if (!prepareStitchingCalibrationRun(runner, working_dir, env, &calibration_required)) {
    showStitchingCalibrationDialog();
    failStitchingCalibration("Could not prepare the game for stitching calibration.");
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    active_calibration_control_points_ = 0;
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Stitching setup failed");
    if (stitched_status_)
      stitched_status_->setText("Stitched canvas preview");
    updateRunControls();
    return;
  }
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
  pending_preview_channel_ = initial_preview_channel.isEmpty() ? QString("program") : initial_preview_channel;
  pending_preview_generation_ = preview_generation_;
  if (!setupPretrainedAssets(args)) {
    if (calibration_pending_)
      failStitchingCalibration("The pretrained calibration assets could not be prepared.");
    calibration_pending_ = false;
    active_run_game_id_.clear();
    active_run_is_calibration_ = false;
    active_calibration_control_points_ = 0;
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Asset setup failed");
    if (stitched_status_)
      stitched_status_->setText("Stitched canvas preview");
    updateRunControls();
    return;
  }
  logMissingTensorRtEngineCaches(args);

  const bool embedded_render = std::any_of(
      args.begin(), args.end(), [](const QString& argument) { return argument.startsWith("--ui-preview-windows="); });
  pipeline_render_embedded_ = embedded_render;
  const bool render_video = !render_video_toggle_ || render_video_toggle_->isChecked();
  if (preview_surface_)
    preview_surface_->setVisible(embedded_render);
  if (preview_render_target_)
    preview_render_target_->setVisible(embedded_render);
  if (stitched_surface_)
    stitched_surface_->setVisible(embedded_render);
  if (stitched_render_target_)
    stitched_render_target_->setVisible(embedded_render);
  for (QWidget* surface : camera_preview_surfaces_) {
    if (surface)
      surface->setVisible(embedded_render);
  }
  for (QWidget* target : camera_preview_render_targets_) {
    if (target)
      target->setVisible(embedded_render);
  }
  if (preview_external_notice_)
    preview_external_notice_->setVisible(!embedded_render);
  if (stitched_external_notice_)
    stitched_external_notice_->setVisible(!embedded_render);
  for (QLabel* notice : camera_preview_notices_) {
    if (notice)
      notice->setVisible(!embedded_render);
  }
  const QString render_notice =
      render_video ? "Video is displayed in a separate DeepStream window" : "Video rendering is disabled for this run";
  if (preview_external_notice_)
    preview_external_notice_->setText(render_notice);
  if (stitched_external_notice_)
    stitched_external_notice_->setText(render_notice);
  const QString camera_render_notice =
      render_video ? "Camera preview requires embedded X11 rendering" : "Video rendering is disabled for this run";
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
    env.insert("HM_MAX_CONTROL_POINTS", QString::number(control_points));
    env.insert("HSTREAM_CALIBRATION_PENDING", "1");
    env.insert("HSTREAM_CALIBRATION_START_STAGE", active_calibration_start_stage_);
    if (active_run_is_calibration_) {
      appendLog(
          QString("stitching calibration control points=%1; starting one-pass stitched playback").arg(control_points));
    } else {
      appendLog(QString(
                    "video inputs require stitching calibration; starting one-pass program playback with control "
                    "points=%1")
                    .arg(control_points));
    }
  } else if (active_run_is_calibration_) {
    appendLog(
        render_video ? "stitching calibration is complete; starting continuous stitched preview"
                     : "stitching calibration is complete; starting without video rendering");
  }
  if (!active_calibration_invalidation_id_.isEmpty())
    env.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", active_calibration_invalidation_id_);
  appendLog("audio enabled via pipeline.hmaudio.enable=1; render audio uses the configured system audio sink");
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

  pipeline_state_->setText("STARTING");
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
  updateRunControls();
}

void HStreamWindow::handlePipelineFinished(int exit_code, QProcess::ExitStatus exit_status) {
  ++scheduled_rotation_control_generation_;
  scheduled_rotation_controls_.clear();
  scheduled_rotation_controls_ready_ = false;
  ++scheduled_playtracker_control_generation_;
  scheduled_playtracker_controls_.clear();
  scheduled_playtracker_controls_ready_ = false;
  publishing_playtracker_controls_.reset();
  scheduled_playtracker_force_all_targets_ = false;
  publishing_playtracker_force_all_targets_ = false;
  readPipelineOutput();
  if (!pipeline_stdout_buffer_.isEmpty()) {
    appendLog(pipeline_stdout_buffer_.trimmed());
    pipeline_stdout_buffer_.clear();
  }
  if (!pipeline_stderr_buffer_.isEmpty()) {
    appendLog(pipeline_stderr_buffer_.trimmed());
    pipeline_stderr_buffer_.clear();
  }
  pipeline_paused_ = false;
  pipeline_uses_process_group_ = false;
  pipeline_render_embedded_ = false;
  if (scoreboard_selection_dialog_)
    scoreboard_selection_dialog_->closeAfterBackendCompletion();
  clearPreviewFrames();
  const bool stopped_by_user = pipeline_stop_requested_;
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
  active_run_game_id_.clear();
  active_run_is_calibration_ = false;
  active_calibration_control_points_ = 0;
  active_calibration_start_stage_.clear();
  active_calibration_invalidation_id_.clear();
  active_force_reconfigure_ = false;
  pipeline_state_->setText("STOPPED");
  preview_status_->setText("Pipeline stopped");
  if (stitched_status_)
    stitched_status_->setText("Stitched canvas preview");
  appendLog(QString("pipeline finished exit=%1 status=%2")
                .arg(exit_code)
                .arg(exit_status == QProcess::NormalExit ? "normal" : "crashed"));
  updateRunControls();
}

void HStreamWindow::clearPreviewFrames() {
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
  preview_recovery_attempts_ = 0;
  preview_runtime_ready_ = false;
}

void HStreamWindow::handlePipelineError(QProcess::ProcessError error) {
  ++scheduled_rotation_control_generation_;
  scheduled_rotation_controls_.clear();
  scheduled_rotation_controls_ready_ = false;
  ++scheduled_playtracker_control_generation_;
  scheduled_playtracker_controls_.clear();
  scheduled_playtracker_controls_ready_ = false;
  publishing_playtracker_controls_.reset();
  scheduled_playtracker_force_all_targets_ = false;
  publishing_playtracker_force_all_targets_ = false;
  const QString error_message = QString("pipeline process error=%1 message=%2")
                                    .arg(static_cast<int>(error))
                                    .arg(pipeline_process_ ? pipeline_process_->errorString() : QString());
  if (error != QProcess::FailedToStart && error != QProcess::Crashed) {
    if (error == QProcess::WriteError || error == QProcess::ReadError) {
      failPendingRuntimeControls(error == QProcess::WriteError ? "pipeline-write-error" : "pipeline-read-error");
    }
    appendLog(error_message + "; pipeline remains running");
    updateRunControls();
    return;
  }
  if (pipeline_stop_requested_) {
    appendLog(error_message + "; pipeline is stopping at the user's request");
    return;
  }
  if (calibration_pending_ && !calibration_dialog_failed_)
    failStitchingCalibration(QString("The calibration process could not continue: %1").arg(error_message));
  pipeline_paused_ = false;
  pipeline_uses_process_group_ = false;
  pipeline_render_embedded_ = false;
  pipeline_stop_requested_ = false;
  clearPreviewFrames();
  if (scoreboard_selection_dialog_)
    scoreboard_selection_dialog_->closeAfterBackendCompletion();
  failPendingRuntimeControls("pipeline-error");
  calibration_pending_ = false;
  active_run_game_id_.clear();
  active_run_is_calibration_ = false;
  active_calibration_control_points_ = 0;
  active_calibration_start_stage_.clear();
  active_calibration_invalidation_id_.clear();
  active_force_reconfigure_ = false;
  pipeline_state_->setText("STOPPED");
  preview_status_->setText("Pipeline failed to start");
  if (stitched_status_)
    stitched_status_->setText("Stitched canvas preview");
  appendLog(error_message);
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
        if (handleGpuPreviewStatus(trimmed)) {
          continue;
        }
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
    const QString selected_channel = selectedPipelinePreviewChannel();
    if (selected_channel.isEmpty() || selected_channel == backend_channel) {
      pending_preview_channel_ = backend_channel;
      pending_preview_generation_ = generation;
      preview_recovery_attempts_ = 0;
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
    if (preview_frame_channels_received_.insert(channel).second)
      appendLog(QString("GPU preview ready channel=%1 generation=%2").arg(channel).arg(generation));
  } else if (status == "failed" || status == "unavailable") {
    const bool affected_active = channel == active_preview_channel_;
    if (matches_pending) {
      pending_preview_channel_.clear();
      pending_preview_generation_ = 0;
      preview_recovery_attempts_ = 0;
    }
    if (affected_active)
      active_preview_channel_.clear();
    if (target)
      target->hide();
    if (surface)
      surface->hide();
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
    if (channel == active_preview_channel_)
      active_preview_channel_.clear();
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

void HStreamWindow::switchPipelineRenderTarget(int tab_index) {
  if (!pipeline_render_embedded_ || !pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning ||
      tab_index < 0) {
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
  if (reason != PreviewRequestReason::kRecovery)
    preview_recovery_attempts_ = 0;
  if (QWidget* surface = previewSurfaceForChannel(channel)) {
    surface->setProperty("previewRendererState", "activating");
    surface->show();
  }
  if (QWidget* target = previewTargetForChannel(channel)) {
    target->setProperty("previewRendererState", "activating");
    target->show();
  }
  appendLog(QString("GPU preview requested channel=%1 generation=%2 reason=%3")
                .arg(channel)
                .arg(generation)
                .arg(
                    reason == PreviewRequestReason::kStartup
                        ? "startup"
                        : (reason == PreviewRequestReason::kRecovery ? "recovery" : "tab-change")));
  schedulePreviewReadyTimeout(channel, generation, previewReadyTimeoutMs());
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

void HStreamWindow::togglePreviewFocus(int tab_index) {
  const bool restore = preview_focus_mode_ && focused_preview_tab_ == tab_index;
  setPreviewFocusMode(!restore, tab_index);
}

void HStreamWindow::setPreviewFocusMode(bool focused, int tab_index) {
  if (!preview_tabs_ || tab_index < 0 || tab_index >= preview_tabs_->count())
    return;
  preview_tabs_->setCurrentIndex(tab_index);
  preview_focus_mode_ = focused;
  focused_preview_tab_ = focused ? tab_index : -1;
  if (top_bar_)
    top_bar_->setVisible(!focused);
  if (log_panel_)
    log_panel_->setVisible(!focused);
  if (setup_panel_) {
    if (QWidget* setup_row = setup_panel_->findChild<QWidget*>("setupControlsRow"))
      setup_row->setVisible(!focused);
  }
  preview_tabs_->tabBar()->setVisible(!focused);
  for (int page_index = 0; page_index < preview_tabs_->count(); ++page_index) {
    QWidget* page = preview_tabs_->widget(page_index);
    QWidget* host = page_index < static_cast<int>(preview_hosts_.size()) ? preview_hosts_[page_index] : nullptr;
    for (QWidget* child : page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
      if (child != host)
        child->setVisible(!focused);
    }
  }
  for (size_t index = 0; index < preview_hosts_.size(); ++index) {
    auto* host = static_cast<LetterboxRenderHost*>(preview_hosts_[index]);
    if (host)
      host->setFocused(focused && static_cast<int>(index) == tab_index);
  }
  appendLog(focused ? QString("preview focus mode tab=%1").arg(tab_index) : "preview restored to normal layout");
}

void HStreamWindow::updateRunControls() {
  const bool running = pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning;
  if (!pipeline_state_) {
    return;
  }
  if (!running && pipeline_state_->text().isEmpty()) {
    pipeline_state_->setText("STOPPED");
  }
  if (start_button_) {
    start_button_->setEnabled(!running);
  }
  if (pause_button_) {
    pause_button_->setEnabled(running);
    pause_button_->setText(pipeline_paused_ ? "Resume" : "Pause");
  }
  if (stop_button_) {
    stop_button_->setEnabled(running);
  }
  if (run_mode_selector_) {
    run_mode_selector_->setEnabled(!running);
  }
  if (control_points_spin_) {
    control_points_spin_->setEnabled(!running);
  }
  if (render_video_toggle_) {
    render_video_toggle_->setEnabled(!running);
  }
  if (game_controls_) {
    game_controls_->setEnabled(!running);
  }
  if (video_controls_) {
    video_controls_->setEnabled(!running);
  }
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

  bool invalidate_rink_masks = false;
  int invalidated_config_artifacts = 0;
  if (!applySavedControlConfig(config, &invalidate_rink_masks, &invalidated_config_artifacts)) {
    return;
  }
  absl::Status publish;
  size_t invalidated_masks = 0;
  if (invalidate_rink_masks) {
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
  if (!publish.ok()) {
    appendLog(QString("failed to write preset %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
    return;
  }
  if (invalidate_rink_masks) {
    appendLog(QString("stitch rotation saved; invalidated %1 scoreboard/ice-mask artifact(s)")
                  .arg(invalidated_config_artifacts + static_cast<int>(invalidated_masks)));
  }
  appendLog(QString("preset saved %1").arg(QString::fromStdString(config_path.string())));
}

void HStreamWindow::resetCameraControls() {
  for (const auto& [id, value] : camera_defaults_) {
    const auto it = camera_sliders_.find(id);
    if (it != camera_sliders_.end()) {
      it->second->setValue(value);
    }
  }
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    // Reset every runtime-tunable field on both boxes, including a box that
    // was tuned before its target selector was returned to the default.
    const QStringList playtracker_reset_controls = {
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
}

void HStreamWindow::loadSavedControlConfig() {
  if (!game_id_edit_ || game_id_edit_->text().isEmpty()) {
    return;
  }
  if (control_points_spin_) {
    const bool blocked = control_points_spin_->blockSignals(true);
    control_points_spin_->setValue(kDefaultStitchCalibrationControlPoints);
    control_points_spin_->blockSignals(blocked);
  }
  for (const auto& [id, value] : camera_defaults_) {
    const auto slider_it = camera_sliders_.find(id);
    if (slider_it == camera_sliders_.end()) {
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
    return;
  }
  if (!loaded_config->has_value()) {
    return;
  }
  try {
    YAML::Node config = **loaded_config;
    auto set_control_without_signal = [this](const QString& id, int value) {
      const auto slider_it = camera_sliders_.find(id);
      if (slider_it == camera_sliders_.end()) {
        return false;
      }
      const bool blocked = slider_it->second->blockSignals(true);
      slider_it->second->setValue(value);
      slider_it->second->blockSignals(blocked);
      const int applied_value = slider_it->second->value();
      const auto label_it = camera_value_labels_.find(id);
      if (label_it != camera_value_labels_.end()) {
        label_it->second->setText(QString::number(applied_value));
      }
      return true;
    };
    YAML::Node control_points;
    if (control_points_spin_ &&
        lookup_yaml_path(config, "hstream_ui.stitching_calibration.control_points", &control_points) &&
        control_points.IsScalar()) {
      const bool blocked = control_points_spin_->blockSignals(true);
      control_points_spin_->setValue(control_points.as<int>());
      control_points_spin_->blockSignals(blocked);
    }
    YAML::Node fixed_edge_rotation;
    if (lookup_yaml_path(config, "rink.camera.fixed_edge_rotation_angle", &fixed_edge_rotation)) {
      auto angle_x10 = [](const YAML::Node& value) { return static_cast<int>(std::lround(value.as<double>() * 10.0)); };
      if (fixed_edge_rotation.IsSequence() && fixed_edge_rotation.size() == 2) {
        set_control_without_signal("Link_Fixed_Edge_Rotation_Left_Right", 0);
        set_control_without_signal("Left_Fixed_Edge_Rotation_Angle_x10", angle_x10(fixed_edge_rotation[0]));
        set_control_without_signal("Right_Fixed_Edge_Rotation_Angle_x10", angle_x10(fixed_edge_rotation[1]));
      } else if (fixed_edge_rotation.IsScalar()) {
        const int value = angle_x10(fixed_edge_rotation);
        set_control_without_signal("Link_Fixed_Edge_Rotation_Left_Right", 1);
        set_control_without_signal("Left_Fixed_Edge_Rotation_Angle_x10", value);
        set_control_without_signal("Right_Fixed_Edge_Rotation_Angle_x10", value);
      } else {
        appendLog("ignored invalid rink.camera.fixed_edge_rotation_angle; expected one value or [left, right]");
      }
    }
    YAML::Node controls = config["hstream_ui"]["camera_controls"];
    int loaded = 0;
    if (controls && controls.IsMap()) {
      for (const auto& entry : controls) {
        const QString id = QString::fromStdString(entry.first.as<std::string>());
        if (set_control_without_signal(id, entry.second.as<int>())) {
          ++loaded;
        }
      }
    }
    if (cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") != 0) {
      set_control_without_signal(
          "Right_Fixed_Edge_Rotation_Angle_x10", cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10"));
    }
    appendLog(QString("loaded %1 saved camera controls").arg(loaded));
  } catch (const std::exception& exc) {
    appendLog(QString("could not load saved camera controls: %1").arg(exc.what()));
  }
}

bool HStreamWindow::applySavedControlConfig(
    YAML::Node& config,
    bool* invalidate_rink_masks,
    int* invalidated_config_artifacts) {
  if (invalidate_rink_masks) {
    *invalidate_rink_masks = false;
  }
  if (invalidated_config_artifacts) {
    *invalidated_config_artifacts = 0;
  }
  if (!yaml_defined(config) || config.IsNull()) {
    config = YAML::Node(YAML::NodeType::Map);
  }
  YAML::Node previous_hstream_ui = map_value(config, "hstream_ui");
  YAML::Node previous_generated = map_value(previous_hstream_ui, "generated_runtime_keys");
  YAML::Node previous_generated_values = map_value(previous_hstream_ui, "generated_runtime_values");
  YAML::Node previous_playtracker_config_base = map_value(previous_hstream_ui, "playtracker_config_base");
  const bool ui_rotation_previously_generated =
      yaml_sequence_contains(previous_generated, "stitching.post_stitch_rotate_degrees");
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
  YAML::Node current_playtracker_config;
  if (previous_playtracker_config_base && previous_playtracker_config_base.IsScalar() &&
      !lookup_yaml_path(config, "pipeline.ds-playtracker.config-file", &current_playtracker_config)) {
    config["pipeline"]["ds-playtracker"]["config-file"] = previous_playtracker_config_base.as<std::string>();
  }
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
  for (const auto& [id, slider] : camera_sliders_) {
    const auto default_it = camera_defaults_.find(id);
    if (!slider || default_it == camera_defaults_.end() || slider->value() == default_it->second) {
      continue;
    }
    const std::string key = id.toStdString();
    controls[key.c_str()] = slider->value();
    ++changed;
  }
  config["hstream_ui"]["camera_controls"] = controls;

  auto slider_value = [this](const QString& id) -> int {
    const auto it = camera_sliders_.find(id);
    return it == camera_sliders_.end() ? 0 : it->second->value();
  };
  if (has_control(controls, "Stitch_Rotate_Degrees")) {
    config["stitching"]["post_stitch_rotate_degrees"] = 90 - slider_value("Stitch_Rotate_Degrees");
    mark_runtime_key("stitching.post_stitch_rotate_degrees");
  }
  const bool fixed_edge_rotation_changed = has_control(controls, "Link_Fixed_Edge_Rotation_Left_Right") ||
      has_control(controls, "Left_Fixed_Edge_Rotation_Angle_x10") ||
      has_control(controls, "Right_Fixed_Edge_Rotation_Angle_x10");
  if (fixed_edge_rotation_changed) {
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
  bool rotation_changed_for_artifacts = false;
  if (has_control(controls, "Stitch_Rotate_Degrees")) {
    YAML::Node previous_rotation_value;
    const bool previous_rotation_value_found =
        lookup_yaml_key(previous_generated_values, "stitching.post_stitch_rotate_degrees", &previous_rotation_value);
    const std::string current_rotation_dump = YAML::Dump(config["stitching"]["post_stitch_rotate_degrees"]);
    rotation_changed_for_artifacts = !ui_rotation_previously_generated || !previous_rotation_value_found ||
        !previous_rotation_value.IsScalar() || previous_rotation_value.as<std::string>() != current_rotation_dump;
  } else if (ui_rotation_previously_generated) {
    rotation_changed_for_artifacts = true;
  }
  if (rotation_changed_for_artifacts) {
    const ArtifactInvalidationResult invalidation = invalidate_rotation_dependent_artifacts(config);
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
  const bool has_playtracker_runtime_controls = has_control(controls, "Stop_Direction_Change_Delay_Frames") ||
      has_control(controls, "Cancel_Stop_On_Opposite_Direction") ||
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
    } else {
      const QString runtime_config_path = runtime_dir.filePath("play_tracker_config.yaml");
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
            if (same_file_path(candidate, runtime_config_path)) {
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
        while (live_boxes.size() < 2) {
          YAML::Node box(YAML::NodeType::Map);
          box["name"] = live_boxes.size() == 0 ? "current_roi" : "current_roi_aspect";
          live_boxes.push_back(box);
        }

        YAML::Node play_tracker = play_tracker_config["play-tracker"];
        if (has_control(controls, "Overshoot_Stop_Delay_Frames")) {
          play_tracker["overshoot-stop-delay-count"] = slider_value("Overshoot_Stop_Delay_Frames");
        }
        if (has_control(controls, "Overshoot_Speed_Ratio_x100")) {
          play_tracker["overshoot-scale-speed-ratio"] = ratio_x100(slider_value("Overshoot_Speed_Ratio_x100"));
        }

        auto apply_live_box = [&](int index) {
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
          apply_live_box(0);
        }
        if (slider_value("Apply_To_Follower_Box") != 0) {
          apply_live_box(1);
        }

        std::ofstream tracker_out(runtime_config_path.toStdString());
        if (!tracker_out) {
          appendLog(QString("could not open playtracker runtime config %1").arg(runtime_config_path));
        } else {
          tracker_out << play_tracker_config << "\n";
          tracker_out.close();
          if (!tracker_out) {
            appendLog(QString("could not write playtracker runtime config %1").arg(runtime_config_path));
          } else {
            config["pipeline"]["ds-playtracker"]["config-file"] = runtime_config_path.toStdString();
            if (!configured_playtracker_config.isEmpty() && configured_playtracker_config != runtime_config_path) {
              config["hstream_ui"]["playtracker_config_base"] = configured_playtracker_config.toStdString();
            }
            mark_runtime_key("pipeline.ds-playtracker.config-file");
            appendLog(QString("playtracker runtime config saved %1").arg(runtime_config_path));
          }
        }
      } catch (const std::exception& exc) {
        appendLog(QString("could not save playtracker runtime config: %1").arg(exc.what()));
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
    std::map<QString, QString> left_by_chapter;
    std::map<QString, QString> right_by_chapter;
    bool parsed = true;
    for (const auto& item : explicit_left) {
      const QString path = QString::fromStdString(item.as<std::string>());
      const std::optional<QString> chapter = explicit_chapter_key(path);
      if (!chapter || left_by_chapter.count(*chapter)) {
        parsed = false;
        break;
      }
      left_by_chapter[*chapter] = path;
    }
    for (const auto& item : explicit_right) {
      const QString path = QString::fromStdString(item.as<std::string>());
      const std::optional<QString> chapter = explicit_chapter_key(path);
      if (!chapter || right_by_chapter.count(*chapter)) {
        parsed = false;
        break;
      }
      right_by_chapter[*chapter] = path;
    }
    if (parsed && !left_by_chapter.empty() && left_by_chapter.size() == right_by_chapter.size()) {
      bool same_chapters = true;
      for (const auto& [chapter, _] : left_by_chapter) {
        if (!right_by_chapter.count(chapter)) {
          same_chapters = false;
          break;
        }
      }
      if (same_chapters) {
        YAML::Node left_list(YAML::NodeType::Sequence);
        YAML::Node right_list(YAML::NodeType::Sequence);
        for (const auto& [chapter, left_path] : left_by_chapter) {
          left_list.push_back(left_path.toStdString());
          right_list.push_back(right_by_chapter.at(chapter).toStdString());
        }
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
    if (explicit_left.size() == 1 && explicit_right.size() == 1) {
      const QString left_path = QString::fromStdString(explicit_left[0].as<std::string>());
      const QString right_path = QString::fromStdString(explicit_right[0].as<std::string>());
      if (!explicit_chapter_key(left_path) && !explicit_chapter_key(right_path)) {
        YAML::Node left_list(YAML::NodeType::Sequence);
        YAML::Node right_list(YAML::NodeType::Sequence);
        left_list.push_back(left_path.toStdString());
        right_list.push_back(right_path.toStdString());
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
    if (explicit_left.size() == explicit_right.size()) {
      YAML::Node left_list(YAML::NodeType::Sequence);
      YAML::Node right_list(YAML::NodeType::Sequence);
      bool all_unparseable = true;
      for (size_t i = 0; i < explicit_left.size(); ++i) {
        const QString left_path = QString::fromStdString(explicit_left[i].as<std::string>());
        const QString right_path = QString::fromStdString(explicit_right[i].as<std::string>());
        if (explicit_chapter_key(left_path) || explicit_chapter_key(right_path)) {
          all_unparseable = false;
          break;
        }
        left_list.push_back(left_path.toStdString());
        right_list.push_back(right_path.toStdString());
      }
      if (all_unparseable && explicit_left.size() > 0) {
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
  }

  changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
  changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
  if (has_left && has_right) {
    appendLog("explicit Left/Right runtime config will apply after both sides have matching chapter sets");
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
  output_states_[id]->setText(enabled ? "ENABLED" : "STOPPED");
  appendLog(QString("output route %1 %2").arg(id, enabled ? "enabled" : "disabled"));
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("output route change will apply on the next pipeline start with the current runner backend");
  }
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
  if (capture_complete_log_) {
    complete_log_ += QString("%1 %2\n").arg(timestamp(), message);
    if (complete_log_.size() > kMaxCapturedLogCharacters) {
      const qsizetype overflow = complete_log_.size() - kMaxCapturedLogCharacters;
      const qsizetype next_line = complete_log_.indexOf('\n', overflow);
      complete_log_.remove(0, next_line >= 0 ? next_line + 1 : overflow);
    }
  }
  const QString html =
      QString("<span style=\"color:#667085\">%1</span> %2").arg(timestamp().toHtmlEscaped(), ansi_to_html(message));
  log_->append(html);
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
          if (same_file_path(candidate, runtime_config_path) || same_file_path(candidate, persistent_runtime_config)) {
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
    while (live_boxes.size() < 2) {
      YAML::Node box(YAML::NodeType::Map);
      box["name"] = live_boxes.size() == 0 ? "current_roi" : "current_roi_aspect";
      live_boxes.push_back(box);
    }

    auto slider_value = [this](const QString& id) -> int {
      const auto it = camera_sliders_.find(id);
      return it == camera_sliders_.end() ? 0 : it->second->value();
    };
    auto slider_changed = [this, &slider_value](const QString& id) -> bool {
      const auto default_it = camera_defaults_.find(id);
      return default_it != camera_defaults_.end() && slider_value(id) != default_it->second;
    };
    auto apply_live_box = [&](int index) {
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
      apply_live_box(0);
    }
    if (slider_value("Apply_To_Follower_Box") != 0) {
      apply_live_box(1);
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

void HStreamWindow::flushScheduledRuntimeControls() {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning || !runtime_control_batches_.empty()) {
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
  slider->setRange(minimum, maximum);
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
        (scheduled_playtracker_controls_.count(id) && scheduled_playtracker_controls_.at(id) == new_value)) {
      // The scheduler already reported the coalesced live update.
    } else {
      appendLog(QString("camera control %1=%2 apply=save/restart").arg(id).arg(new_value));
    }
  });
  row->addWidget(name, 0, 0);
  row->addWidget(value_label, 0, 1);
  row->addWidget(slider, 1, 0, 1, 2);
  layout->addLayout(row);
  return slider;
}
