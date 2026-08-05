#include "src/apps/hmstream-ui/HmStreamWindow.h"

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
#include <QtCore/Qt>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextDocument>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>

#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kExposureEvSliderZero = 40;
constexpr int kDefaultStitchCalibrationControlPoints = 1500;

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
        candidate_root.filePath(QString("bazel-bin/src/apps/hmstream-ui/%1").arg(application_name));
    const QString candidate_path = QFileInfo(candidate_application).canonicalFilePath();
    const QString runner = candidate_root.filePath("bazel-bin/src/apps/pipeline-app/hmstream-cli");
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

class LetterboxRenderHost : public QWidget {
 public:
  explicit LetterboxRenderHost(double aspect_ratio, QWidget* parent = nullptr)
      : QWidget(parent), aspect_ratio_(aspect_ratio > 0.0 ? aspect_ratio : 16.0 / 9.0) {
    setObjectName("letterboxRenderHost");
    setMinimumHeight(420);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setAutoFillBackground(true);
  }

  QWidget* renderSurface() const {
    return render_surface_;
  }

 protected:
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
  }

 private:
  double aspect_ratio_;
  QWidget* render_surface_{new QWidget(this)};
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

void configure_pipeline_runtime_environment(QProcessEnvironment& env, const QString& working_dir) {
  // DeepStream 9.1's legacy nvstreammux rejects the native 8K source caps used
  // by stitching. Match run.sh while preserving an explicit diagnostic
  // override from the caller.
  if (env.value("USE_NEW_NVSTREAMMUX").isEmpty()) {
    env.insert("USE_NEW_NVSTREAMMUX", "yes");
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
  auto loaded_config = hm::stitching::load_game_config_file(config_path);
  if (loaded_config.ok() && loaded_config->has_value()) {
    try {
      YAML::Node config = **loaded_config;
      YAML::Node entries = config["hmstream_ui"]["auto_import_sources"];
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
  auto loaded_config = hm::stitching::load_game_config_file(config_path);
  if (!loaded_config.ok() || !loaded_config->has_value()) {
    return false;
  }
  try {
    YAML::Node config = **loaded_config;
    YAML::Node entries = config["hmstream_ui"]["auto_import_sources"];
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
  YAML::Node list = config["hmstream_ui"]["copied_imports"];
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

void remove_yaml_path_if_empty_map(YAML::Node root, std::initializer_list<const char*> path) {
  QStringList parts;
  for (const char* part : path) {
    parts.push_back(part);
  }
  YAML::Node value;
  if (lookup_yaml_path(root, parts.join('.'), &value) && value.IsMap() && value.size() == 0) {
    remove_yaml_path(root, path);
  }
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

double slider_to_exposure_ev(int position) {
  return static_cast<double>(std::max(0, std::min(80, position)) - 40) / 10.0;
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
      current["hmstream_ui"]["video_roles"], map_value(map_value(previous, "hmstream_ui"), "video_roles"), "left");
  restore_child(
      current["hmstream_ui"]["video_roles"], map_value(map_value(previous, "hmstream_ui"), "video_roles"), "center");
  restore_child(
      current["hmstream_ui"]["video_roles"], map_value(map_value(previous, "hmstream_ui"), "video_roles"), "right");
  restore_child(current["game"]["videos"], map_value(map_value(previous, "game"), "videos"), "left");
  restore_child(current["game"]["videos"], map_value(map_value(previous, "game"), "videos"), "right");
  restore_child(current["game"]["stitching"], map_value(map_value(previous, "game"), "stitching"), "frame_offsets");
  restore_child(current["stitching"], map_value(previous, "stitching"), "frame_offsets");
}

HmStreamWindow::HmStreamWindow(QWidget* parent) : QMainWindow(parent) {
  pipeline_process_ = new QProcess(this);
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
  appendLog("hmstream-ui started with hmstream-cli runner backend");
}

QString HmStreamWindow::pipelineStateText() const {
  return pipeline_state_ ? pipeline_state_->text() : QString();
}

QString HmStreamWindow::outputStateText(const QString& id) const {
  const auto it = output_states_.find(id);
  return it == output_states_.end() ? QString() : it->second->text();
}

QString HmStreamWindow::logText() const {
  return log_ ? log_->toPlainText() : QString();
}

QString HmStreamWindow::gameIdText() const {
  return game_id_edit_ ? game_id_edit_->text() : QString();
}

QString HmStreamWindow::gameDirectoryText() const {
  return game_path_label_ ? game_path_label_->text() : QString();
}

int HmStreamWindow::videoSetCount() const {
  return video_set_list_ ? video_set_list_->count() : 0;
}

int HmStreamWindow::cameraControlValue(const QString& id) const {
  const auto it = camera_sliders_.find(id);
  return it == camera_sliders_.end() ? 0 : it->second->value();
}

int HmStreamWindow::cameraTabCount() const {
  return camera_tabs_ ? camera_tabs_->count() : 0;
}

void HmStreamWindow::buildUi() {
  setObjectName("hmstreamUi");
  setWindowTitle("HMStream UI");
  resize(1440, 900);

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(10);

  buildTopBar(root);
  buildMainArea(root);
  buildLog(root);

  setCentralWidget(central);
}

void HmStreamWindow::buildTopBar(QVBoxLayout* root) {
  auto* bar = new QHBoxLayout();
  bar->setSpacing(8);

  auto* title = new QLabel("HMStream Runtime Control");
  title->setObjectName("titleLabel");
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 2);
  title->setFont(title_font);

  pipeline_state_ = make_value_label("pipelineStateLabel", "STOPPED");
  backend_mode_ = new QLabel("Backend: hmstream-cli");
  backend_mode_->setObjectName("backendModeLabel");

  run_mode_selector_ = new QComboBox();
  run_mode_selector_->setObjectName("runModeCombo");
  run_mode_selector_->addItem("Program", "program");
  run_mode_selector_->addItem("Stitching Calibration", "stitch-calibration");
  connect(run_mode_selector_, &QComboBox::currentIndexChanged, this, [this]() {
    if (control_points_spin_) {
      control_points_spin_->setEnabled(isCalibrationRun());
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
  control_points_spin_->setEnabled(false);
  control_points_spin_->setPrefix("CP ");

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

  bar->addWidget(title);
  bar->addSpacing(16);
  bar->addWidget(new QLabel("Pipeline:"));
  bar->addWidget(pipeline_state_);
  bar->addWidget(backend_mode_);
  bar->addWidget(run_mode_selector_);
  bar->addWidget(control_points_spin_);
  bar->addStretch(1);
  bar->addWidget(start_button_);
  bar->addWidget(pause_button_);
  bar->addWidget(restart);
  bar->addWidget(save);
  bar->addWidget(reset);
  bar->addWidget(stop_button_);
  root->addLayout(bar);
}

void HmStreamWindow::buildMainArea(QVBoxLayout* root) {
  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName("mainSplitter");

  auto* left = new QWidget();
  auto* left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 0, 0);
  buildGameControls(left_layout);
  buildPreviewPane(left_layout);

  auto* right = new QWidget();
  auto* right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(0, 0, 0, 0);
  buildOutputControls(right_layout);
  buildCameraControls(right_layout);

  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);
  root->addWidget(splitter, 1);
}

void HmStreamWindow::buildGameControls(QVBoxLayout* root) {
  auto* group = new QGroupBox("Game");
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
  video_set_list_->setMinimumHeight(92);

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

void HmStreamWindow::buildPreviewPane(QVBoxLayout* root) {
  preview_tabs_ = new QTabWidget();
  preview_tabs_->setObjectName("previewTabs");

  auto* program = new QWidget();
  auto* layout = new QVBoxLayout(program);
  auto* preview_host = new LetterboxRenderHost(16.0 / 9.0);
  preview_host->setObjectName("programLetterboxHost");
  preview_surface_ = preview_host->renderSurface();
  preview_surface_->setObjectName("previewSurface");
  preview_surface_->setAttribute(Qt::WA_NativeWindow);
  preview_surface_->setAttribute(Qt::WA_DontCreateNativeAncestors);
  preview_surface_->setStyleSheet("QWidget#previewSurface { background: #12171c; }");
  preview_external_notice_ = new QLabel("Video is displayed in a separate DeepStream window", preview_surface_);
  preview_external_notice_->setAlignment(Qt::AlignCenter);
  preview_external_notice_->setWordWrap(true);
  preview_external_notice_->setStyleSheet("color: #c9d1d9; padding: 24px;");
  preview_external_notice_->hide();
  auto* preview_surface_layout = new QVBoxLayout(preview_surface_);
  preview_surface_layout->addWidget(preview_external_notice_);

  preview_status_ = new QLabel("Pipeline stopped");
  preview_status_->setObjectName("previewStatusLabel");
  program_fullscreen_button_ = new QPushButton("Fullscreen");
  program_fullscreen_button_->setObjectName("programFullscreenButton");
  connect(program_fullscreen_button_, &QPushButton::clicked, this, [this]() { togglePreviewFullscreen(0); });
  auto* program_footer = new QHBoxLayout();
  program_footer->addWidget(preview_status_, 1);
  program_footer->addWidget(program_fullscreen_button_);
  layout->addWidget(preview_host, 1);
  layout->addLayout(program_footer);

  auto* stitched = new QWidget();
  auto* stitched_layout = new QVBoxLayout(stitched);
  auto* stitched_host = new LetterboxRenderHost(16.0 / 9.0);
  stitched_host->setObjectName("stitchedLetterboxHost");
  stitched_surface_ = stitched_host->renderSurface();
  stitched_surface_->setObjectName("stitchedPreviewSurface");
  stitched_surface_->setAttribute(Qt::WA_NativeWindow);
  stitched_surface_->setAttribute(Qt::WA_DontCreateNativeAncestors);
  stitched_surface_->setStyleSheet("QWidget#stitchedPreviewSurface { background: #10151a; }");
  stitched_external_notice_ = new QLabel("Video is displayed in a separate DeepStream window", stitched_surface_);
  stitched_external_notice_->setAlignment(Qt::AlignCenter);
  stitched_external_notice_->setWordWrap(true);
  stitched_external_notice_->setStyleSheet("color: #c9d1d9; padding: 24px;");
  stitched_external_notice_->hide();
  auto* stitched_surface_layout = new QVBoxLayout(stitched_surface_);
  stitched_surface_layout->addWidget(stitched_external_notice_);
  stitched_status_ = new QLabel("Stitched canvas preview");
  stitched_status_->setObjectName("stitchedPreviewStatusLabel");
  stitched_fullscreen_button_ = new QPushButton("Fullscreen");
  stitched_fullscreen_button_->setObjectName("stitchedFullscreenButton");
  connect(stitched_fullscreen_button_, &QPushButton::clicked, this, [this]() { togglePreviewFullscreen(1); });
  auto* stitched_footer = new QHBoxLayout();
  stitched_footer->addWidget(stitched_status_, 1);
  stitched_footer->addWidget(stitched_fullscreen_button_);
  stitched_layout->addWidget(stitched_host, 1);
  stitched_layout->addLayout(stitched_footer);

  preview_tabs_->addTab(program, "Program");
  preview_tabs_->addTab(stitched, "Stitched");
  preview_tabs_->addTab(new QLabel("Camera 1 preview"), "Camera 1");
  preview_tabs_->addTab(new QLabel("Camera 2 preview"), "Camera 2");
  preview_tabs_->addTab(new QLabel("Camera 3 preview"), "Camera 3");
  root->addWidget(preview_tabs_, 1);
}

void HmStreamWindow::buildOutputControls(QVBoxLayout* parent) {
  auto* group = new QGroupBox("Output Routing");
  group->setObjectName("outputRoutingGroup");
  auto* layout = new QVBoxLayout(group);
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
  parent->addWidget(group);
}

void HmStreamWindow::buildCameraControls(QVBoxLayout* parent) {
  auto* group = new QGroupBox("Camera Controls");
  group->setObjectName("cameraControlsGroup");
  group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  auto* layout = new QVBoxLayout(group);

  camera_tabs_ = new QTabWidget();
  camera_tabs_->setObjectName("cameraControlTabs");
  camera_tabs_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

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
      {"Stop_Direction_Change_Delay_Frames", "Stop direction-change delay frames", 0, 60, 12},
      {"Cancel_Stop_On_Opposite_Direction", "Cancel stop on opposite direction", 0, 1, 1},
      {"Stop_Cancel_Hysteresis_Frames", "Stop cancel hysteresis frames", 0, 10, 2},
      {"Stop_Delay_Cooldown_Frames", "Stop-delay cooldown frames", 0, 30, 4},
      {"Time_To_Dest_Speed_Limit_Frames", "Time-to-destination speed limit frames", 0, 120, 24},
      {"Apply_To_Fast_Box", "Apply to fast box", 0, 1, 0},
      {"Apply_To_Follower_Box", "Apply to follower box", 0, 1, 1},
  };
  const std::vector<CameraSliderSpec> motion_controls = {
      {"Overshoot_Stop_Delay_Frames", "Overshoot stop-delay frames", 0, 60, 12},
      {"Post_Nonstop_Stop_Delay_Frames", "Post-nonstop stop-delay frames", 0, 60, 12},
      {"Overshoot_Speed_Ratio_x100", "Overshoot speed ratio x100", 0, 200, 100},
      {"Max_Speed_X_x10", "Max speed X x10", 0, 2000, 300},
      {"Max_Speed_Y_x10", "Max speed Y x10", 0, 2000, 300},
      {"Max_Accel_X_x10", "Max accel X x10", 0, 1000, 120},
      {"Max_Accel_Y_x10", "Max accel Y x10", 0, 1000, 120},
  };
  const std::vector<CameraSliderSpec> stitched_color_controls = {
      {"White_Balance_Kelvin_Enable", "White balance Kelvin enable", 0, 1, 0},
      {"White_Balance_Kelvin_Temperature", "White balance Kelvin temperature", 1000, 15000, 6500},
      {"White_Balance_Red_Gain_x100", "White balance red gain x100", 1, 300, 100},
      {"White_Balance_Green_Gain_x100", "White balance green gain x100", 1, 300, 100},
      {"White_Balance_Blue_Gain_x100", "White balance blue gain x100", 1, 300, 100},
      {"Brightness_Multiplier_x100", "Brightness multiplier x100", 1, 300, 100},
      {"Exposure_EV_x10", "Exposure EV x10", 0, 80, kExposureEvSliderZero},
      {"Contrast_Multiplier_x100", "Contrast multiplier x100", 1, 300, 100},
      {"Gamma_Multiplier_x100", "Gamma multiplier x100", 1, 300, 100},
  };
  const std::vector<CameraSliderSpec> left_color_controls = {
      {"Left_White_Balance_Kelvin_Enable", "Left white balance Kelvin enable", 0, 1, 0},
      {"Left_White_Balance_Kelvin_Temperature", "Left white balance Kelvin temperature", 1000, 15000, 6500},
      {"Left_White_Balance_Red_Gain_x100", "Left white balance red gain x100", 1, 300, 100},
      {"Left_White_Balance_Green_Gain_x100", "Left white balance green gain x100", 1, 300, 100},
      {"Left_White_Balance_Blue_Gain_x100", "Left white balance blue gain x100", 1, 300, 100},
      {"Left_Brightness_Multiplier_x100", "Left brightness multiplier x100", 1, 300, 100},
      {"Left_Exposure_EV_x10", "Left exposure EV x10", 0, 80, kExposureEvSliderZero},
      {"Left_Contrast_Multiplier_x100", "Left contrast multiplier x100", 1, 300, 100},
      {"Left_Gamma_Multiplier_x100", "Left gamma multiplier x100", 1, 300, 100},
      {"Right_White_Balance_Kelvin_Enable", "Right white balance Kelvin enable", 0, 1, 0},
      {"Right_White_Balance_Kelvin_Temperature", "Right white balance Kelvin temperature", 1000, 15000, 6500},
      {"Right_White_Balance_Red_Gain_x100", "Right white balance red gain x100", 1, 300, 100},
      {"Right_White_Balance_Green_Gain_x100", "Right white balance green gain x100", 1, 300, 100},
      {"Right_White_Balance_Blue_Gain_x100", "Right white balance blue gain x100", 1, 300, 100},
      {"Right_Brightness_Multiplier_x100", "Right brightness multiplier x100", 1, 300, 100},
      {"Right_Exposure_EV_x10", "Right exposure EV x10", 0, 80, kExposureEvSliderZero},
      {"Right_Contrast_Multiplier_x100", "Right contrast multiplier x100", 1, 300, 100},
      {"Right_Gamma_Multiplier_x100", "Right gamma multiplier x100", 1, 300, 100},
  };
  const std::vector<CameraSliderSpec> stitch_controls = {
      {"Stitch_Rotate_Degrees", "Stitch rotate degrees", 0, 180, 90},
  };

  auto* plugin = new QWidget();
  auto* plugin_layout = new QVBoxLayout(plugin);
  auto* property = new QLineEdit("nvarguscamerasrc.exposuretimerange");
  property->setObjectName("pluginPropertyEdit");
  plugin_layout->addWidget(new QLabel("GStreamer property"));
  plugin_layout->addWidget(property);
  plugin_layout->addStretch(1);

  camera_tabs_->addTab(add_slider_tab(tracking_controls), "Tracking");
  camera_tabs_->addTab(add_slider_tab(motion_controls), "Motion");
  camera_tabs_->addTab(add_slider_tab(stitched_color_controls), "Color");
  camera_tabs_->addTab(add_slider_tab(left_color_controls), "Side Color");
  camera_tabs_->addTab(add_slider_tab(stitch_controls), "Stitch");
  camera_tabs_->addTab(plugin, "Plugin");
  layout->addWidget(camera_tabs_, 1);
  parent->addWidget(group, 1);
}

void HmStreamWindow::buildLog(QVBoxLayout* root) {
  log_ = new QTextEdit();
  log_->setObjectName("runtimeLog");
  log_->setReadOnly(true);
  log_->setAcceptRichText(true);
  log_->setLineWrapMode(QTextEdit::NoWrap);
  log_->document()->setMaximumBlockCount(250);
  log_->setMinimumHeight(110);
  log_->setStyleSheet(
      "QTextEdit#runtimeLog {"
      " background: #05070a;"
      " color: #d8dee9;"
      " font-family: \"JetBrains Mono\", \"SFMono-Regular\", Consolas, monospace;"
      " font-size: 12px;"
      " border: 1px solid #252a31;"
      " selection-background-color: #264f78;"
      "}");
  root->addWidget(log_);
}

QString HmStreamWindow::pipelineRunnerPath() const {
  const QByteArray test_runner = qgetenv("HMSTREAM_UI_TEST_RUNNER");
  if (!test_runner.isEmpty()) {
    return QString::fromLocal8Bit(test_runner);
  }
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    return QDir(development_root).filePath("bazel-bin/src/apps/pipeline-app/hmstream-cli");
  }
  const QString installed_runner = "/opt/hmstream/bin/hmstream-cli";
  if (QFileInfo::exists(installed_runner)) {
    return installed_runner;
  }
  const QString bazel_runner = QDir::current().filePath("bazel-bin/src/apps/pipeline-app/hmstream-cli");
  if (QFileInfo::exists(bazel_runner)) {
    return bazel_runner;
  }
  const QString legacy_bazel_runner = QDir::current().filePath("bazel-bin/src/apps/pipeline-app/pipeline-app");
  if (QFileInfo::exists(legacy_bazel_runner)) {
    return legacy_bazel_runner;
  }
  return "hmstream-cli";
}

QString HmStreamWindow::pipelineConfigPath(const QString& config_name) const {
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    const QString development_config = QDir(QDir(development_root).filePath("configs")).filePath(config_name);
    if (QFileInfo::exists(development_config)) {
      return development_config;
    }
  }
  const QString installed_config = QDir("/opt/hmstream/configs").filePath(config_name);
  if (QFileInfo::exists(installed_config)) {
    return installed_config;
  }
  return QDir("configs").filePath(config_name);
}

QString HmStreamWindow::pipelineWorkingDirectory() const {
  if (!qgetenv("HMSTREAM_UI_TEST_RUNNER").isEmpty()) {
    return QDir::currentPath();
  }
  const QString development_root = development_runtime_root();
  if (!development_root.isEmpty()) {
    return development_root;
  }
  if (QFileInfo::exists("/opt/hmstream/bin/hmstream-cli")) {
    return "/opt/hmstream";
  }
  return QDir::currentPath();
}

bool HmStreamWindow::setupPretrainedAssets(const QStringList& pipeline_args) {
  Q_UNUSED(pipeline_args);
  appendLog("pretrained assets will be verified by hmstream-cli");
  return true;
}

void HmStreamWindow::logMissingTensorRtEngineCaches(const QStringList& pipeline_args) {
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

      appendLog(QString("TensorRT engine cache missing: %1").arg(engine_file));
      appendLog(
          "first run will build/cache the primary-gie engine before video appears; the render window may stay black during this step");
      appendLog("DeepStream may also log a model-engine-file open/deserialize warning while it builds the engine");
    } catch (const std::exception& e) {
      appendLog(QString("could not inspect TensorRT engine cache from %1: %2").arg(config_file, e.what()));
    }
  }
}

QStringList HmStreamWindow::enabledSinkNames() const {
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
  if (!sinks.contains("RENDER")) {
    sinks.push_front("RENDER");
  }
  sinks.removeDuplicates();
  return sinks;
}

bool HmStreamWindow::isCalibrationRun() const {
  return run_mode_selector_ && run_mode_selector_->currentData().toString() == "stitch-calibration";
}

int HmStreamWindow::stitchingCalibrationControlPoints() const {
  return control_points_spin_ ? control_points_spin_->value() : kDefaultStitchCalibrationControlPoints;
}

bool HmStreamWindow::runStitchingClean(
    const QString& runner,
    const QString& working_dir,
    const QProcessEnvironment& env) {
  const QString game_id = game_id_edit_ ? game_id_edit_->text().trimmed() : QString();
  QStringList clean_args;
  clean_args << "-g" << game_id << "--enable-sources=URI-MULTIPLE";
  clean_args << "-c" << pipelineConfigPath("ds_hockey_configure_stitching.yaml");
  clean_args << "--clean";

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

bool HmStreamWindow::saveStitchingCalibrationState(int control_points, const QString& status) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
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

  config["hmstream_ui"]["stitching_calibration"]["control_points"] = control_points;
  config["hmstream_ui"]["stitching_calibration"]["status"] = status.toStdString();
  const auto publish = publish_yaml_config(config_path, config);
  if (!publish.ok()) {
    appendLog(QString("failed to write stitching calibration settings %1: %2")
                  .arg(QString::fromStdString(config_path.string()), publish.ToString().c_str()));
    return false;
  }
  appendLog(QString("stitching calibration control points saved %1 status=%2").arg(control_points).arg(status));
  return true;
}

bool HmStreamWindow::prepareStitchingCalibrationRun(
    const QString& runner,
    const QString& working_dir,
    const QProcessEnvironment& env) {
  const int control_points = stitchingCalibrationControlPoints();
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  bool saved_found = false;
  int saved_control_points = 0;
  QString saved_status;
  auto loaded_config = hm::stitching::load_game_config_file(config_path);
  if (!loaded_config.ok()) {
    appendLog(
        QString("could not read stitching calibration settings: %1").arg(loaded_config.status().ToString().c_str()));
    return false;
  }
  if (loaded_config->has_value()) {
    try {
      const YAML::Node config = **loaded_config;
      YAML::Node saved;
      if (lookup_yaml_path(config, "hmstream_ui.stitching_calibration.control_points", &saved) && saved.IsScalar()) {
        saved_control_points = saved.as<int>();
        saved_found = true;
      }
      YAML::Node status;
      if (lookup_yaml_path(config, "hmstream_ui.stitching_calibration.status", &status) && status.IsScalar()) {
        saved_status = QString::fromStdString(status.as<std::string>());
      }
    } catch (const std::exception& exc) {
      appendLog(QString("could not read stitching calibration settings: %1").arg(exc.what()));
      return false;
    }
  }

  if (!saved_found || saved_control_points != control_points || saved_status != "complete") {
    const QString previous = saved_found ? QString::number(saved_control_points) : QString("unset");
    appendLog(QString("stitching calibration control points changed %1 -> %2 status=%3; cleaning stitch artifacts")
                  .arg(previous)
                  .arg(control_points)
                  .arg(saved_status.isEmpty() ? "unset" : saved_status));
    if (!runStitchingClean(runner, working_dir, env)) {
      return false;
    }
  }
  return saveStitchingCalibrationState(control_points, "pending");
}

QStringList HmStreamWindow::pipelineArguments() const {
  const QString game_id = game_id_edit_ ? game_id_edit_->text().trimmed() : QString();
  const QString configured_render_sink = qEnvironmentVariable("HM_RENDER_SINK").trimmed().toLower();
  const bool embed_render_window = configured_render_sink == "nveglglessink" || configured_render_sink == "egl";
  QStringList args;
  args << "-g" << game_id << "--enable-sources=URI-MULTIPLE";
  if (isCalibrationRun()) {
    args << "-c" << pipelineConfigPath("ds_hockey_configure_stitching.yaml");
    args << "--enable-sinks=RENDER";
    args << "--show-stitching" << "1";
    if (embed_render_window && stitched_surface_) {
      args << QString("--render-window-id=%1").arg(static_cast<qulonglong>(stitched_surface_->winId()));
    }
  } else {
    args << "-c" << pipelineConfigPath("ds_hockey_app_config.yaml");
    args << QString("--enable-sinks=%1").arg(enabledSinkNames().join(","));
    args << "--show";
    if (embed_render_window && preview_surface_) {
      args << QString("--render-window-id=%1").arg(static_cast<qulonglong>(preview_surface_->winId()));
    }
  }
  args << "--options=pipeline.hmaudio.enable=1";
  return args;
}

void HmStreamWindow::startPipeline() {
  if (!pipeline_process_ || pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("pipeline already running");
    return;
  }
  if (!ensureGameDirectory()) {
    updateRunControls();
    return;
  }
  if (preview_tabs_) {
    preview_tabs_->setCurrentIndex(isCalibrationRun() ? 1 : 0);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }

  const QString runner = pipelineRunnerPath();
  const QStringList args = pipelineArguments();
  if (QFileInfo(runner).isAbsolute() && !QFileInfo::exists(runner)) {
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Pipeline failed to start");
    appendLog(QString("pipeline process error=missing runner %1").arg(runner));
    updateRunControls();
    return;
  }
  if (!setupPretrainedAssets(args)) {
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Asset setup failed");
    updateRunControls();
    return;
  }
  logMissingTensorRtEngineCaches(args);

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString working_dir = pipelineWorkingDirectory();
  configure_pipeline_runtime_environment(env, working_dir);
  const bool embedded_render = std::any_of(
      args.begin(), args.end(), [](const QString& argument) { return argument.startsWith("--render-window-id="); });
  if (preview_external_notice_)
    preview_external_notice_->setVisible(!embedded_render);
  if (stitched_external_notice_)
    stitched_external_notice_->setVisible(!embedded_render);
  if (program_fullscreen_button_)
    program_fullscreen_button_->setEnabled(embedded_render);
  if (stitched_fullscreen_button_)
    stitched_fullscreen_button_->setEnabled(embedded_render);
  if (!embedded_render)
    appendLog("nv3dsink render output will open in a separate DeepStream window; embedded preview is disabled");
  if (isCalibrationRun()) {
    if (!prepareStitchingCalibrationRun(runner, working_dir, env)) {
      pipeline_state_->setText("STOPPED");
      preview_status_->setText("Stitching calibration setup failed");
      updateRunControls();
      return;
    }
    const int control_points = stitchingCalibrationControlPoints();
    env.insert("HM_MAX_CONTROL_POINTS", QString::number(control_points));
    appendLog(QString("stitching calibration control points=%1").arg(control_points));
  }
  appendLog("audio enabled via pipeline.hmaudio.enable=1; render audio uses the configured system audio sink");
  pipeline_process_->setProcessEnvironment(env);
  pipeline_process_->setWorkingDirectory(working_dir);
#ifdef Q_OS_UNIX
  const QString setsid = "/usr/bin/setsid";
  if (QFileInfo::exists(setsid)) {
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
  preview_status_->setText(
      isCalibrationRun() ? "Starting stitching calibration pipeline" : "Starting program pipeline");
  if (isCalibrationRun() && stitched_status_) {
    stitched_status_->setText(
        QString("Starting stitching calibration\nControl points: %1\nRender sink: hmstitcher output")
            .arg(stitchingCalibrationControlPoints()));
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

void HmStreamWindow::pauseOrResumePipeline() {
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

void HmStreamWindow::stopPipeline() {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    pipeline_state_->setText("STOPPED");
    preview_status_->setText("Pipeline stopped");
    appendLog("pipeline already stopped");
    updateRunControls();
    return;
  }
  appendLog("pipeline stop requested");
  pipeline_stop_requested_ = true;
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

void HmStreamWindow::handlePipelineStarted() {
  pipeline_state_->setText("PLAYING");
  preview_status_->setText(isCalibrationRun() ? "Stitching calibration running" : "Program pipeline running");
  appendLog(QString("pipeline started pid=%1").arg(pipeline_process_ ? pipeline_process_->processId() : 0));
  updateRunControls();
}

void HmStreamWindow::handlePipelineFinished(int exit_code, QProcess::ExitStatus exit_status) {
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
  const bool stopped_by_user = pipeline_stop_requested_;
  pipeline_stop_requested_ = false;
  if (isCalibrationRun() && !stopped_by_user && exit_status == QProcess::NormalExit && exit_code == 0) {
    saveStitchingCalibrationState(stitchingCalibrationControlPoints(), "complete");
  }
  pipeline_state_->setText("STOPPED");
  preview_status_->setText("Pipeline stopped");
  appendLog(QString("pipeline finished exit=%1 status=%2")
                .arg(exit_code)
                .arg(exit_status == QProcess::NormalExit ? "normal" : "crashed"));
  updateRunControls();
}

void HmStreamWindow::handlePipelineError(QProcess::ProcessError error) {
  pipeline_paused_ = false;
  pipeline_uses_process_group_ = false;
  pipeline_stop_requested_ = false;
  pipeline_state_->setText("STOPPED");
  preview_status_->setText("Pipeline failed to start");
  appendLog(QString("pipeline process error=%1 message=%2")
                .arg(static_cast<int>(error))
                .arg(pipeline_process_ ? pipeline_process_->errorString() : QString()));
  updateRunControls();
}

void HmStreamWindow::readPipelineOutput() {
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
        appendLog(line.trimmed());
      }
    }
  };
  drain(pipeline_process_->readAllStandardOutput(), &pipeline_stdout_buffer_);
  drain(pipeline_process_->readAllStandardError(), &pipeline_stderr_buffer_);
}

void HmStreamWindow::togglePreviewFullscreen(int tab_index) {
  if (preview_tabs_) {
    preview_tabs_->setCurrentIndex(tab_index);
  }
  preview_fullscreen_ = !preview_fullscreen_;
  if (preview_fullscreen_) {
    showFullScreen();
    appendLog(tab_index == 1 ? "stitched preview fullscreen" : "program preview fullscreen");
  } else {
    showNormal();
    appendLog("preview restored to normal window");
  }
}

void HmStreamWindow::updateRunControls() {
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
    control_points_spin_->setEnabled(!running && isCalibrationRun());
  }
}

void HmStreamWindow::restartStage() {
  appendLog("stage restart requested");
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    stopPipeline();
  }
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("restart skipped because pipeline is still stopping");
    return;
  }
  startPipeline();
}

void HmStreamWindow::savePreset() {
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

void HmStreamWindow::resetCameraControls() {
  for (const auto& [id, value] : camera_defaults_) {
    const auto it = camera_sliders_.find(id);
    if (it != camera_sliders_.end()) {
      it->second->setValue(value);
    }
  }
  appendLog("camera controls reset to defaults");
}

void HmStreamWindow::loadSavedControlConfig() {
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
    YAML::Node control_points;
    if (control_points_spin_ &&
        lookup_yaml_path(config, "hmstream_ui.stitching_calibration.control_points", &control_points) &&
        control_points.IsScalar()) {
      const bool blocked = control_points_spin_->blockSignals(true);
      control_points_spin_->setValue(control_points.as<int>());
      control_points_spin_->blockSignals(blocked);
    }
    YAML::Node controls = config["hmstream_ui"]["camera_controls"];
    if (!controls || !controls.IsMap()) {
      return;
    }
    int loaded = 0;
    for (const auto& entry : controls) {
      const QString id = QString::fromStdString(entry.first.as<std::string>());
      const auto slider_it = camera_sliders_.find(id);
      if (slider_it == camera_sliders_.end()) {
        continue;
      }
      const bool blocked = slider_it->second->blockSignals(true);
      const int value = entry.second.as<int>();
      slider_it->second->setValue(value);
      slider_it->second->blockSignals(blocked);
      const auto label_it = camera_value_labels_.find(id);
      if (label_it != camera_value_labels_.end()) {
        label_it->second->setText(QString::number(value));
      }
      ++loaded;
    }
    appendLog(QString("loaded %1 saved camera controls").arg(loaded));
  } catch (const std::exception& exc) {
    appendLog(QString("could not load saved camera controls: %1").arg(exc.what()));
  }
}

bool HmStreamWindow::applySavedControlConfig(
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
  YAML::Node previous_hmstream_ui = map_value(config, "hmstream_ui");
  YAML::Node previous_generated = map_value(previous_hmstream_ui, "generated_runtime_keys");
  YAML::Node previous_generated_values = map_value(previous_hmstream_ui, "generated_runtime_values");
  YAML::Node previous_playtracker_config_base = map_value(previous_hmstream_ui, "playtracker_config_base");
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
  remove_yaml_path(config, {"hmstream_ui", "generated_runtime_keys"});
  remove_yaml_path(config, {"hmstream_ui", "generated_runtime_values"});
  remove_yaml_path(config, {"hmstream_ui", "playtracker_config_base"});
  YAML::Node current_playtracker_config;
  if (previous_playtracker_config_base && previous_playtracker_config_base.IsScalar() &&
      !lookup_yaml_path(config, "pipeline.ds-playtracker.config-file", &current_playtracker_config)) {
    config["pipeline"]["ds-playtracker"]["config-file"] = previous_playtracker_config_base.as<std::string>();
  }
  remove_yaml_path_if_empty_map(config, {"stitching", "left", "color"});
  remove_yaml_path_if_empty_map(config, {"stitching", "right", "color"});
  remove_yaml_path_if_empty_map(config, {"rink", "camera", "color"});

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
  config["hmstream_ui"]["camera_controls"] = controls;

  auto slider_value = [this](const QString& id) -> int {
    const auto it = camera_sliders_.find(id);
    return it == camera_sliders_.end() ? 0 : it->second->value();
  };
  auto slider_changed = [this](const QString& id) -> bool {
    const auto slider_it = camera_sliders_.find(id);
    const auto default_it = camera_defaults_.find(id);
    return slider_it != camera_sliders_.end() && default_it != camera_defaults_.end() &&
        slider_it->second->value() != default_it->second;
  };
  if (has_control(controls, "Stitch_Rotate_Degrees")) {
    config["stitching"]["post_stitch_rotate_degrees"] = 90 - slider_value("Stitch_Rotate_Degrees");
    mark_runtime_key("stitching.post_stitch_rotate_degrees");
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
  auto apply_color_prefix = [&](const QString& prefix) {
    const QString name_prefix = prefix.isEmpty() ? QString() : prefix + "_";
    const char* side_key = prefix.compare("Left", Qt::CaseInsensitive) == 0 ? "left" : "right";
    const QString config_prefix =
        prefix.isEmpty() ? QString("rink.camera.color") : QString("stitching.%1.color").arg(prefix.toLower());
    auto set_color_leaf = [&](const char* leaf, const auto& value) {
      if (prefix.isEmpty()) {
        config["rink"]["camera"]["color"][leaf] = value;
      } else {
        config["stitching"][side_key]["color"][leaf] = value;
      }
      mark_runtime_key(config_prefix + "." + leaf);
    };
    auto remove_color_leaf = [&](const char* leaf) { remove_yaml_path(config, config_prefix + "." + leaf); };

    if (slider_changed(name_prefix + "Brightness_Multiplier_x100")) {
      set_color_leaf("brightness", ratio_x100(slider_value(name_prefix + "Brightness_Multiplier_x100")));
    }
    if (slider_changed(name_prefix + "Exposure_EV_x10")) {
      set_color_leaf("exposure_ev", slider_to_exposure_ev(slider_value(name_prefix + "Exposure_EV_x10")));
    }
    if (slider_changed(name_prefix + "Contrast_Multiplier_x100")) {
      set_color_leaf("contrast", ratio_x100(slider_value(name_prefix + "Contrast_Multiplier_x100")));
    }
    if (slider_changed(name_prefix + "Gamma_Multiplier_x100")) {
      set_color_leaf("gamma", ratio_x100(slider_value(name_prefix + "Gamma_Multiplier_x100")));
    }

    const QStringList white_balance_ids = {
        name_prefix + "White_Balance_Kelvin_Enable",
        name_prefix + "White_Balance_Kelvin_Temperature",
        name_prefix + "White_Balance_Red_Gain_x100",
        name_prefix + "White_Balance_Green_Gain_x100",
        name_prefix + "White_Balance_Blue_Gain_x100",
    };
    bool white_balance_changed = false;
    for (const QString& id : white_balance_ids) {
      white_balance_changed = white_balance_changed || slider_changed(id);
    }
    if (white_balance_changed) {
      const int kelvin_enabled = slider_value(name_prefix + "White_Balance_Kelvin_Enable");
      const int kelvin = slider_value(name_prefix + "White_Balance_Kelvin_Temperature");
      const int red = slider_value(name_prefix + "White_Balance_Red_Gain_x100");
      const int green = slider_value(name_prefix + "White_Balance_Green_Gain_x100");
      const int blue = slider_value(name_prefix + "White_Balance_Blue_Gain_x100");
      if (kelvin_enabled > 0) {
        set_color_leaf("white_balance_temp", QString("%1k").arg(kelvin).toStdString());
        remove_color_leaf("white_balance");
        return;
      }
      YAML::Node white_balance(YAML::NodeType::Sequence);
      white_balance.push_back(ratio_x100(blue));
      white_balance.push_back(ratio_x100(green));
      white_balance.push_back(ratio_x100(red));
      set_color_leaf("white_balance", white_balance);
      remove_color_leaf("white_balance_temp");
    }
  };
  apply_color_prefix("");
  apply_color_prefix("Left");
  apply_color_prefix("Right");

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
  auto apply_ratio_control = [&](const QString& id, const char* yaml_key) {
    const std::string control_key = id.toStdString();
    if (!has_control(controls, control_key.c_str())) {
      return;
    }
    const auto default_it = camera_defaults_.find(id);
    const double default_value = default_it == camera_defaults_.end() ? 0.0 : static_cast<double>(default_it->second);
    if (default_value <= 0.0) {
      return;
    }
    config["rink"]["camera"][yaml_key] = static_cast<double>(slider_value(id)) / default_value;
    mark_runtime_key(QString("rink.camera.") + yaml_key);
  };
  apply_ratio_control("Max_Speed_X_x10", "max_speed_ratio_x");
  apply_ratio_control("Max_Speed_Y_x10", "max_speed_ratio_y");
  apply_ratio_control("Max_Accel_X_x10", "max_accel_ratio_x");
  apply_ratio_control("Max_Accel_Y_x10", "max_accel_ratio_y");
  const bool has_live_box_runtime_controls = has_control(controls, "Max_Speed_X_x10") ||
      has_control(controls, "Max_Speed_Y_x10") || has_control(controls, "Max_Accel_X_x10") ||
      has_control(controls, "Max_Accel_Y_x10");
  if (has_live_box_runtime_controls && game_id_edit_) {
    const QString game_dir = gameDirectory(game_id_edit_->text());
    QDir runtime_dir(QDir(game_dir).filePath(".hmstream-ui"));
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

        auto apply_live_box = [&](int index) {
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
              config["hmstream_ui"]["playtracker_config_base"] = configured_playtracker_config.toStdString();
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
    config["hmstream_ui"]["camera_control_targets"]["apply_to_fast_box"] = slider_value("Apply_To_Fast_Box") != 0;
    mark_runtime_key("hmstream_ui.camera_control_targets.apply_to_fast_box");
  }
  if (has_control(controls, "Apply_To_Follower_Box")) {
    config["hmstream_ui"]["camera_control_targets"]["apply_to_follower_box"] =
        slider_value("Apply_To_Follower_Box") != 0;
    mark_runtime_key("hmstream_ui.camera_control_targets.apply_to_follower_box");
  }
  if (generated_runtime_keys.size() > 0) {
    for (const auto& path_node : generated_runtime_keys) {
      const std::string key = path_node.as<std::string>();
      YAML::Node value;
      if (lookup_yaml_path(config, QString::fromStdString(key), &value)) {
        generated_runtime_values[key.c_str()] = YAML::Dump(value);
      }
    }
    config["hmstream_ui"]["generated_runtime_keys"] = generated_runtime_keys;
    config["hmstream_ui"]["generated_runtime_values"] = generated_runtime_values;
  }
  appendLog(QString("preset captured %1 non-default camera controls").arg(changed));
  return true;
}

void HmStreamWindow::refreshGames() {
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

void HmStreamWindow::selectGame(const QString& game_id) {
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

void HmStreamWindow::createOrLoadGame() {
  if (!ensureGameDirectory()) {
    return;
  }
  refreshGames();
  refreshVideoSets();
  loadSavedControlConfig();
  appendLog(QString("game ready %1").arg(game_id_edit_->text()));
}

void HmStreamWindow::addVideoPath() {
  if (!video_path_edit_) {
    return;
  }
  const QString role = selectedVideoRole();
  if (!ensureGameDirectory()) {
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
    return;
  }
  if (role == "auto") {
    const CopiedImportCleanupResult cleanup =
        removeClearedCopiedExplicitImports(original_config, had_config, true, published_config);
    if (cleanup == CopiedImportCleanupResult::kRolledBack) {
      if (imported_path_created) {
        rollbackImportedVideoPath(imported_relative_path);
      }
      return;
    }
    if (cleanup == CopiedImportCleanupResult::kCommittedWithCleanupFailure) {
      appendLog("video set added, but one or more unreferenced copied imports could not be cleaned");
    }
  }
  refreshVideoSets();
  appendLog(QString("video set added role=%1 path=%2").arg(role_label(role), imported_relative_path));
}

void HmStreamWindow::browseVideoPath() {
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

void HmStreamWindow::removeSelectedVideoSet() {
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
    refreshVideoSets();
    return;
  }
  if (role == "auto" &&
      removeClearedCopiedExplicitImports(original_config, had_config, false) != CopiedImportCleanupResult::kSuccess) {
    appendLog("video set removed, but one or more unreferenced copied imports could not be cleaned");
  }

  appendLog(QString("video set removed role=%1 path=%2").arg(role_label(role), relative_path));
  delete item;
  refreshVideoSets();
}

void HmStreamWindow::refreshVideoSets() {
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
      YAML::Node explicit_roles = config["hmstream_ui"]["video_roles"];
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

QString HmStreamWindow::selectedVideoRole() const {
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

QString HmStreamWindow::gameRoot() const {
  const QByteArray env = qgetenv("HM_GAME_DIR");
  if (!env.isEmpty()) {
    return QString::fromLocal8Bit(env);
  }
  return QDir::home().filePath("Videos");
}

QString HmStreamWindow::gameDirectory(const QString& game_id) const {
  if (game_id.isEmpty()) {
    return gameRoot();
  }
  return QDir(gameRoot()).filePath(game_id);
}

QString HmStreamWindow::relativeToGameDir(const QString& path) const {
  const QDir dir(gameDirectory(game_id_edit_->text()));
  return dir.relativeFilePath(path);
}

bool HmStreamWindow::ensureGameDirectory() {
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

bool HmStreamWindow::importVideoPath(const QString& source_path, QString* imported_relative_path, bool* created) {
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
    const QString ui_dir = ".hmstream-ui";
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

bool HmStreamWindow::saveCopiedImport(
    const QString& relative_path,
    const QString& auto_group_family,
    const QString& source_parent) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock copied import metadata: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not update copied import metadata: %1").arg(exc.what()));
      return false;
    }
  }

  YAML::Node list = config["hmstream_ui"]["copied_imports"];
  if (!list || !list.IsSequence()) {
    config["hmstream_ui"]["copied_imports"] = YAML::Node(YAML::NodeType::Sequence);
    list = config["hmstream_ui"]["copied_imports"];
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
    YAML::Node sources = config["hmstream_ui"]["auto_import_sources"];
    if (!sources || !sources.IsSequence()) {
      config["hmstream_ui"]["auto_import_sources"] = YAML::Node(YAML::NodeType::Sequence);
      sources = config["hmstream_ui"]["auto_import_sources"];
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

bool HmStreamWindow::rollbackImportedVideoPath(const QString& relative_path) {
  const QDir game_dir(gameDirectory(game_id_edit_->text()));
  const fs::path config_path = fs::path(game_dir.absolutePath().toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock imported video rollback: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }

  bool copied_import = false;
  if (fs::exists(config_path)) {
    YAML::Node config;
    try {
      config = YAML::LoadFile(config_path.string());
      copied_import = is_copied_import_in_config(config, game_dir, relative_path);
    } catch (const std::exception& exc) {
      appendLog(QString("could not read copied import metadata during rollback: %1").arg(exc.what()));
      return false;
    }

    if (copied_import) {
      auto matches_path = [&](const QString& value) {
        return normalized_config_video_path(game_dir, value) == normalized_config_video_path(game_dir, relative_path);
      };
      YAML::Node copied_imports = config["hmstream_ui"]["copied_imports"];
      YAML::Node copied_replacement(YAML::NodeType::Sequence);
      if (copied_imports && copied_imports.IsSequence()) {
        for (const auto& item : copied_imports) {
          const QString path = QString::fromStdString(item.as<std::string>());
          if (!matches_path(path)) {
            copied_replacement.push_back(item.as<std::string>());
          }
        }
      }
      config["hmstream_ui"]["copied_imports"] = copied_replacement;

      YAML::Node sources = config["hmstream_ui"]["auto_import_sources"];
      if (sources && sources.IsSequence()) {
        YAML::Node source_replacement(YAML::NodeType::Sequence);
        for (const auto& item : sources) {
          if (!item["path"] || !matches_path(QString::fromStdString(item["path"].as<std::string>()))) {
            source_replacement.push_back(item);
          }
        }
        config["hmstream_ui"]["auto_import_sources"] = source_replacement;
      }

      const auto publish = publish_yaml_config(config_path, config);
      if (!publish.ok()) {
        appendLog(
            QString("failed to remove copied import metadata during rollback: %1").arg(publish.ToString().c_str()));
        return false;
      }
    }
  }

  if (!removeImportedVideoPath(relative_path, copied_import)) {
    appendLog(QString("failed to remove imported video during rollback %1").arg(relative_path));
    return false;
  }
  return true;
}

HmStreamWindow::CopiedImportCleanupResult HmStreamWindow::removeClearedCopiedExplicitImports(
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
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock copied import cleanup: %1").arg(config_lock.status().ToString().c_str()));
    return CopiedImportCleanupResult::kCommittedWithCleanupFailure;
  }
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
  YAML::Node current_roles = current_config["hmstream_ui"]["video_roles"];
  for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
    collect_current(current_roles[role.toStdString()]);
  }

  std::set<QString> cleanup_paths;
  YAML::Node old_roles = old_config["hmstream_ui"]["video_roles"];
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
    YAML::Node copied_imports = current_config["hmstream_ui"]["copied_imports"];
    YAML::Node copied_replacement(YAML::NodeType::Sequence);
    if (copied_imports && copied_imports.IsSequence()) {
      for (const auto& item : copied_imports) {
        const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
        if (!removed_paths.count(path)) {
          copied_replacement.push_back(item.as<std::string>());
        }
      }
    }
    current_config["hmstream_ui"]["copied_imports"] = copied_replacement;

    YAML::Node sources = current_config["hmstream_ui"]["auto_import_sources"];
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
      current_config["hmstream_ui"]["auto_import_sources"] = source_replacement;
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

bool HmStreamWindow::syncRuntimeExplicitVideoConfig(YAML::Node& config) {
  bool changed = false;
  YAML::Node explicit_left = config["hmstream_ui"]["video_roles"]["left"];
  YAML::Node explicit_right = config["hmstream_ui"]["video_roles"]["right"];
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

bool HmStreamWindow::savePrivateConfigForRole(
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
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock private config: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }
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
  if (is_explicit_role(role)) {
    YAML::Node list = config["hmstream_ui"]["video_roles"][role.toStdString()];
    if (!list || !list.IsSequence()) {
      config["hmstream_ui"]["video_roles"][role.toStdString()] = YAML::Node(YAML::NodeType::Sequence);
      list = config["hmstream_ui"]["video_roles"][role.toStdString()];
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
    }
    changed = clear_stitching_frame_offsets(config) || changed;
  }

  if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
  }

  if (role == "auto") {
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
    appendLog("auto video set will be discovered from the game directory");
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

bool HmStreamWindow::removePrivateConfigForRole(
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
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock private config: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }
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
  auto matches_path = [&](const QString& value) {
    if (value == relative_path) {
      return true;
    }
    const QString normalized = normalized_config_video_path(QDir(gameDirectory(game_id_edit_->text())), value);
    return normalized == relative_path;
  };

  auto remove_from_list = [&](YAML::Node parent, const QString& key) {
    YAML::Node list = parent[key.toStdString()];
    if (!list || !list.IsSequence()) {
      return;
    }

    YAML::Node replacement(YAML::NodeType::Sequence);
    for (const auto& item : list) {
      const QString value = QString::fromStdString(item.as<std::string>());
      if (matches_path(value)) {
        changed = true;
      } else {
        replacement.push_back(value.toStdString());
      }
    }
    parent[key.toStdString()] = replacement;
  };

  auto remove_auto_source_metadata = [&]() {
    YAML::Node list = config["hmstream_ui"]["auto_import_sources"];
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
    config["hmstream_ui"]["auto_import_sources"] = replacement;
  };

  if (is_explicit_role(role)) {
    remove_from_list(config["hmstream_ui"]["video_roles"], role);
  }
  remove_from_list(config["hmstream_ui"], "copied_imports");
  remove_auto_source_metadata();
  if (role == "auto") {
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
  } else if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
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

bool HmStreamWindow::restorePrivateConfigAfterRemoveFailure(
    const QByteArray& original_config,
    bool had_config,
    const QByteArray& removed_config) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config_path.parent_path());
  if (!config_lock.ok()) {
    appendLog(QString("could not lock private config rollback: %1").arg(config_lock.status().ToString().c_str()));
    return false;
  }
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

bool HmStreamWindow::removeImportedVideoPath(const QString& relative_path, bool allow_regular_delete) {
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
  if (!imported.exists()) {
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

void HmStreamWindow::toggleOutput(const QString& id, bool enabled) {
  output_states_[id]->setText(enabled ? "ENABLED" : "STOPPED");
  appendLog(QString("output route %1 %2").arg(id, enabled ? "enabled" : "disabled"));
  if (pipeline_process_ && pipeline_process_->state() != QProcess::NotRunning) {
    appendLog("output route change will apply on the next pipeline start with the current runner backend");
  }
}

void HmStreamWindow::redirectYoutube() {
  QCheckBox* toggle = output_toggles_["youtube-primary"];
  const bool was_blocked = toggle->blockSignals(true);
  toggle->setChecked(true);
  toggle->blockSignals(was_blocked);
  output_states_["youtube-primary"]->setText("REDIRECTED");
  appendLog("youtube-primary RTMP route enabled for the next pipeline start");
}

void HmStreamWindow::addRtspOutput() {
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

void HmStreamWindow::appendLog(const QString& message) {
  const QString html =
      QString("<span style=\"color:#667085\">%1</span> %2").arg(timestamp().toHtmlEscaped(), ansi_to_html(message));
  log_->append(html);
}

QString HmStreamWindow::writePlaytrackerRuntimeConfig() {
  if (!game_id_edit_) {
    return {};
  }
  const QString game_dir = gameDirectory(game_id_edit_->text());
  QDir runtime_dir(QDir(game_dir).filePath(".hmstream-ui"));
  if (!runtime_dir.exists() && !runtime_dir.mkpath(".")) {
    appendLog(QString("could not create playtracker runtime config directory %1").arg(runtime_dir.path()));
    return {};
  }

  const QString runtime_config_path = runtime_dir.filePath("play_tracker_config.yaml");
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
          if (same_file_path(candidate, runtime_config_path)) {
            YAML::Node base_config_file;
            if (lookup_yaml_path(game_config, "hmstream_ui.playtracker_config_base", &base_config_file) &&
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
      set_if_changed("Max_Speed_X_x10", "max-speed-x");
      set_if_changed("Max_Speed_Y_x10", "max-speed-y");
      set_if_changed("Max_Accel_X_x10", "max-accel-x");
      set_if_changed("Max_Accel_Y_x10", "max-accel-y");
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
      return {};
    }
    tracker_out << play_tracker_config << "\n";
    tracker_out.close();
    if (!tracker_out) {
      appendLog(QString("could not write playtracker runtime config %1").arg(runtime_config_path));
      return {};
    }
    return runtime_config_path;
  } catch (const std::exception& exc) {
    appendLog(QString("could not save playtracker runtime config: %1").arg(exc.what()));
    return {};
  }
}

bool HmStreamWindow::sendLiveCameraControl(const QString& id, int value) {
  if (!pipeline_process_ || pipeline_process_->state() == QProcess::NotRunning) {
    return false;
  }
  if (id == "Stitch_Rotate_Degrees") {
    const int post_stitch_rotate_degrees = 90 - value;
    const QByteArray command = QString("@set-property hmstitcher0 post-stitch-rotate-degrees=%1\n")
                                   .arg(post_stitch_rotate_degrees)
                                   .toLocal8Bit();
    return pipeline_process_->write(command) == command.size();
  }
  const QSet<QString> playtracker_live_controls = {
      "Max_Speed_X_x10",
      "Max_Speed_Y_x10",
      "Max_Accel_X_x10",
      "Max_Accel_Y_x10",
      "Apply_To_Fast_Box",
      "Apply_To_Follower_Box",
  };
  if (playtracker_live_controls.contains(id)) {
    const QString runtime_config_path = writePlaytrackerRuntimeConfig();
    if (runtime_config_path.isEmpty()) {
      return false;
    }
    const QByteArray command =
        QString("@set-property dsplaytracker0 config-file=%1\n").arg(runtime_config_path).toLocal8Bit();
    return pipeline_process_->write(command) == command.size();
  }
  return false;
}

QSlider* HmStreamWindow::addSlider(
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
    const bool sent_live = sendLiveCameraControl(id, new_value);
    appendLog(QString("camera control %1=%2 apply=%3").arg(id).arg(new_value).arg(sent_live ? "live" : "save/restart"));
  });
  row->addWidget(name, 0, 0);
  row->addWidget(value_label, 0, 1);
  row->addWidget(slider, 1, 0, 1, 2);
  layout->addLayout(row);
  return slider;
}
