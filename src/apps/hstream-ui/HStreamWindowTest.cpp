#include "src/apps/hstream-ui/HStreamWindow.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerRuntimeConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyleOptionSlider>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QToolButton>

#include <yaml-cpp/yaml.h>

#include <tiffio.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct HStreamWindowTestAccess {
  static void appendLog(HStreamWindow* window, const QString& message) {
    window->appendLog(message);
  }

  static void recordCalibrationDiagnostic(HStreamWindow* window, const QString& line) {
    window->recordStitchingCalibrationDiagnostic(line);
  }

  static QString calibrationFailureAnalysis(HStreamWindow* window, const QString& message) {
    return window->stitchingCalibrationFailureAnalysis(message);
  }

  static void setCalibrationPrecisionRunActive(HStreamWindow* window, bool active) {
    window->active_run_is_calibration_ = active;
    window->active_run_high_bit_depth_ = false;
    window->active_run_high_bit_depth_resolved_ = false;
    window->active_run_high_bit_depth_mode_.clear();
    window->active_run_source_count_ = 0;
    window->active_run_unknown_source_count_ = 0;
    window->active_run_minimum_source_bit_depth_ = 0;
    window->updateStitchedColorPrecisionControls();
  }

  static bool reportHighBitDepth(HStreamWindow* window, const QString& line) {
    return window->handleHighBitDepthOutput(line);
  }

  static bool liveRotationAuthorizationPending(HStreamWindow* window) {
    return window->live_rotation_authorization_pending_;
  }

  static void beginPendingPlaybackSeek(HStreamWindow* window, quint64 generation) {
    window->active_run_local_render_only_ = true;
    window->active_run_is_calibration_ = false;
    window->calibration_pending_ = false;
    window->pipeline_paused_ = false;
    window->playback_duration_ns_ = 600'000'000'000LL;
    if (window->render_video_toggle_) {
      window->render_video_toggle_->setChecked(true);
    }
    window->pending_playback_seek_generation_ = generation;
    window->playback_seek_channel_available_ = true;
    window->updatePlaybackSeekControls();
  }

  static void reportPipelineError(HStreamWindow* window, QProcess::ProcessError error) {
    window->handlePipelineError(error);
  }

  static void beginPendingResumedPlaybackSeek(HStreamWindow* window, quint64 generation) {
    beginPendingPlaybackSeek(window, generation);
    window->playback_warming_after_resume_ = true;
    window->resume_progress_reset_waiting_for_seek_ = true;
  }

  static quint64 playbackResetGeneration(HStreamWindow* window) {
    return window->playback_reset_generation_;
  }

  static bool resumeProgressResetWaitingForSeek(HStreamWindow* window) {
    return window->resume_progress_reset_waiting_for_seek_;
  }

  static bool handlePlaybackProgressOutput(HStreamWindow* window, const QString& line) {
    return window->handlePlaybackProgressOutput(line);
  }

  static void beginTimedOutPlaybackSeekRecovery(HStreamWindow* window, quint64 generation) {
    beginPendingPlaybackSeek(window, generation);
    window->handlePlaybackSeekOutput(
        QString("HSTREAM_SEEK status=failed generation=%1 reason=pipeline-recreate-timeout").arg(generation));
  }

  static qint64 requestPipelineProcessExit(HStreamWindow* window) {
    return window->pipeline_process_ ? window->pipeline_process_->write("@test-exit\n") : -1;
  }

  static QStringList pipelineArguments(HStreamWindow* window) {
    return window->pipelineArguments();
  }

  static void refreshRunControls(HStreamWindow* window) {
    window->updateRunControls();
  }

  static void refreshPlaybackSeekControls(HStreamWindow* window) {
    window->updatePlaybackSeekControls();
  }

  static qint64 playbackPositionNs(HStreamWindow* window) {
    return window->playback_position_ns_;
  }

  static qint64 playbackDurationNs(HStreamWindow* window) {
    return window->playback_duration_ns_;
  }

  static void clearLog(HStreamWindow* window) {
    if (window->log_)
      window->log_->clear();
  }

  static void finishArchiveJobLogAfterFinalizationFailure(
      HStreamWindow* window,
      const QString& configured_output_path,
      const QString& resolved_output_path,
      const QString& run_id,
      QString* log_path,
      QString* guard_path) {
    window->beginArchiveJobLog(configured_output_path, run_id);
    window->resolveArchiveJobLogPath(resolved_output_path);
    window->archive_finalize_source_path_ = resolved_output_path;
    if (log_path)
      *log_path = window->archive_job_log_path_;
    if (guard_path)
      *guard_path = window->archive_job_log_guard_path_;
    window->finishArchiveJobLogAfterFinalizationFailure();
  }
};

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

QString format_test_video_time_ns(qint64 nanoseconds) {
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

struct SliderStyleGeometry {
  int available_span{0};
  QPoint handle_center;
};

SliderStyleGeometry slider_style_geometry(const QSlider* slider) {
  if (!slider)
    return {};
  QStyleOptionSlider option;
  option.initFrom(slider);
  option.orientation = slider->orientation();
  option.minimum = slider->minimum();
  option.maximum = slider->maximum();
  option.sliderPosition = slider->sliderPosition();
  option.sliderValue = slider->value();
  option.singleStep = slider->singleStep();
  option.pageStep = slider->pageStep();
  option.tickPosition = slider->tickPosition();
  option.tickInterval = slider->tickInterval();
  option.upsideDown = slider->orientation() == Qt::Horizontal
      ? slider->invertedAppearance() != (option.direction == Qt::RightToLeft)
      : !slider->invertedAppearance();
  if (slider->orientation() == Qt::Horizontal)
    option.state |= QStyle::State_Horizontal;
  else
    option.state &= ~QStyle::State_Horizontal;
  const QRect handle =
      slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
  const QPoint handle_center = slider->orientation() == Qt::Horizontal
      ? QPoint(handle.left() + handle.width() / 2, handle.center().y())
      : QPoint(handle.center().x(), handle.top() + handle.height() / 2);
  return {
      slider->style()->pixelMetric(QStyle::PM_SliderSpaceAvailable, &option, slider),
      handle_center,
  };
}

bool write_tiff_fixture(const fs::path& path) {
  TIFF* tiff = TIFFOpen(path.c_str(), "w");
  if (tiff == nullptr)
    return false;
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 1);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 1);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  uint16_t value = 0;
  const bool written = TIFFWriteScanline(tiff, &value, 0, 0) >= 0;
  TIFFClose(tiff);
  return written;
}

absl::StatusOr<std::string> write_live_hugin_generation_fixture(const fs::path& game_dir) {
  auto artifact_lock = hm::stitching::lock_canvas_constraint_artifacts(game_dir);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto", "seam_file.png"}) {
    std::ofstream output(game_dir / name, std::ios::binary | std::ios::trunc);
    output << name << '\n';
    if (!output)
      return absl::InternalError("Could not write live-rotation Hugin fixture");
  }
  for (const char* name : {
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    if (!write_tiff_fixture(game_dir / name))
      return absl::InternalError("Could not write live-rotation Hugin fixture");
  }
  std::error_code ignored;
  fs::remove(game_dir / hm::stitching::kStitchCanvasProvenanceArtifact, ignored);
  return hm::stitching::stitch_artifact_generation_id_locked(game_dir);
}

std::optional<fs::path> find_test_baseline_yaml() {
  auto check = [](const fs::path& path) -> std::optional<fs::path> {
    std::error_code error;
    if (fs::is_regular_file(path, error) && !error)
      return path;
    return std::nullopt;
  };
  if (const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY"); workspace && *workspace) {
    if (auto found = check(fs::path(workspace) / "configs" / "baseline.yaml"))
      return found;
  }
  if (const char* test_srcdir = std::getenv("TEST_SRCDIR"); test_srcdir && *test_srcdir) {
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    if (auto found = check(
            fs::path(test_srcdir) / (test_workspace && *test_workspace ? test_workspace : "kstream") / "configs" /
            "baseline.yaml")) {
      return found;
    }
  }
  std::error_code error;
  for (fs::path path = fs::current_path(error); !error && !path.empty(); path = path.parent_path()) {
    if (auto found = check(path / "configs" / "baseline.yaml"))
      return found;
    if (path == path.parent_path())
      break;
  }
  return std::nullopt;
}

bool expect_x11_widget_state(
    QWidget* widget,
    bool expected_viewable,
    const std::string& description,
    bool require_geometry = true) {
#ifdef Q_OS_UNIX
  if (!widget || QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) != 0)
    return true;
  Display* display = XOpenDisplay(nullptr);
  if (!display)
    return expect(false, description + ": could not open the X11 display");
  XWindowAttributes attributes{};
  const Window native_window = static_cast<Window>(widget->winId());
  Window root = None;
  Window parent = None;
  Window* children = nullptr;
  unsigned int child_count = 0;
  const bool queried = XGetWindowAttributes(display, native_window, &attributes) != 0 &&
      XQueryTree(display, native_window, &root, &parent, &children, &child_count) != 0;
  if (children)
    XFree(children);
  const Window expected_parent = static_cast<Window>(widget->parentWidget()->winId());
  XCloseDisplay(display);
  if (!queried)
    return expect(false, description + ": could not query the native X11 window");
  const bool viewable = attributes.map_state == IsViewable;
  const qreal scale = widget->devicePixelRatioF();
  const int expected_x = qRound(widget->x() * scale);
  const int expected_y = qRound(widget->y() * scale);
  const int expected_width = qRound(widget->width() * scale);
  const int expected_height = qRound(widget->height() * scale);
  const bool geometry_matches = !require_geometry ||
      (attributes.x == expected_x && attributes.y == expected_y && attributes.width == expected_width &&
       attributes.height == expected_height);
  return expect(
      parent == expected_parent && geometry_matches && viewable == expected_viewable,
      description + ": native parent/geometry/map state differs from Qt (parent=" + std::to_string(parent) +
          " expected-parent=" + std::to_string(expected_parent) + " X11=" + std::to_string(attributes.x) + "," +
          std::to_string(attributes.y) + " " + std::to_string(attributes.width) + "x" +
          std::to_string(attributes.height) + " Qt=" + std::to_string(widget->x()) + "," + std::to_string(widget->y()) +
          " " + std::to_string(widget->width()) + "x" + std::to_string(widget->height()) +
          " scale=" + std::to_string(scale) + " viewable=" + (viewable ? "true" : "false") + ")");
#else
  (void)widget;
  (void)expected_viewable;
  (void)description;
  return true;
#endif
}

bool expect_x11_application_icon(QWidget* widget) {
#ifdef Q_OS_UNIX
  if (!widget || QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) != 0)
    return true;
  Display* display = XOpenDisplay(nullptr);
  if (!display)
    return expect(false, "Could not open X11 display to inspect the HStream application icon");
  XSync(display, False);
  const Atom icon_atom = XInternAtom(display, "_NET_WM_ICON", True);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long item_count = 0;
  unsigned long bytes_after = 0;
  unsigned char* data = nullptr;
  int status = icon_atom == None ? BadAtom
                                 : XGetWindowProperty(
                                       display,
                                       static_cast<Window>(widget->winId()),
                                       icon_atom,
                                       0,
                                       0,
                                       False,
                                       XA_CARDINAL,
                                       &actual_type,
                                       &actual_format,
                                       &item_count,
                                       &bytes_after,
                                       &data);
  if (data) {
    XFree(data);
    data = nullptr;
  }
  constexpr unsigned long kMaximumIconPropertyBytes = 4UL * 1024UL * 1024UL;
  const unsigned long property_bytes = bytes_after;
  if (status == Success && actual_type == XA_CARDINAL && actual_format == 32 && property_bytes > 0 &&
      property_bytes <= kMaximumIconPropertyBytes) {
    status = XGetWindowProperty(
        display,
        static_cast<Window>(widget->winId()),
        icon_atom,
        0,
        static_cast<long>((property_bytes + 3) / 4),
        False,
        XA_CARDINAL,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);
  } else {
    status = BadValue;
  }

  bool complete_payload = status == Success && actual_type == XA_CARDINAL && actual_format == 32 && data &&
      bytes_after == 0 && item_count >= 3;
  std::set<std::pair<unsigned long, unsigned long>> visible_sizes;
  if (complete_payload) {
    const auto* values = reinterpret_cast<const unsigned long*>(data);
    unsigned long index = 0;
    while (index < item_count) {
      if (item_count - index < 2) {
        complete_payload = false;
        break;
      }
      const unsigned long width = values[index++];
      const unsigned long height = values[index++];
      constexpr unsigned long kMaximumIconDimension = 4096;
      if (width == 0 || height == 0 || width > kMaximumIconDimension || height > kMaximumIconDimension ||
          width > (item_count - index) / height) {
        complete_payload = false;
        break;
      }
      const unsigned long pixel_count = width * height;
      bool has_visible_pixel = false;
      for (unsigned long pixel = 0; pixel < pixel_count; ++pixel) {
        has_visible_pixel = has_visible_pixel || ((values[index + pixel] >> 24U) & 0xffU) != 0;
      }
      if (has_visible_pixel)
        visible_sizes.emplace(width, height);
      index += pixel_count;
    }
    complete_payload = complete_payload && index == item_count;
  }
  const std::set<std::pair<unsigned long, unsigned long>> expected_sizes = {
      {16, 16}, {24, 24}, {32, 32}, {48, 48}, {64, 64}, {128, 128}};
  const bool exported = complete_payload &&
      std::includes(visible_sizes.begin(), visible_sizes.end(), expected_sizes.begin(), expected_sizes.end());
  if (data)
    XFree(data);

  XClassHint class_hint{};
  const bool class_hint_available = XGetClassHint(display, static_cast<Window>(widget->winId()), &class_hint) != 0;
  const std::string class_hint_name = class_hint.res_name ? class_hint.res_name : "<missing>";
  const std::string class_hint_class = class_hint.res_class ? class_hint.res_class : "<missing>";
  const std::string executable_name = QFileInfo(QCoreApplication::applicationFilePath()).fileName().toStdString();
  const bool class_hint_matches = class_hint_available && class_hint.res_name && class_hint.res_class &&
      class_hint_name == executable_name && class_hint_class == "hstream-ui";
  if (class_hint.res_name)
    XFree(class_hint.res_name);
  if (class_hint.res_class)
    XFree(class_hint.res_class);
  XCloseDisplay(display);
  if (!exported || !class_hint_matches) {
    std::cerr << "X11 application identity: complete-icon=" << complete_payload << " visible-sizes=";
    for (const auto& [width, height] : visible_sizes)
      std::cerr << width << 'x' << height << ',';
    std::cerr << " class-hint=" << class_hint_matches << " name=" << class_hint_name << " class=" << class_hint_class
              << '\n';
  }
  return expect(
      exported && class_hint_matches,
      "The native HStream window should export complete multi-size _NET_WM_ICON data and matching WM_CLASS identity");
#else
  (void)widget;
  return true;
#endif
}

bool capture_interaction_artifact(HStreamWindow* window, const QString& name) {
  const QString artifact_dir = qEnvironmentVariable("HSTREAM_UI_X11_ARTIFACT_DIR");
  if (artifact_dir.isEmpty())
    return true;
  if (!window || !window->screen() || !QDir().mkpath(artifact_dir))
    return expect(false, "Could not prepare the X11 interaction artifact directory");
  QApplication::processEvents();
  const QPixmap composed = window->screen()->grabWindow(window->winId());
  const QString path = QDir(artifact_dir).filePath(name);
  return expect(
      !composed.isNull() && composed.save(path), "Could not save composed X11 screenshot: " + path.toStdString());
}

bool expect_composed_focus_control(
    HStreamWindow* window,
    QPushButton* button,
    const std::optional<QPoint>& stale_center,
    const std::string& description) {
  if (!window || !button || QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) != 0)
    return true;
  int current_light_pixels = -1;
  int stale_light_pixels = -1;
  const QPoint current_center = button->mapTo(window, button->rect().center());
  for (int attempt = 0; attempt < 50; ++attempt) {
    QApplication::processEvents();
    QGuiApplication::sync();
    const QPixmap composed = window->screen()->grabWindow(window->winId());
    if (!composed.isNull()) {
      const QImage image = composed.toImage().convertToFormat(QImage::Format_RGB32);
      const qreal scale = composed.devicePixelRatio();
      auto light_pixels = [&image, scale](const QPoint& center) {
        const QPoint scaled(qRound(center.x() * scale), qRound(center.y() * scale));
        const int radius = std::max(1, qRound(12 * scale));
        int count = 0;
        for (int y = std::max(0, scaled.y() - radius); y < std::min(image.height(), scaled.y() + radius); ++y) {
          for (int x = std::max(0, scaled.x() - radius); x < std::min(image.width(), scaled.x() + radius); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() + pixel.green() + pixel.blue() >= 600)
              ++count;
          }
        }
        return count;
      };
      current_light_pixels = light_pixels(current_center);
      stale_light_pixels =
          stale_center.has_value() && *stale_center != current_center ? light_pixels(*stale_center) : 0;
      if (current_light_pixels >= 8 && stale_light_pixels <= 2)
        return true;
    }
    if (attempt + 1 < 50)
      QTest::qWait(10);
  }
  return expect(
      false,
      description + ": composed maximize glyph pixels current=" + std::to_string(current_light_pixels) +
          " stale=" + std::to_string(stale_light_pixels));
}

bool capture_widget_artifact(QWidget* widget, const QString& name) {
  const QString artifact_dir = qEnvironmentVariable("HSTREAM_UI_X11_ARTIFACT_DIR");
  if (artifact_dir.isEmpty())
    return true;
  if (!widget || !QDir().mkpath(artifact_dir))
    return expect(false, "Could not prepare the widget artifact directory");
  QApplication::processEvents();
  const QPixmap screenshot = widget->grab();
  const QString path = QDir(artifact_dir).filePath(name);
  return expect(
      !screenshot.isNull() && screenshot.save(path), "Could not save widget screenshot: " + path.toStdString());
}

bool test_path_scoped_auto_rollback() {
  YAML::Node before(YAML::NodeType::Map);
  before["hstream_ui"]["video_roles"]["left"].push_back("left.mp4");
  before["game"]["videos"]["left"].push_back("left.mp4");
  before["game"]["stitching"]["frame_offsets"]["left"] = "3";

  YAML::Node latest(YAML::NodeType::Map);
  latest["concurrent"]["keep"] = true;
  latest["hstream_ui"]["copied_imports"].push_back("copied.mp4");
  hm::ui_internal::restore_auto_selection_paths(latest, before);

  return expect(
      latest["concurrent"]["keep"].as<bool>() &&
          latest["hstream_ui"]["video_roles"]["left"][0].as<std::string>() == "left.mp4" &&
          latest["game"]["videos"]["left"][0].as<std::string>() == "left.mp4" &&
          latest["game"]["stitching"]["frame_offsets"]["left"].as<std::string>() == "3" &&
          latest["hstream_ui"]["copied_imports"][0].as<std::string>() == "copied.mp4",
      "Auto cleanup rollback must restore owned paths without replacing an intervening unrelated update");
}

bool test_matching_development_runtime_selection() {
  QTemporaryDir temporary_root;
  if (!temporary_root.isValid())
    return expect(false, "Could not create a temporary Bazel runtime layout");

  const fs::path workspace_root = fs::path(temporary_root.path().toStdString()) / "workspace";
  const fs::path execution_root = fs::path(temporary_root.path().toStdString()) / "output-base/execroot/synthetic";
  const fs::path output_apps = execution_root / "bazel-out" / "k8-opt" / "bin" / "src" / "apps";
  const fs::path application = output_apps / "hstream-ui" / "hstream-ui";
  const fs::path matching_runner = output_apps / "pipeline-app" / "hstream-cli";
  const fs::path unrelated_runner = workspace_root / "bazel-bin" / "src" / "apps" / "pipeline-app" / "hstream-cli";
  const std::vector<fs::path> runtime_artifacts = {
      execution_root / "bazel-out/k8-opt/bin/src/gst-plugins/gst-dsxvideoconvert/libgstdsxvideoconvert.so",
      execution_root / "bazel-out/k8-opt/bin/src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so",
      execution_root / "bazel-out/k8-opt/bin/src/gst-plugins/gst-playtracker/libgstplaytracker.so",
      execution_root / "bazel-out/k8-opt/bin/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so",
      execution_root / "bazel-out/k8-opt/bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so",
  };
  std::error_code error;
  fs::create_directories(application.parent_path(), error);
  fs::create_directories(matching_runner.parent_path(), error);
  fs::create_directories(unrelated_runner.parent_path(), error);
  fs::create_directories(workspace_root / "configs", error);
  fs::create_directories(workspace_root / "src", error);
  if (error)
    return expect(false, "Could not create a synthetic Bazel runtime layout: " + error.message());
  fs::create_directory_symlink(workspace_root / "src", execution_root / "src", error);
  if (error)
    return expect(false, "Could not create a synthetic Bazel source-package link: " + error.message());
  for (const fs::path& path : {application, matching_runner, unrelated_runner}) {
    std::ofstream file(path);
    file << "synthetic executable\n";
    file.close();
    fs::permissions(
        path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace, error);
    if (error)
      return expect(false, "Could not make a synthetic Bazel runtime executable: " + error.message());
  }
  for (const fs::path& path : runtime_artifacts) {
    fs::create_directories(path.parent_path(), error);
    if (error)
      return expect(false, "Could not create a synthetic runtime artifact directory: " + error.message());
    std::ofstream(path) << "synthetic runtime artifact\n";
  }
  {
    std::ofstream workspace_marker(workspace_root / "WORKSPACE.bazel");
    workspace_marker << "workspace(name = \"synthetic\")\n";
  }

  const QString selected_runner =
      hm::ui_internal::matching_development_pipeline_runner(QString::fromStdString(application.string()));
  const QString selected_bazel_bin =
      hm::ui_internal::matching_development_bazel_bin(QString::fromStdString(application.string()));
  const QString selected_root =
      hm::ui_internal::development_runtime_root_for_application(QString::fromStdString(application.string()));
  const bool selected_ok = expect(
                               QFileInfo(selected_runner).canonicalFilePath() ==
                                   QFileInfo(QString::fromStdString(matching_runner.string())).canonicalFilePath(),
                               "A Bazel-built UI must select the sibling CLI from its immutable output tree") &&
      expect(QFileInfo(selected_runner).canonicalFilePath() !=
                 QFileInfo(QString::fromStdString(unrelated_runner.string())).canonicalFilePath(),
             "A changed bazel-bin output must not redirect an already-running UI to another CLI") &&
      expect(QFileInfo(selected_bazel_bin).canonicalFilePath() ==
                 QFileInfo(QString::fromStdString((output_apps.parent_path().parent_path()).string()))
                     .canonicalFilePath(),
             "A Bazel-built UI must retain the immutable output tree used for plugins and runtime libraries") &&
      expect(QFileInfo(selected_root).canonicalFilePath() ==
                 QFileInfo(QString::fromStdString(workspace_root.string())).canonicalFilePath(),
             "A Bazel-built UI must recover its source workspace for configs and the pipeline working directory") &&
      expect(hm::ui_internal::matching_development_pipeline_runner("/opt/hstream/bin/hstream-ui").isEmpty() &&
                 hm::ui_internal::matching_development_bazel_bin("/opt/hstream/bin/hstream-ui").isEmpty() &&
                 hm::ui_internal::development_runtime_root_for_application("/opt/hstream/bin/hstream-ui").isEmpty(),
             "An installed UI must not be mistaken for a Bazel development runtime");
  if (!selected_ok)
    return false;

  const QString selected_runtime_error = hm::ui_internal::missing_development_runtime_artifact(selected_bazel_bin);
  if (!expect(selected_runtime_error.isEmpty(), "A complete matching Bazel runtime must pass validation"))
    return false;
  fs::remove(runtime_artifacts.front(), error);
  if (error)
    return expect(false, "Could not remove a synthetic runtime artifact: " + error.message());
  if (!expect(
          hm::ui_internal::missing_development_runtime_artifact(selected_bazel_bin)
              .endsWith("libgstdsxvideoconvert.so"),
          "A Bazel UI must identify a missing matching runtime artifact before Play"))
    return false;

  fs::remove(matching_runner, error);
  if (error)
    return expect(false, "Could not remove the matching synthetic CLI: " + error.message());
  return expect(
             hm::ui_internal::matching_development_pipeline_runner(QString::fromStdString(application.string()))
                 .isEmpty(),
             "A missing sibling CLI must not redirect a Bazel-built UI to another runner") &&
      expect(QFileInfo(hm::ui_internal::matching_development_bazel_bin(QString::fromStdString(application.string())))
                     .canonicalFilePath() ==
                 QFileInfo(QString::fromStdString((output_apps.parent_path().parent_path()).string()))
                     .canonicalFilePath(),
             "A Bazel-built UI must retain its immutable output tree when the sibling CLI is missing") &&
      expect(QFileInfo(
                 hm::ui_internal::development_runtime_root_for_application(
                     QString::fromStdString(application.string())))
                     .canonicalFilePath() ==
                 QFileInfo(QString::fromStdString(workspace_root.string())).canonicalFilePath(),
             "A Bazel-built UI must retain its source workspace when the sibling CLI is missing");
}

bool test_stitching_canvas_constraint_decisions() {
  const auto unchanged = hm::ui_internal::decide_stitching_canvas_constraint_change(
      /*width_changed=*/false, /*artifacts_compatible=*/std::nullopt, /*requires_regeneration=*/std::nullopt);
  const auto reusable = hm::ui_internal::decide_stitching_canvas_constraint_change(
      /*width_changed=*/true, /*artifacts_compatible=*/true, /*requires_regeneration=*/false);
  const auto missing = hm::ui_internal::decide_stitching_canvas_constraint_change(
      /*width_changed=*/true, /*artifacts_compatible=*/false, /*requires_regeneration=*/false);
  const auto stale = hm::ui_internal::decide_stitching_canvas_constraint_change(
      /*width_changed=*/true, /*artifacts_compatible=*/false, /*requires_regeneration=*/true);
  const auto failed_check = hm::ui_internal::decide_stitching_canvas_constraint_change(
      /*width_changed=*/true, /*artifacts_compatible=*/std::nullopt, /*requires_regeneration=*/std::nullopt);
  return expect(
      !unchanged.calibration_required && !unchanged.cleanup_required && !reusable.calibration_required &&
          !reusable.cleanup_required && missing.calibration_required && !missing.cleanup_required &&
          stale.calibration_required && stale.cleanup_required && failed_check.calibration_required &&
          failed_check.cleanup_required,
      "UI width edits must reuse compatible maps, avoid cleaning absent maps, and fail closed on stale or unknown maps");
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

bool lookup_yaml_path_at(
    const YAML::Node& node,
    const std::vector<const char*>& path,
    size_t index,
    YAML::Node* value) {
  if (index >= path.size()) {
    if (value) {
      *value = node;
    }
    return true;
  }
  YAML::Node next;
  if (node.IsSequence()) {
    std::string segment(path[index]);
    if (segment.empty() || segment.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    const size_t sequence_index = static_cast<size_t>(std::stoul(segment));
    if (sequence_index >= node.size()) {
      return false;
    }
    next = node[sequence_index];
  } else if (!lookup_yaml_key(node, path[index], &next)) {
    return false;
  }
  return lookup_yaml_path_at(next, path, index + 1, value);
}

bool lookup_yaml_path(YAML::Node root, std::initializer_list<const char*> path, YAML::Node* value) {
  return lookup_yaml_path_at(root, std::vector<const char*>(path), 0, value);
}

template <typename T>
T* require_child(HStreamWindow* window, const char* name) {
  T* child = window->findChild<T*>(name);
  if (!child) {
    std::cerr << "Missing child: " << name << '\n';
  }
  return child;
}

void activate(QAbstractButton* button) {
  button->click();
  QApplication::processEvents();
}

bool select_list_item(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text().contains(text)) {
      list->setCurrentRow(i);
      return true;
    }
  }
  std::cerr << "Missing list item containing: " << text.toStdString() << '\n';
  return false;
}

bool list_contains(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text().contains(text)) {
      return true;
    }
  }
  return false;
}

int list_match_count(QListWidget* list, const QString& text) {
  int matches = 0;
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text().contains(text)) {
      ++matches;
    }
  }
  return matches;
}

bool write_fake_video(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << "Failed to create fake video: " << path.toStdString() << '\n';
    return false;
  }
  file.write("hstream-ui-test-video");
  return true;
}

bool write_fake_runner(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << "Failed to create fake runner: " << path.toStdString() << '\n';
    return false;
  }
  file.write("#!/usr/bin/env python3\n");
  file.write("import os\n");
  file.write("import select\n");
  file.write("import sys\n");
  file.write("import time\n");
  file.write("for arg in sys.argv[1:]:\n");
  file.write("    print(arg, flush=True)\n");
  file.write("print('USE_NEW_NVSTREAMMUX=' + os.environ.get('USE_NEW_NVSTREAMMUX', ''), flush=True)\n");
  file.write("print('HM_RENDER_SINK=' + os.environ.get('HM_RENDER_SINK', ''), flush=True)\n");
  file.write("print('HM_NO_SCOREBOARD=' + os.environ.get('HM_NO_SCOREBOARD', ''), flush=True)\n");
  file.write("print('HM_MAX_CONTROL_POINTS=' + os.environ.get('HM_MAX_CONTROL_POINTS', ''), flush=True)\n");
  file.write("print('HM_OUTPUT_WORK_DIR=' + os.environ.get('HM_OUTPUT_WORK_DIR', ''), flush=True)\n");
  file.write("print('HSTREAM_ARCHIVE_RUN_ID=' + os.environ.get('HSTREAM_ARCHIVE_RUN_ID', ''), flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH'):\n");
  file.write("    archive_path = os.environ['HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH']\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_ARCHIVE_RECOVER_EXISTING') == '1':\n");
  file.write("        try:\n");
  file.write("            if os.path.getsize(archive_path) > 0:\n");
  file.write("                stem, extension = os.path.splitext(archive_path)\n");
  file.write("                recovery_path = stem + '-finalization-failed' + extension\n");
  file.write("                suffix = 0\n");
  file.write("                while os.path.exists(recovery_path):\n");
  file.write("                    suffix += 1\n");
  file.write("                    recovery_path = stem + '-finalization-failed-' + str(suffix) + extension\n");
  file.write("                os.rename(archive_path, recovery_path)\n");
  file.write(
      "                print('HSTREAM_OUTPUT_RECOVERY type=archive sink=2 path=' + recovery_path, flush=True)\n");
  file.write("        except FileNotFoundError:\n");
  file.write("            pass\n");
  file.write("    try:\n");
  file.write("        archive_stat = os.stat(archive_path)\n");
  file.write(
      "        archive_before = 'existed=1 size=%d mtime-ms=%d' % "
      "(archive_stat.st_size, archive_stat.st_mtime_ns // 1000000)\n");
  file.write("    except FileNotFoundError:\n");
  file.write("        archive_before = 'existed=0 size=-1 mtime-ms=-1'\n");
  file.write(
      "    print('HSTREAM_OUTPUT type=archive sink=2 ' + archive_before + ' codec=hevc path=' + archive_path, flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH'):\n");
  file.write("    stitched_archive_path = os.environ['HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH']\n");
  file.write("    try:\n");
  file.write("        stitched_archive_stat = os.stat(stitched_archive_path)\n");
  file.write(
      "        stitched_archive_before = 'existed=1 size=%d mtime-ms=%d' % "
      "(stitched_archive_stat.st_size, stitched_archive_stat.st_mtime_ns // 1000000)\n");
  file.write("    except FileNotFoundError:\n");
  file.write("        stitched_archive_before = 'existed=0 size=-1 mtime-ms=-1'\n");
  file.write(
      "    print('HSTREAM_OUTPUT type=archive sink=5 kind=stitched ' + stitched_archive_before + "
      "' codec=hevc path=' + stitched_archive_path, flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_TELEMETRY_MANIFEST'):\n");
  file.write(
      "    print('HSTREAM_TELEMETRY manifest=' + os.environ['HSTREAM_UI_TEST_TELEMETRY_MANIFEST'], flush=True)\n");
  file.write(
      "print('HSTREAM_CALIBRATION_INVALIDATION_ID=' + "
      "os.environ.get('HSTREAM_CALIBRATION_INVALIDATION_ID', ''), flush=True)\n");
  file.write("print('LD_LIBRARY_PATH=' + os.environ.get('LD_LIBRARY_PATH', ''), flush=True)\n");
  file.write("print('HSTREAM_RENDER_AUDIO_MUTED=' + os.environ.get('HSTREAM_RENDER_AUDIO_MUTED', ''), flush=True)\n");
  file.write("if os.environ.get('HM_NO_SCOREBOARD') != '1':\n");
  file.write("    print('Scoreboard corners are not configured. Open this private, expiring URL:', flush=True)\n");
  file.write("    print('  http://127.0.0.1:45678/?token=' + ('a' * 64), flush=True)\n");
  file.write("    print('Scoreboard overlay disabled by config reload', flush=True)\n");
  file.write("if '--clean' in sys.argv[1:] or '--clean-from-control-points' in sys.argv[1:]:\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_CLEAN_INVALIDATE_INPUT') == '1':\n");
  file.write("        game_id = sys.argv[sys.argv.index('-g') + 1]\n");
  file.write("        config_path = os.path.join(os.environ['HM_GAME_DIR'], game_id, 'config.yaml')\n");
  file.write("        with open(config_path, 'r', encoding='utf-8') as source:\n");
  file.write("            config_text = source.read()\n");
  file.write("        config_text = config_text.replace('stale_from: features', 'stale_from: input')\n");
  file.write(
      "        config_text = ''.join(line for line in config_text.splitlines(True) if 'invalidation_id:' not in line)\n");
  file.write("        with open(config_path, 'w', encoding='utf-8') as destination:\n");
  file.write("            destination.write(config_text)\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_CLEAN_RESULT') == 'failure':\n");
  file.write("        print('clean runner forced failure', flush=True)\n");
  file.write("        sys.exit(8)\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_SIMULATE_CLEAN_ARTIFACTS') == '1':\n");
  file.write("        import glob\n");
  file.write("        game_id = sys.argv[sys.argv.index('-g') + 1]\n");
  file.write("        game_dir = os.path.join(os.environ['HM_GAME_DIR'], game_id)\n");
  file.write("        for pattern in ['hm_project.pto', 'autooptimiser_out.pto', '*.pto', 'mapping_*.tif',\n");
  file.write("                        'mapping_*.tiff', 'stitching_canvas_provenance', 'stitch_generation',\n");
  file.write("                        'panorama.tif', 'seam_file.png', 'matches.png', 'keypoints.png', 's.png',\n");
  file.write("                        'rink_mask_*.png', 'left.png', 'right.png']:\n");
  file.write("            for path in glob.glob(os.path.join(game_dir, pattern)):\n");
  file.write("                try:\n");
  file.write("                    os.remove(path)\n");
  file.write("                except FileNotFoundError:\n");
  file.write("                    pass\n");
  file.write("    print('clean runner exiting', flush=True)\n");
  file.write("    sys.exit(0)\n");
  file.write("sys.stdout.write('\\033[34mANSI')\n");
  file.write("sys.stdout.flush()\n");
  file.write("time.sleep(0.05)\n");
  file.write("sys.stdout.write(' blue runner line\\033[0m\\n')\n");
  file.write("sys.stdout.flush()\n");
  file.write(
      "print('HSTREAM_STARTUP stage=stitching message=Discovering source chapters and validating saved stitching "
      "artifacts', flush=True)\n");
  file.write("time.sleep(float(os.environ.get('HSTREAM_UI_TEST_STARTUP_DELAY_MS', '0')) / 1000.0)\n");
  file.write(
      "print('HSTREAM_STARTUP stage=decoding message=Starting decoders and waiting for the first frame', "
      "flush=True)\n");
  file.write(
      "print('HSTREAM_PROGRESS processed_ns=42000000000 total_ns=600000000000 remaining_ns=558000000000 "
      "eta_ns=279000000000 speed_x=2.000000 fps=42.750000 fps_avg=40.500000 fraction=0.070000 stage=0 "
      "instance=aggregate instances=1 generation=0', "
      "flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS'):\n");
  file.write(
      "    if os.environ.get('HSTREAM_UI_TEST_ARCHIVE_WRITE') and os.environ.get('HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH'):\n");
  file.write("        with open(os.environ['HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH'], 'wb') as archive:\n");
  file.write("            archive.write(b'completed lossless archive')\n");
  file.write(
      "    if os.environ.get('HSTREAM_UI_TEST_ARCHIVE_WRITE') and "
      "os.environ.get('HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH'):\n");
  file.write("        with open(os.environ['HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH'], 'wb') as archive:\n");
  file.write("            archive.write(b'completed stitched archive')\n");
  file.write("    sys.exit(int(os.environ['HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS']))\n");
  file.write(
      "if os.environ.get('HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW') == '1' or any(argument.startswith("
      "'--ui-preview-windows=') for argument in sys.argv[1:]):\n");
  file.write("    initial_preview = 'program'\n");
  file.write("    for argument in sys.argv[1:]:\n");
  file.write("        if argument.startswith('--ui-preview-active='):\n");
  file.write("            initial_preview = argument.split('=', 1)[1]\n");
  file.write(
      "    print('HSTREAM_PREVIEW_RUNTIME status=ready channel=' + initial_preview + ' generation=2', flush=True)\n");
  file.write("    if initial_preview != 'none':\n");
  file.write(
      "        print('HSTREAM_PREVIEW channel=' + initial_preview + ' status=activated generation=2 message=GPU "
      "preview branch re-armed', flush=True)\n");
  file.write("        if int(os.environ.get('HSTREAM_UI_TEST_PREVIEW_READY_AFTER', '0')) == 0:\n");
  file.write(
      "            resolution = '1920x1080' if initial_preview == 'program' else '4096x1080'\n"
      "            print('HSTREAM_PREVIEW channel=' + initial_preview + ' status=ready generation=2 message=first "
      "GPU frame presented resolution=' + resolution, flush=True)\n");
  file.write("calibration_result = os.environ.get('HSTREAM_UI_TEST_CALIBRATION_RESULT', '')\n");
  file.write("stitching_only = '--stitching-calibration-only' in sys.argv[1:]\n");
  file.write("if not calibration_result and os.environ.get('HSTREAM_UI_TEST_COMPLETE_CALIBRATION') == '1':\n");
  file.write("    calibration_result = 'success'\n");
  file.write("if calibration_result in ('success', 'failure', 'exit', 'diagnostic-exit'):\n");
  file.write("    time.sleep(float(os.environ.get('HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS', '0')) / 1000.0)\n");
  file.write("    delay = float(os.environ.get('HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS', '0')) / 1000.0\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_PRECALIBRATION_STDERR'):\n");
  file.write("        print(os.environ['HSTREAM_UI_TEST_PRECALIBRATION_STDERR'], file=sys.stderr, flush=True)\n");
  file.write(
      "        time.sleep(float(os.environ.get('HSTREAM_UI_TEST_AFTER_PRECALIBRATION_STDERR_DELAY_MS', '0')) / "
      "1000.0)\n");
  file.write("    events = []\n");
  file.write("    if os.environ.get('HSTREAM_CALIBRATION_START_STAGE') != 'features':\n");
  file.write("        events = [\n");
  file.write(
      "            'HSTREAM_CALIBRATION stage=input status=started message=Waiting for synchronized frames from both "
      "cameras',\n");
  file.write(
      "            'HSTREAM_CALIBRATION stage=input status=complete message=Captured synchronized frames from both "
      "cameras',\n");
  file.write(
      "            'HSTREAM_CALIBRATION stage=orientation status=started message=Looking for the ice rink and camera "
      "orientation',\n");
  file.write("        ]\n");
  file.write("    for event in events:\n");
  file.write("        print(event, flush=True)\n");
  file.write("        time.sleep(delay)\n");
  file.write("    if calibration_result == 'exit':\n");
  file.write("        sys.exit(9)\n");
  file.write("    if calibration_result == 'diagnostic-exit':\n");
  file.write(
      "        sys.stderr.write('Skipping pooled stitching calibration: FAILED_PRECONDITION: OpenCV transform has "
      "unsafe canvas extent')\n");
  file.write("        sys.stderr.flush()\n");
  file.write("        sys.exit(12)\n");
  file.write("    events = []\n");
  file.write("    if os.environ.get('HSTREAM_CALIBRATION_START_STAGE') != 'features':\n");
  file.write(
      "        events.append('HSTREAM_CALIBRATION stage=orientation status=complete message=Camera orientation is configured')\n");
  file.write("    events += [\n");
  file.write(
      "        'HSTREAM_CALIBRATION stage=features status=started message=Looking for control points in both camera "
      "frames',\n");
  file.write(
      "        'HSTREAM_CALIBRATION stage=features status=complete message=Control points found in both camera "
      "frames',\n");
  file.write(
      "        'HSTREAM_CALIBRATION stage=matching status=started message=Selecting and validating control-point "
      "matches',\n");
  file.write("        'HSTREAM_CALIBRATION stage=matching status=complete message=Matched 750 control points',\n");
  file.write(
      "        'HSTREAM_CALIBRATION stage=optimizer status=started message=Running panorama optimizer "
      "(autooptimiser)',\n");
  file.write("    ]\n");
  file.write("    for event in events:\n");
  file.write("        print(event, flush=True)\n");
  file.write("        time.sleep(delay)\n");
  file.write("    if calibration_result == 'failure':\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=calibration status=failed message=autooptimiser rejected the "
      "control-point geometry', flush=True)\n");
  file.write("    else:\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=optimizer status=complete message=Panorama alignment optimized', "
      "flush=True)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=canvas status=started message=Building stitch maps and panorama "
      "preview', flush=True)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=canvas status=complete message=Stitch maps and panorama preview are "
      "ready', flush=True)\n");
  file.write("        if not stitching_only:\n");
  file.write(
      "            print('HSTREAM_CALIBRATION stage=rink-mask status=started message=Looking for the ice surface in "
      "the stitched panorama', flush=True)\n");
  file.write(
      "            print('HSTREAM_CALIBRATION stage=rink-mask status=complete message=Ice surface calibration is "
      "ready', flush=True)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=calibration status=complete message=Stitching calibration is "
      "complete', flush=True)\n");
  file.write("        print('hmstitcher: one-pass stitching configuration complete', flush=True)\n");
  file.write("        time.sleep(float(os.environ.get('HSTREAM_UI_TEST_PLAYBACK_RESTART_DELAY_MS', '0')) / 1000.0)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=playback-restart status=complete message=Playback restarted', "
      "flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_CLOSE_STDIN') == '1':\n");
  file.write("    sys.stdin.close()\n");
  file.write("    time.sleep(5.0)\n");
  file.write("    sys.exit(0)\n");
  file.write("def handle_stdin_line(line):\n");
  file.write(
      "    global preview_activation_count, preview_disable_stalled, stall_next_progress_reset, "
      "delayed_progress_generation, drop_progress_resets, stall_next_seek, delayed_seek_position, "
      "delayed_seek_generation, timeout_next_seek, reject_next_seek, backend_seek_position, "
      "reject_next_preview_overlays, delay_next_preview_overlays, delayed_preview_overlay_responses, "
      "runtime_control_delay_seconds, "
      "reject_next_runtime_control\n");
  file.write("    print('stdin:' + line.rstrip('\\n'), flush=True)\n");
  file.write("    if line.startswith('@test-exit'):\n");
  file.write("        print('test process exit requested', flush=True)\n");
  file.write("        sys.exit(0)\n");
  file.write("    if line.startswith('@test-stall-preview-disable'):\n");
  file.write("        preview_disable_stalled = True\n");
  file.write("        print('test preview disable stalled', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-reject-preview-overlays'):\n");
  file.write("        reject_next_preview_overlays = True\n");
  file.write("        print('test preview overlay rejection armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-delay-preview-overlays'):\n");
  file.write("        delay_next_preview_overlays = True\n");
  file.write("        print('test preview overlay delay armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-complete-preview-overlays'):\n");
  file.write("        if delayed_preview_overlay_responses:\n");
  file.write("            print(delayed_preview_overlay_responses.pop(0), flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-delay-runtime-control '):\n");
  file.write("        runtime_control_delay_seconds = float(line.rstrip('\\n').split(' ')[1]) / 1000.0\n");
  file.write("        print('test runtime control delay armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-reject-runtime-control'):\n");
  file.write("        reject_next_runtime_control = True\n");
  file.write("        print('test runtime control rejection armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-drop-progress-resets'):\n");
  file.write("        drop_progress_resets = True\n");
  file.write("        print('test progress resets dropped', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-resume-progress-resets'):\n");
  file.write("        drop_progress_resets = False\n");
  file.write("        print('test progress resets resumed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-stall-progress-reset'):\n");
  file.write("        stall_next_progress_reset = True\n");
  file.write("        print('test progress reset stalled', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-resume-preview-disable'):\n");
  file.write("        preview_disable_stalled = False\n");
  file.write("        print('test preview disable resumed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-stall-seek'):\n");
  file.write("        stall_next_seek = True\n");
  file.write("        print('test seek acknowledgement stalled', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-timeout-seek'):\n");
  file.write("        timeout_next_seek = True\n");
  file.write("        print('test seek reconstruction timeout armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-reject-seek'):\n");
  file.write("        reject_next_seek = True\n");
  file.write("        print('test seek rejection armed', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-set-backend-position '):\n");
  file.write("        backend_seek_position = int(line.rstrip('\\n').split(' ', 1)[1])\n");
  file.write("        print('test backend position set to ' + str(backend_seek_position), flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-complete-seek'):\n");
  file.write("        if delayed_seek_generation:\n");
  file.write(
      "            print('HSTREAM_SEEK status=ok generation=' + delayed_seek_generation + ' position_ns=' + "
      "delayed_seek_position, flush=True)\n");
  file.write("            delayed_seek_position = ''\n");
  file.write("            delayed_seek_generation = ''\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-preview-status '):\n");
  file.write("        _, channel, status, generation = line.rstrip('\\n').split(' ', 3)\n");
  file.write(
      "        print('HSTREAM_PREVIEW channel=' + channel + ' status=' + status + ' generation=' + generation + "
      "' message=synthetic review regression', flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@test-preview-runtime-ready '):\n");
  file.write("        _, channel, generation = line.rstrip('\\n').split(' ', 2)\n");
  file.write(
      "        print('HSTREAM_PREVIEW_RUNTIME status=ready channel=' + channel + ' generation=' + generation, "
      "flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@reset-progress-rate'):\n");
  file.write("        generation = line.rstrip('\\n').split(' ', 1)[1]\n");
  file.write("        if drop_progress_resets:\n");
  file.write("            return\n");
  file.write("        if stall_next_progress_reset:\n");
  file.write("            stall_next_progress_reset = False\n");
  file.write("            delayed_progress_generation = generation\n");
  file.write("            return\n");
  file.write("        if delayed_progress_generation:\n");
  file.write(
      "            print('HSTREAM_PROGRESS status=reset generation=' + delayed_progress_generation + "
      "' stage=0 instance=aggregate instances=2', flush=True)\n");
  file.write(
      "            print('HSTREAM_PROGRESS processed_ns=43000000000 total_ns=600000000000 "
      "remaining_ns=557000000000 eta_ns=1114000000000 speed_x=0.500000 fraction=0.071667 stage=0 "
      "instance=aggregate instances=2 generation=' + delayed_progress_generation, flush=True)\n");
  file.write("            delayed_progress_generation = ''\n");
  file.write(
      "        print('HSTREAM_PROGRESS processed_ns=43000000000 total_ns=600000000000 remaining_ns=557000000000 "
      "eta_ns=1114000000000 speed_x=0.500000 fraction=0.071667 stage=0 instance=aggregate instances=2 "
      "generation=' + generation, "
      "flush=True)\n");
  file.write(
      "        print('HSTREAM_PROGRESS status=reset generation=' + generation + "
      "' stage=0 instance=aggregate instances=2', flush=True)\n");
  file.write(
      "        print('HSTREAM_PROGRESS processed_ns=44000000000 total_ns=600000000000 remaining_ns=556000000000 "
      "eta_ns=unknown speed_x=0.000000 fraction=0.073333 stage=0 instance=aggregate instances=2 generation=' + "
      "generation, "
      "flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@seek-relative '):\n");
  file.write("        _, delta_ns, generation = line.rstrip('\\n').split(' ', 2)\n");
  file.write("        requested_seek_position = max(0, min(600000000000, backend_seek_position + int(delta_ns)))\n");
  file.write("        position_ns = str(requested_seek_position)\n");
  file.write("        if timeout_next_seek:\n");
  file.write("            timeout_next_seek = False\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write(
      "            print('HSTREAM_SEEK status=failed generation=' + generation + "
      "' reason=pipeline-recreate-timeout', flush=True)\n");
  file.write("            time.sleep(0.25)\n");
  file.write("            print('Pipeline running', flush=True)\n");
  file.write("            time.sleep(0.25)\n");
  file.write("            print('HSTREAM_SEEK_RECOVERY status=ready generation=' + generation, flush=True)\n");
  file.write("            return\n");
  file.write("        if stall_next_seek:\n");
  file.write("            stall_next_seek = False\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write("            delayed_seek_position = position_ns\n");
  file.write("            delayed_seek_generation = generation\n");
  file.write("            return\n");
  file.write("        if reject_next_seek or os.environ.get('HSTREAM_UI_TEST_REJECT_SEEK') == '1':\n");
  file.write("            reject_next_seek = False\n");
  file.write(
      "            print('HSTREAM_SEEK status=rejected generation=' + generation + "
      "' reason=nonlocal-output-active', flush=True)\n");
  file.write("        else:\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write(
      "            print('HSTREAM_SEEK status=ok generation=' + generation + ' position_ns=' + position_ns, "
      "flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@seek '):\n");
  file.write("        _, position_ns, generation = line.rstrip('\\n').split(' ', 2)\n");
  file.write("        requested_seek_position = int(position_ns)\n");
  file.write("        if timeout_next_seek:\n");
  file.write("            timeout_next_seek = False\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write(
      "            print('HSTREAM_SEEK status=failed generation=' + generation + "
      "' reason=pipeline-recreate-timeout', flush=True)\n");
  file.write("            time.sleep(0.25)\n");
  file.write("            print('Pipeline running', flush=True)\n");
  file.write("            time.sleep(0.25)\n");
  file.write("            print('HSTREAM_SEEK_RECOVERY status=ready generation=' + generation, flush=True)\n");
  file.write("            return\n");
  file.write("        if stall_next_seek:\n");
  file.write("            stall_next_seek = False\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write("            delayed_seek_position = position_ns\n");
  file.write("            delayed_seek_generation = generation\n");
  file.write("            return\n");
  file.write("        if reject_next_seek or os.environ.get('HSTREAM_UI_TEST_REJECT_SEEK') == '1':\n");
  file.write("            reject_next_seek = False\n");
  file.write(
      "            print('HSTREAM_SEEK status=rejected generation=' + generation + "
      "' reason=nonlocal-output-active', flush=True)\n");
  file.write("        else:\n");
  file.write("            backend_seek_position = requested_seek_position\n");
  file.write(
      "            print('HSTREAM_SEEK status=ok generation=' + generation + ' position_ns=' + position_ns, "
      "flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@set-preview-active '):\n");
  file.write("        _, channel, generation = line.rstrip('\\n').split(' ', 2)\n");
  file.write("        preview_activation_count += 1\n");
  file.write("        if channel == 'none':\n");
  file.write("            if preview_disable_stalled:\n");
  file.write("                print('test preview disable acknowledgement suppressed', flush=True)\n");
  file.write("                return\n");
  file.write(
      "            print('HSTREAM_PREVIEW channel=none status=deactivated generation=' + generation + "
      "' message=all GPU preview branches are inactive', flush=True)\n");
  file.write("            return\n");
  file.write(
      "        print('HSTREAM_PREVIEW channel=' + channel + ' status=activated generation=' + generation + "
      "' message=GPU preview branch activated', flush=True)\n");
  file.write("        ready_after = int(os.environ.get('HSTREAM_UI_TEST_PREVIEW_READY_AFTER', '0'))\n");
  file.write("        if ready_after and preview_activation_count >= ready_after:\n");
  file.write(
      "            resolution = '1920x1080' if channel == 'program' else ('4096x1080' if channel == 'stitched' else '3840x2160')\n"
      "            print('HSTREAM_PREVIEW channel=' + channel + ' status=ready generation=' + generation + "
      "' message=first GPU frame presented resolution=' + resolution, flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@set-preview-overlays '):\n");
  file.write("        _, generation, players, play, rink = line.rstrip('\\n').split(' ')\n");
  file.write("        status = 'failed' if reject_next_preview_overlays else 'applied'\n");
  file.write("        reject_next_preview_overlays = False\n");
  file.write(
      "        response = 'HSTREAM_PREVIEW_OVERLAYS status=' + status + ' generation=' + generation + "
      "' players=' + players + ' play=' + play + ' rink=' + rink\n");
  file.write("        if status == 'failed': response += ' reason=injected-rejection'\n");
  file.write("        if delay_next_preview_overlays:\n");
  file.write("            delay_next_preview_overlays = False\n");
  file.write("            delayed_preview_overlay_responses.append(response)\n");
  file.write("            return\n");
  file.write("        print(response, flush=True)\n");
  file.write("        return\n");
  file.write("    if line.startswith('@set-properties '):\n");
  file.write("        updates = line.rstrip('\\n').split(' ', 1)[1].split(';')\n");
  file.write(
      "        reject = reject_next_runtime_control or "
      "os.environ.get('HSTREAM_UI_TEST_REJECT_RUNTIME_CONTROL') == '1'\n");
  file.write("        reject_next_runtime_control = False\n");
  file.write("        stall = os.environ.get('HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL') == '1'\n");
  file.write("        if reject and updates:\n");
  file.write(
      "            time.sleep(float(os.environ.get('HSTREAM_UI_TEST_RUNTIME_REJECTION_DELAY_MS', '0')) / 1000.0)\n");
  file.write("            element, assignment = updates[-1].split(' ', 1)\n");
  file.write("            property_name, runtime_value = assignment.split('=', 1)\n");
  file.write(
      "            print('runtime command failed: plugin rejected ' + element + '.' + property_name + '=' + "
      "runtime_value, file=sys.stderr, flush=True)\n");
  file.write("        for update in updates:\n");
  file.write("            element, assignment = update.split(' ', 1)\n");
  file.write("            property_name, runtime_value = assignment.split('=', 1)\n");
  file.write("            print('stdin:@set-property ' + update, flush=True)\n");
  file.write("            if not reject and not stall:\n");
  file.write("                if runtime_control_delay_seconds > 0: time.sleep(runtime_control_delay_seconds)\n");
  file.write(
      "                print('runtime property ' + element + ' ' + property_name + '=' + runtime_value, flush=True)\n");
  file.write("preview_activation_count = 0\n");
  file.write("preview_disable_stalled = False\n");
  file.write("stall_next_progress_reset = False\n");
  file.write("delayed_progress_generation = ''\n");
  file.write("drop_progress_resets = False\n");
  file.write("stall_next_seek = False\n");
  file.write("delayed_seek_position = ''\n");
  file.write("delayed_seek_generation = ''\n");
  file.write("timeout_next_seek = False\n");
  file.write("reject_next_seek = False\n");
  file.write("backend_seek_position = 42000000000\n");
  file.write("reject_next_preview_overlays = False\n");
  file.write("reject_next_runtime_control = False\n");
  file.write("delay_next_preview_overlays = False\n");
  file.write("delayed_preview_overlay_responses = []\n");
  file.write("runtime_control_delay_seconds = 0.0\n");
  file.write("deadline = time.monotonic() + 15.0\n");
  file.write("stdin_fd = sys.stdin.fileno()\n");
  file.write("pending_stdin = b''\n");
  file.write("while time.monotonic() < deadline:\n");
  file.write("    readable, _, _ = select.select([stdin_fd], [], [], 0.05)\n");
  file.write("    if not readable:\n");
  file.write("        continue\n");
  file.write("    chunk = os.read(stdin_fd, 4096)\n");
  file.write("    if not chunk:\n");
  file.write("        break\n");
  file.write("    pending_stdin += chunk\n");
  file.write("    while b'\\n' in pending_stdin:\n");
  file.write("        raw_line, pending_stdin = pending_stdin.split(b'\\n', 1)\n");
  file.write("        handle_stdin_line(raw_line.decode(errors='replace') + '\\n')\n");
  file.close();
  return QFile::setPermissions(
      path,
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
          QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool write_fake_ffmpeg(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  file.write("#!/usr/bin/env python3\n");
  file.write("import os\n");
  file.write("import shutil\n");
  file.write("import sys\n");
  file.write("import time\n");
  file.write("args = sys.argv[1:]\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_FFMPEG_ARGS'):\n");
  file.write("    with open(os.environ['HSTREAM_UI_TEST_FFMPEG_ARGS'], 'w', encoding='utf-8') as output:\n");
  file.write("        output.write('\\n'.join(args))\n");
  file.write("source = args[args.index('-i') + 1]\n");
  file.write("target = args[-1]\n");
  file.write("print('out_time=00:05:00.000000', flush=True)\n");
  file.write("print('progress=continue', flush=True)\n");
  file.write("fail_route = os.environ.get('HSTREAM_UI_TEST_FFMPEG_FAIL_ROUTE', '')\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_FFMPEG_FAIL') == '1' or (fail_route and fail_route in target):\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY') == '1':\n");
  file.write("        os.chmod(os.path.dirname(os.environ['HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH']), 0o500)\n");
  file.write("    print('intentional remux failure', file=sys.stderr, flush=True)\n");
  file.write("    sys.exit(17)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_FFMPEG_SOURCE_REPLACEMENT') == '1':\n");
  file.write("    original = os.environ['HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH']\n");
  file.write("    os.unlink(original)\n");
  file.write("    with open(original, 'wb') as replacement:\n");
  file.write("        replacement.write(b'injected foreign source before ffmpeg input')\n");
  file.write("time.sleep(0.15)\n");
  file.write("shutil.copyfile(source, target)\n");
  file.write("print('out_time=00:10:00.000000', flush=True)\n");
  file.write("print('progress=end', flush=True)\n");
  file.close();
  return QFile::setPermissions(
      path,
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
          QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool write_fake_sync(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  file.write("#!/usr/bin/env python3\n");
  file.write("import os\n");
  file.write("import sys\n");
  file.write("import time\n");
  file.write("time.sleep(float(os.environ.get('HSTREAM_UI_TEST_SYNC_DELAY', '0')))\n");
  file.write(
      "if len(sys.argv) < 3 or sys.argv[1] != '-f' or not all(os.path.exists(path) for path in sys.argv[2:]):\n");
  file.write("    sys.exit(19)\n");
  file.close();
  return QFile::setPermissions(
      path,
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
          QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool test_game_setup(HStreamWindow* window, const QString& source_dir) {
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* video_path = require_child<QLineEdit>(window, "videoPathEdit");
  auto* add_video = require_child<QPushButton>(window, "addVideoButton");
  auto* remove_video = require_child<QPushButton>(window, "removeVideoButton");
  auto* automatic = require_child<QRadioButton>(window, "videoRole_auto");
  auto* left = require_child<QRadioButton>(window, "videoRole_left");
  auto* center = require_child<QRadioButton>(window, "videoRole_center");
  auto* right = require_child<QRadioButton>(window, "videoRole_right");
  auto* list = require_child<QListWidget>(window, "videoSetList");
  if (!game_id || !create || !video_path || !add_video || !remove_video || !automatic || !left || !center || !right ||
      !list) {
    return false;
  }

  game_id->setText("..");
  activate(create);
  if (!expect(window->gameIdText().isEmpty(), "Unsafe game IDs should be rejected")) {
    return false;
  }

  const QString auto_video = source_dir + "/GX010001.MP4";
  const QString suffix_auto_video = source_dir + "/game-left-1.mp4";
  const QString center_video = source_dir + "/GX010003.MP4";
  const QString left_video = source_dir + "/GX010005.MP4";
  const QString left_video_2 = source_dir + "/GX020005.MP4";
  const QString left_video_3 = source_dir + "/GX030005.MP4";
  const QString left_video_restart = source_dir + "/GX010006.MP4";
  const QString right_video = source_dir + "/GX010002.MP4";
  const QString right_video_2 = source_dir + "/GX020002.MP4";
  if (!write_fake_video(auto_video) || !write_fake_video(suffix_auto_video) || !write_fake_video(center_video) ||
      !write_fake_video(left_video) || !write_fake_video(left_video_2) || !write_fake_video(right_video) ||
      !write_fake_video(right_video_2) || !write_fake_video(left_video_3) || !write_fake_video(left_video_restart)) {
    return false;
  }

  game_id->setText("ui-test-game");
  activate(create);
  if (!expect(window->gameIdText() == "ui-test-game", "Game ID field should define the selected game")) {
    return false;
  }

  const fs::path cam_dir = fs::path(window->gameDirectoryText().toStdString()) / "cam1";
  fs::create_directories(cam_dir);
  if (!write_fake_video(QString::fromStdString((cam_dir / "GX010004.MP4").string()))) {
    return false;
  }
  if (!write_fake_video(window->gameDirectoryText() + "/GX019999.MP4")) {
    return false;
  }
  if (!write_fake_video(window->gameDirectoryText() + "/stitched_output-with-audio.mp4")) {
    return false;
  }
  activate(create);
  if (!expect(list_contains(list, "Auto  cam1/GX010004.MP4"), "Auto listing should include camN video sets")) {
    return false;
  }
  if (!expect(!list_contains(list, "GX019999.MP4"), "Root Auto files should be hidden when camN sets exist")) {
    return false;
  }
  if (!expect(!list_contains(list, "stitched_output-with-audio.mp4"), "Pre-stitched files are not UI video sets")) {
    return false;
  }

  activate(automatic);
  const QString undiscoverable_auto = source_dir + "/clip.mov";
  const QString undiscoverable_part = source_dir + "/left-10.mp4";
  if (!write_fake_video(undiscoverable_auto) || !write_fake_video(undiscoverable_part)) {
    return false;
  }
  const int before_undiscoverable = list->count();
  video_path->setText(undiscoverable_auto);
  activate(add_video);
  video_path->setText(undiscoverable_part);
  activate(add_video);
  if (!expect(
          list->count() == before_undiscoverable,
          "Auto should reject filenames that pipeline discovery cannot consume")) {
    return false;
  }

  video_path->setText(suffix_auto_video);
  activate(add_video);
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam2" / "game-left-1.mp4"),
          "Auto should accept the same left/right suffix patterns as pipeline discovery")) {
    return false;
  }

  video_path->setText(auto_video);
  activate(add_video);
  if (!expect(window->videoSetCount() >= 1, "Adding an auto video should populate the video set list")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam3" / "GX010001.MP4"),
          "Auto video should be imported into the game directory for existing discovery")) {
    return false;
  }

  fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  YAML::Node stale_auto = fs::exists(config) ? YAML::LoadFile(config.string()) : YAML::Node();
  stale_auto["game"]["videos"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  stale_auto["game"]["videos"]["left"].push_back("stale-generated-left.mp4");
  stale_auto["game"]["videos"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  stale_auto["game"]["videos"]["right"].push_back("stale-generated-right.mp4");
  stale_auto["game"]["stitching"]["frame_offsets"]["left"] = "11";
  stale_auto["stitching"]["frame_offsets"]["right"] = "22";
  {
    std::ofstream out(config);
    out << stale_auto << "\n";
  }
  activate(create);
  if (!expect(
          !list_contains(list, "stale-generated-left.mp4") && !list_contains(list, "stale-generated-right.mp4"),
          "Stale root generated video config should be hidden when camN sets exist")) {
    return false;
  }
  const QString auto_video_2 = source_dir + "/GX020001.MP4";
  if (!write_fake_video(auto_video_2)) {
    return false;
  }
  video_path->setText(auto_video_2);
  activate(add_video);
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam3" / "GX020001.MP4"),
          "Auto chapters from the same source folder should stay in the same camN set")) {
    return false;
  }
  YAML::Node auto_cleared = YAML::LoadFile(config.string());
  if (!expect(
          !auto_cleared["game"]["videos"]["left"] && !auto_cleared["game"]["videos"]["right"] &&
              !auto_cleared["game"]["stitching"]["frame_offsets"] && !auto_cleared["stitching"]["frame_offsets"],
          "Adding Auto videos should clear stale generated video config and offsets")) {
    return false;
  }

  activate(center);
  video_path->setText(center_video);
  activate(add_video);
  if (!expect(
          select_list_item(list, "Center  .hstream-ui/center/GX010003.MP4"),
          "Explicit Center assignment should remain visible")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui" / "center" / "GX010003.MP4") &&
              !fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "GX010003.MP4"),
          "Center imports should stay outside runtime Auto discovery paths")) {
    return false;
  }

  YAML::Node stale_offsets = YAML::LoadFile(config.string());
  stale_offsets["game"]["stitching"]["frame_offsets"]["left"] = "12";
  stale_offsets["game"]["stitching"]["frame_offsets"]["right"] = "34";
  stale_offsets["stitching"]["frame_offsets"]["left"] = "56";
  stale_offsets["stitching"]["frame_offsets"]["right"] = "78";
  stale_offsets["game"]["videos"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  stale_offsets["game"]["videos"]["left"].push_back("stale-generated-left.mp4");
  stale_offsets["game"]["videos"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  stale_offsets["game"]["videos"]["right"].push_back("stale-generated-right.mp4");
  stale_offsets["hstream_ui"]["stitching_calibration"]["control_points"] = 750;
  stale_offsets["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  {
    std::ofstream out(config);
    out << stale_offsets << "\n";
  }

  activate(right);
  video_path->setText(right_video);
  activate(add_video);
  YAML::Node one_sided = YAML::LoadFile(config.string());
  if (!expect(
          !one_sided["game"]["videos"]["left"] && !one_sided["game"]["videos"]["right"] &&
              !one_sided["game"]["stitching"]["frame_offsets"] && !one_sided["stitching"]["frame_offsets"],
          "A single explicit Left/Right side should not write a partial runtime video config") ||
      !expect(
          one_sided["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "pending",
          "Changing a video input should invalidate completed stitching calibration")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui" / "right" / "GX010002.MP4") &&
              !fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "GX010002.MP4"),
          "Explicit Right imports should stay outside runtime Auto discovery paths")) {
    return false;
  }
  activate(left);
  video_path->setText(left_video);
  activate(add_video);
  std::ifstream input(config);
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  YAML::Node yaml = YAML::LoadFile(config.string());
  if (!expect(
          yaml["hstream_ui"]["video_roles"]["center"] &&
              yaml["hstream_ui"]["video_roles"]["center"][0].as<std::string>() == ".hstream-ui/center/GX010003.MP4" &&
              !yaml["game"]["videos"]["center"] && text.find("left") != std::string::npos &&
              text.find("GX010005.MP4") != std::string::npos && text.find("right") != std::string::npos &&
              text.find("GX010002.MP4") != std::string::npos && yaml["game"]["videos"]["left"].size() == 1 &&
              yaml["game"]["videos"]["left"][0].as<std::string>() == ".hstream-ui/left/GX010005.MP4" &&
              yaml["game"]["videos"]["right"].size() == 1 &&
              yaml["game"]["videos"]["right"][0].as<std::string>() == ".hstream-ui/right/GX010002.MP4" &&
              !yaml["game"]["stitching"]["frame_offsets"] && !yaml["stitching"]["frame_offsets"],
          "Explicit roles should replace stale pipeline config, keep all chapters, and clear stale offsets")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui" / "left" / "GX010005.MP4") &&
              !fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "GX010005.MP4"),
          "Explicit Left imports should stay outside runtime Auto discovery paths")) {
    return false;
  }

  video_path->setText(left_video_2);
  activate(add_video);
  YAML::Node uneven_explicit = YAML::LoadFile(config.string());
  if (!expect(
          uneven_explicit["game"]["videos"]["left"].size() == 2 &&
              uneven_explicit["game"]["videos"]["right"].size() == 1 &&
              uneven_explicit["game"]["videos"]["left"][1].as<std::string>() == ".hstream-ui/left/GX020005.MP4",
          "Explicit Left/Right playlists with different physical chapter counts should be persisted independently")) {
    return false;
  }
  activate(right);
  video_path->setText(right_video_2);
  activate(add_video);
  YAML::Node matched_explicit = YAML::LoadFile(config.string());
  if (!expect(
          matched_explicit["game"]["videos"]["left"].size() == 2 &&
              matched_explicit["game"]["videos"]["right"].size() == 2 &&
              matched_explicit["game"]["videos"]["left"][1].as<std::string>() == ".hstream-ui/left/GX020005.MP4" &&
              matched_explicit["game"]["videos"]["right"][1].as<std::string>() == ".hstream-ui/right/GX020002.MP4",
          "Matching explicit Left/Right chapter counts should write runtime video config")) {
    return false;
  }
  activate(left);
  video_path->setText(left_video_3);
  activate(add_video);
  YAML::Node mismatched_chapters = YAML::LoadFile(config.string());
  if (!expect(
          mismatched_chapters["game"]["videos"]["left"].size() == 3 &&
              mismatched_chapters["game"]["videos"]["right"].size() == 2 &&
              mismatched_chapters["game"]["videos"]["left"][2].as<std::string>() == ".hstream-ui/left/GX030005.MP4",
          "Explicit Left/Right playlists with different chapter labels should remain independently ordered")) {
    return false;
  }
  video_path->setText(left_video_restart);
  activate(add_video);
  YAML::Node restarted_explicit = YAML::LoadFile(config.string());
  if (!expect(
          restarted_explicit["game"]["videos"]["left"].size() == 4 &&
              restarted_explicit["game"]["videos"]["right"].size() == 2 &&
              restarted_explicit["game"]["videos"]["left"][2].as<std::string>() == ".hstream-ui/left/GX030005.MP4" &&
              restarted_explicit["game"]["videos"]["left"][3].as<std::string>() == ".hstream-ui/left/GX010006.MP4",
          "Explicit GoPro playlists should sort by recording ID before physical chapter number")) {
    return false;
  }

  if (!select_list_item(list, "Right  .hstream-ui/right/GX010002.MP4")) {
    return false;
  }
  {
    YAML::Node before_failed_remove = YAML::LoadFile(config.string());
    before_failed_remove["hstream_ui"]["copied_imports"].push_back(".hstream-ui/right/GX010002.MP4");
    YAML::Node source_metadata(YAML::NodeType::Map);
    source_metadata["path"] = ".hstream-ui/right/GX010002.MP4";
    source_metadata["family"] = "test-family";
    source_metadata["source_parent"] = source_dir.toStdString();
    before_failed_remove["hstream_ui"]["auto_import_sources"].push_back(source_metadata);
    std::ofstream out(config);
    out << before_failed_remove << "\n";
  }
  std::atomic<bool> concurrent_remove_write_ok{false};
  std::thread concurrent_remove_writer([config, &concurrent_remove_write_ok] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
    if (!lock.ok())
      return;
    YAML::Node latest = YAML::LoadFile(config.string());
    latest["concurrent"]["keep"] = true;
    concurrent_remove_write_ok =
        hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(latest) + "\n").ok();
  });
  ::setenv("HM_TEST_VIDEO_REMOVE_PRE_TRANSACTION_DELAY_MS", "100", 1);
  ::setenv("HM_TEST_VIDEO_REMOVE_FAIL", ".hstream-ui/right/GX010002.MP4", 1);
  activate(remove_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_FAIL");
  ::unsetenv("HM_TEST_VIDEO_REMOVE_PRE_TRANSACTION_DELAY_MS");
  concurrent_remove_writer.join();
  YAML::Node after_failed_right_remove = YAML::LoadFile(config.string());
  if (!expect(
          concurrent_remove_write_ok &&
              fs::exists(
                  fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui" / "right" / "GX010002.MP4") &&
              after_failed_right_remove["hstream_ui"]["video_roles"]["right"] &&
              after_failed_right_remove["hstream_ui"]["copied_imports"].size() == 1 &&
              after_failed_right_remove["hstream_ui"]["auto_import_sources"].size() == 1 &&
              after_failed_right_remove["concurrent"]["keep"].as<bool>(),
          "Failed deletion must restore the transactional pre-removal state without losing an interleaved writer")) {
    return false;
  }
  if (!select_list_item(list, "Right  .hstream-ui/right/GX010002.MP4")) {
    return false;
  }
  std::atomic<bool> post_remove_writer_ok{false};
  std::thread post_remove_writer([config, &post_remove_writer_ok] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
    if (!lock.ok())
      return;
    YAML::Node latest = YAML::LoadFile(config.string());
    latest["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/concurrent.mov");
    latest["concurrent"]["post_remove_keep"] = true;
    post_remove_writer_ok = hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(latest) + "\n").ok();
  });
  ::setenv("HM_TEST_VIDEO_REMOVE_POST_TRANSACTION_DELAY_MS", "100", 1);
  ::setenv("HM_TEST_VIDEO_REMOVE_FAIL", ".hstream-ui/right/GX010002.MP4", 1);
  activate(remove_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_FAIL");
  ::unsetenv("HM_TEST_VIDEO_REMOVE_POST_TRANSACTION_DELAY_MS");
  post_remove_writer.join();
  const YAML::Node after_post_transaction_failure = YAML::LoadFile(config.string());
  const bool post_remove_state_ok = post_remove_writer_ok &&
      after_post_transaction_failure["hstream_ui"]["video_roles"]["right"].size() == 3 &&
      after_post_transaction_failure["hstream_ui"]["video_roles"]["right"][0].as<std::string>() ==
          ".hstream-ui/right/GX010002.MP4" &&
      after_post_transaction_failure["hstream_ui"]["video_roles"]["right"][1].as<std::string>() ==
          ".hstream-ui/right/GX020002.MP4" &&
      after_post_transaction_failure["hstream_ui"]["video_roles"]["right"][2].as<std::string>() ==
          ".hstream-ui/right/concurrent.mov" &&
      after_post_transaction_failure["concurrent"]["post_remove_keep"].as<bool>();
  if (!post_remove_state_ok)
    std::cerr << "post-remove config:\n" << YAML::Dump(after_post_transaction_failure) << '\n';
  if (!expect(
          post_remove_state_ok,
          "Failed deletion rollback must preserve the original role before a serialized same-role append")) {
    return false;
  }
  if (!select_list_item(list, "Right  .hstream-ui/right/GX010002.MP4")) {
    return false;
  }
  std::atomic<bool> successful_remove_writer_checked{false};
  std::atomic<bool> successful_remove_writer_saw_missing{false};
  std::thread successful_remove_writer(
      [config, &successful_remove_writer_checked, &successful_remove_writer_saw_missing] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
        if (!lock.ok())
          return;
        successful_remove_writer_saw_missing = !fs::exists(config.parent_path() / ".hstream-ui/right/GX010002.MP4");
        if (!successful_remove_writer_saw_missing) {
          YAML::Node latest = YAML::LoadFile(config.string());
          latest["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX010002.MP4");
          const auto unexpected_publish =
              hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(latest) + "\n");
          (void)unexpected_publish;
        }
        successful_remove_writer_checked = true;
      });
  ::setenv("HM_TEST_VIDEO_REMOVE_POST_TRANSACTION_DELAY_MS", "100", 1);
  activate(remove_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_POST_TRANSACTION_DELAY_MS");
  successful_remove_writer.join();
  std::ifstream updated_input(config);
  const std::string updated_text((std::istreambuf_iterator<char>(updated_input)), std::istreambuf_iterator<char>());
  YAML::Node after_right_remove = YAML::LoadFile(config.string());
  if (!expect(
          successful_remove_writer_checked && successful_remove_writer_saw_missing &&
              updated_text.find("GX010002.MP4") == std::string::npos &&
              after_right_remove["game"]["videos"]["left"].size() == 4 &&
              after_right_remove["game"]["videos"]["right"].size() == 2 &&
              after_right_remove["game"]["videos"]["right"][0].as<std::string>() == ".hstream-ui/right/GX020002.MP4" &&
              after_right_remove["game"]["videos"]["right"][1].as<std::string>() == ".hstream-ui/right/concurrent.mov",
          "A successful deletion must complete before a same-path adopter can acquire the config transaction") ||
      !expect(!list_contains(list, "GX010002.MP4"), "Removed explicit imports should not reappear as Auto")) {
    return false;
  }

  YAML::Node generated = YAML::LoadFile(config.string());
  const QString auto_import = window->gameDirectoryText() + "/cam3/GX010001.MP4";
  generated["game"]["videos"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  generated["game"]["videos"]["left"].push_back(auto_import.toStdString());
  generated["game"]["videos"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  generated["game"]["videos"]["right"].push_back("stale-generated-right.mp4");
  generated["hstream_ui"]["video_roles"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hstream_ui"]["video_roles"]["left"].push_back(".hstream-ui/left/GX010005.MP4");
  generated["hstream_ui"]["video_roles"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX020002.MP4");
  generated["hstream_ui"]["video_roles"]["center"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hstream_ui"]["video_roles"]["center"].push_back(".hstream-ui/center/GX010003.MP4");
  generated["game"]["stitching"]["frame_offsets"]["left"] = "90";
  generated["stitching"]["frame_offsets"]["left"] = "91";
  {
    std::ofstream out(config);
    out << generated << "\n";
  }
  activate(create);
  if (!expect(
          list_match_count(list, "GX010001.MP4") == 1,
          "Absolute generated config paths should not duplicate scanned Auto files")) {
    return false;
  }
  if (!select_list_item(list, "Auto  cam3/GX010001.MP4")) {
    return false;
  }
  ::setenv("HM_TEST_VIDEO_REMOVE_FAIL", "cam3/GX010001.MP4", 1);
  activate(remove_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_FAIL");
  YAML::Node after_failed_auto_remove = YAML::LoadFile(config.string());
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam3" / "GX010001.MP4") &&
              after_failed_auto_remove["game"]["videos"]["left"] &&
              after_failed_auto_remove["game"]["videos"]["right"] &&
              after_failed_auto_remove["hstream_ui"]["video_roles"]["left"] &&
              after_failed_auto_remove["hstream_ui"]["video_roles"]["center"] &&
              after_failed_auto_remove["hstream_ui"]["video_roles"]["right"] &&
              after_failed_auto_remove["game"]["stitching"]["frame_offsets"] &&
              after_failed_auto_remove["stitching"]["frame_offsets"],
          "Failed Auto deletion must restore every cleared selection and frame-offset path")) {
    return false;
  }
  if (!select_list_item(list, "Auto  cam3/GX010001.MP4")) {
    return false;
  }
  activate(remove_video);
  YAML::Node removed_auto = YAML::LoadFile(config.string());
  if (!expect(
          !removed_auto["game"]["videos"]["left"] && !removed_auto["game"]["videos"]["right"] &&
              !removed_auto["hstream_ui"]["video_roles"]["left"] &&
              !removed_auto["hstream_ui"]["video_roles"]["center"] &&
              !removed_auto["hstream_ui"]["video_roles"]["right"] &&
              !removed_auto["game"]["stitching"]["frame_offsets"] && !removed_auto["stitching"]["frame_offsets"],
          "Removing Auto config entries should clear stale generated runtime and explicit role config")) {
    return false;
  }
  if (!expect(!list_contains(list, "GX010001.MP4"), "Removed Auto imports should not reappear from config")) {
    return false;
  }

  const QString arbitrary_left = source_dir + "/left-camera.mov";
  const QString arbitrary_right = source_dir + "/right-camera.mov";
  const QString arbitrary_left_2 = source_dir + "/left-camera-alt.mov";
  const QString arbitrary_right_2 = source_dir + "/right-camera-alt.mov";
  if (!write_fake_video(arbitrary_left) || !write_fake_video(arbitrary_right) || !write_fake_video(arbitrary_left_2) ||
      !write_fake_video(arbitrary_right_2)) {
    return false;
  }
  game_id->setText("ui-explicit-single-file-game");
  activate(create);
  activate(left);
  video_path->setText(arbitrary_left);
  activate(add_video);
  activate(right);
  video_path->setText(arbitrary_right);
  activate(add_video);
  YAML::Node arbitrary_config =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  if (!expect(
          arbitrary_config["game"]["videos"]["left"].size() == 1 &&
              arbitrary_config["game"]["videos"]["right"].size() == 1 &&
              arbitrary_config["game"]["videos"]["left"][0].as<std::string>() == ".hstream-ui/left/left-camera.mov" &&
              arbitrary_config["game"]["videos"]["right"][0].as<std::string>() == ".hstream-ui/right/right-camera.mov",
          "Single-file explicit Left/Right pairs with arbitrary filenames should run as chapter 1")) {
    return false;
  }
  activate(left);
  video_path->setText(arbitrary_left_2);
  activate(add_video);
  YAML::Node arbitrary_mismatched =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  if (!expect(
          arbitrary_mismatched["game"]["videos"]["left"].size() == 2 &&
              arbitrary_mismatched["game"]["videos"]["right"].size() == 1 &&
              arbitrary_mismatched["game"]["videos"]["left"][1].as<std::string>() ==
                  ".hstream-ui/left/left-camera-alt.mov",
          "Arbitrary explicit playlists with different counts should remain in independent insertion order")) {
    return false;
  }
  activate(right);
  video_path->setText(arbitrary_right_2);
  activate(add_video);
  YAML::Node arbitrary_multi =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  if (!expect(
          arbitrary_multi["game"]["videos"]["left"].size() == 2 &&
              arbitrary_multi["game"]["videos"]["right"].size() == 2 &&
              arbitrary_multi["game"]["videos"]["left"][1].as<std::string>() ==
                  ".hstream-ui/left/left-camera-alt.mov" &&
              arbitrary_multi["game"]["videos"]["right"][1].as<std::string>() ==
                  ".hstream-ui/right/right-camera-alt.mov",
          "Equal-length arbitrary explicit lists should run in insertion order")) {
    return false;
  }

  const QString part_left_12 = source_dir + "/left-12.mkv";
  const QString part_left_2 = source_dir + "/left-2.mkv";
  const QString part_right = source_dir + "/right.mkv";
  if (!write_fake_video(part_left_12) || !write_fake_video(part_left_2) || !write_fake_video(part_right)) {
    return false;
  }
  game_id->setText("ui-explicit-numbered-parts-game");
  activate(create);
  activate(left);
  video_path->setText(part_left_12);
  activate(add_video);
  video_path->setText(part_left_2);
  activate(add_video);
  activate(right);
  video_path->setText(part_right);
  activate(add_video);
  const YAML::Node numbered_parts =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  if (!expect(
          numbered_parts["game"]["videos"]["left"].size() == 2 &&
              numbered_parts["game"]["videos"]["right"].size() == 1 &&
              numbered_parts["game"]["videos"]["left"][0].as<std::string>() == ".hstream-ui/left/left-2.mkv" &&
              numbered_parts["game"]["videos"]["left"][1].as<std::string>() == ".hstream-ui/left/left-12.mkv",
          "Explicit left/right part playlists should support MKV and sort multi-digit parts numerically")) {
    return false;
  }

  const QString heterogeneous_insta = source_dir + "/VID_20260815_101000_001.MP4";
  const QString heterogeneous_gopro = source_dir + "/GX010007.MP4";
  const QString heterogeneous_arbitrary = source_dir + "/left-camera.mov";
  const QString heterogeneous_right = source_dir + "/right-1.m4v";
  if (!write_fake_video(heterogeneous_insta) || !write_fake_video(heterogeneous_gopro) ||
      !write_fake_video(heterogeneous_arbitrary) || !write_fake_video(heterogeneous_right)) {
    return false;
  }
  game_id->setText("ui-explicit-heterogeneous-camera-game");
  activate(create);
  activate(left);
  video_path->setText(heterogeneous_insta);
  activate(add_video);
  video_path->setText(heterogeneous_gopro);
  activate(add_video);
  video_path->setText(heterogeneous_arbitrary);
  activate(add_video);
  activate(right);
  video_path->setText(heterogeneous_right);
  activate(add_video);
  const YAML::Node heterogeneous_camera =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  if (!expect(
          heterogeneous_camera["game"]["videos"]["left"].size() == 3 &&
              heterogeneous_camera["game"]["videos"]["right"].size() == 1 &&
              heterogeneous_camera["game"]["videos"]["left"][0].as<std::string>() ==
                  ".hstream-ui/left/VID_20260815_101000_001.MP4" &&
              heterogeneous_camera["game"]["videos"]["left"][1].as<std::string>() == ".hstream-ui/left/GX010007.MP4" &&
              heterogeneous_camera["game"]["videos"]["left"][2].as<std::string>() == ".hstream-ui/left/left-camera.mov",
          "A heterogeneous explicit camera playlist should preserve the user's total recording order")) {
    return false;
  }

  const fs::path duplicate_source_a = fs::path(source_dir.toStdString()) / "source-a";
  const fs::path duplicate_source_b = fs::path(source_dir.toStdString()) / "source-b";
  fs::create_directories(duplicate_source_a);
  fs::create_directories(duplicate_source_b);
  const QString duplicate_a = QString::fromStdString((duplicate_source_a / "GX020001.MP4").string());
  const QString duplicate_a_2 = QString::fromStdString((duplicate_source_a / "GX030001.MP4").string());
  const QString duplicate_a_other_camera = QString::fromStdString((duplicate_source_a / "GX020002.MP4").string());
  const QString duplicate_b = QString::fromStdString((duplicate_source_b / "GX020001.MP4").string());
  if (!write_fake_video(duplicate_a) || !write_fake_video(duplicate_a_2) ||
      !write_fake_video(duplicate_a_other_camera) || !write_fake_video(duplicate_b)) {
    return false;
  }
  game_id->setText("ui-copied-auto-game");
  activate(create);
  fs::path copied_game = fs::path(window->gameDirectoryText().toStdString());
  fs::create_directories(copied_game / "cam1");
  if (!write_fake_video(QString::fromStdString((copied_game / "cam1" / "GX020001.MP4").string()))) {
    return false;
  }
  YAML::Node copied_metadata;
  YAML::Node copied_entry(YAML::NodeType::Map);
  copied_entry["path"] = "cam1/GX020001.MP4";
  copied_entry["family"] = "gopro-0001";
  copied_entry["source_parent"] = duplicate_source_a.string();
  copied_metadata["hstream_ui"]["auto_import_sources"].push_back(copied_entry);
  {
    std::ofstream out(copied_game / "config.yaml");
    out << copied_metadata << "\n";
  }
  activate(automatic);
  video_path->setText(duplicate_a_2);
  activate(add_video);
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam1" / "GX030001.MP4"),
          "Copied Auto import metadata should allow later chapters to reuse the same camN directory")) {
    return false;
  }

  game_id->setText("ui-symlink-auto-game");
  activate(create);
  fs::create_directory_symlink(duplicate_source_a, fs::path(window->gameDirectoryText().toStdString()) / "cam1");
  activate(automatic);
  video_path->setText(duplicate_a);
  activate(add_video);
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam2" / "GX020001.MP4"),
          "Symlinked camN directories should not block Auto import reuse")) {
    return false;
  }

  game_id->setText("ui-auto-remove-cleanup-game");
  activate(create);
  fs::path cleanup_game = fs::path(window->gameDirectoryText().toStdString());
  fs::create_directories(cleanup_game / "cam1");
  fs::create_directories(cleanup_game / ".hstream-ui" / "left");
  if (!write_fake_video(QString::fromStdString((cleanup_game / "cam1" / "GX010001.MP4").string()))) {
    return false;
  }
  if (!write_fake_video(QString::fromStdString((cleanup_game / ".hstream-ui" / "left" / "copied-left.mp4").string()))) {
    return false;
  }
  YAML::Node cleanup_config;
  cleanup_config["hstream_ui"]["video_roles"]["left"].push_back(".hstream-ui/left/copied-left.mp4");
  cleanup_config["hstream_ui"]["copied_imports"].push_back(".hstream-ui/left/copied-left.mp4");
  {
    std::ofstream out(cleanup_game / "config.yaml");
    out << cleanup_config << "\n";
  }
  activate(create);
  if (!select_list_item(list, "Auto  cam1/GX010001.MP4")) {
    return false;
  }
  activate(remove_video);
  YAML::Node cleanup_after = YAML::LoadFile((cleanup_game / "config.yaml").string());
  if (!expect(
          fs::exists(cleanup_game / "cam1" / "GX010001.MP4") &&
              fs::exists(cleanup_game / ".hstream-ui" / "left" / "copied-left.mp4") &&
              cleanup_after["hstream_ui"]["video_roles"]["left"] &&
              cleanup_after["hstream_ui"]["copied_imports"].size() == 1,
          "Refused Auto deletion must preserve the complete prior file and config state")) {
    return false;
  }

  game_id->setText("ui-partial-auto-cleanup-game");
  activate(create);
  const fs::path partial_cleanup_game = fs::path(window->gameDirectoryText().toStdString());
  fs::create_directories(partial_cleanup_game / ".hstream-ui" / "left");
  fs::create_directories(partial_cleanup_game / ".hstream-ui" / "right");
  const std::string removed_old_path = ".hstream-ui/left/aa-old-copy.mp4";
  const std::string retained_old_path = ".hstream-ui/right/zz-old-copy.mp4";
  if (!write_fake_video(QString::fromStdString((partial_cleanup_game / removed_old_path).string())) ||
      !write_fake_video(QString::fromStdString((partial_cleanup_game / retained_old_path).string()))) {
    return false;
  }
  YAML::Node partial_cleanup_config;
  partial_cleanup_config["hstream_ui"]["video_roles"]["left"].push_back(removed_old_path);
  partial_cleanup_config["hstream_ui"]["video_roles"]["right"].push_back(retained_old_path);
  partial_cleanup_config["hstream_ui"]["copied_imports"].push_back(removed_old_path);
  partial_cleanup_config["hstream_ui"]["copied_imports"].push_back(retained_old_path);
  {
    std::ofstream out(partial_cleanup_game / "config.yaml");
    out << partial_cleanup_config << "\n";
  }
  activate(create);
  activate(automatic);
  ::setenv("HM_TEST_VIDEO_REMOVE_FAIL", retained_old_path.c_str(), 1);
  video_path->setText(duplicate_a);
  activate(add_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_FAIL");
  const YAML::Node partial_cleanup_after = YAML::LoadFile((partial_cleanup_game / "config.yaml").string());
  if (!expect(
          !fs::exists(partial_cleanup_game / removed_old_path) &&
              fs::exists(partial_cleanup_game / retained_old_path) &&
              fs::exists(partial_cleanup_game / "cam1" / "GX020001.MP4") &&
              !partial_cleanup_after["hstream_ui"]["video_roles"]["left"] &&
              !partial_cleanup_after["hstream_ui"]["video_roles"]["right"] &&
              partial_cleanup_after["hstream_ui"]["copied_imports"].size() == 1 &&
              partial_cleanup_after["hstream_ui"]["copied_imports"][0].as<std::string>() == retained_old_path &&
              window->logText().contains(
                  "video set added, but one or more unreferenced copied imports could not be cleaned"),
          "Partial old-copy cleanup must retain the committed new Auto import and metadata for the failed deletion")) {
    return false;
  }

  game_id->setText("ui-auto-copy-rollback-game");
  activate(create);
  const fs::path rollback_game = fs::path(window->gameDirectoryText().toStdString());
  fs::create_directories(rollback_game / ".hstream-ui" / "left");
  const std::string rollback_old_path = ".hstream-ui/left/old-copy.mp4";
  const std::string rollback_concurrent_path = ".hstream-ui/left/concurrent-copy.mp4";
  if (!write_fake_video(QString::fromStdString((rollback_game / rollback_old_path).string())) ||
      !write_fake_video(QString::fromStdString((rollback_game / rollback_concurrent_path).string()))) {
    return false;
  }
  YAML::Node rollback_config;
  rollback_config["hstream_ui"]["video_roles"]["left"].push_back(rollback_old_path);
  rollback_config["hstream_ui"]["copied_imports"].push_back(rollback_old_path);
  {
    std::ofstream out(rollback_game / "config.yaml");
    out << rollback_config << "\n";
  }
  activate(create);
  activate(automatic);
  std::atomic<bool> auto_writer_ok{false};
  std::atomic<bool> auto_writer_saw_missing{false};
  std::thread auto_writer([rollback_game, rollback_concurrent_path, &auto_writer_ok, &auto_writer_saw_missing] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(rollback_game);
    if (!lock.ok())
      return;
    auto_writer_saw_missing = !fs::exists(rollback_game / "cam1/GX020001.MP4");
    YAML::Node latest = YAML::LoadFile((rollback_game / "config.yaml").string());
    latest["hstream_ui"]["video_roles"]["left"] = YAML::Node(YAML::NodeType::Sequence);
    latest["hstream_ui"]["video_roles"]["left"].push_back(rollback_concurrent_path);
    latest["hstream_ui"]["copied_imports"].push_back(rollback_concurrent_path);
    if (!auto_writer_saw_missing)
      latest["hstream_ui"]["copied_imports"].push_back("cam1/GX020001.MP4");
    latest["concurrent"]["auto_keep"] = true;
    auto_writer_ok = hm::stitching::publish_game_config(rollback_game, YAML::Dump(latest) + "\n").ok();
  });
  ::setenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY", "1", 1);
  ::setenv("HM_TEST_VIDEO_ADD_PRE_CONFIG_SAVE_DELAY_MS", "100", 1);
  ::setenv("HM_TEST_VIDEO_REMOVE_FAIL", rollback_old_path.c_str(), 1);
  video_path->setText(duplicate_a);
  activate(add_video);
  ::unsetenv("HM_TEST_VIDEO_REMOVE_FAIL");
  ::unsetenv("HM_TEST_VIDEO_ADD_PRE_CONFIG_SAVE_DELAY_MS");
  ::unsetenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY");
  auto_writer.join();
  const YAML::Node rollback_after = YAML::LoadFile((rollback_game / "config.yaml").string());
  if (!expect(
          auto_writer_ok && auto_writer_saw_missing && !fs::exists(rollback_game / "cam1" / "GX020001.MP4") &&
              fs::exists(rollback_game / rollback_old_path) && fs::exists(rollback_game / rollback_concurrent_path) &&
              rollback_after["hstream_ui"]["video_roles"]["left"] &&
              rollback_after["hstream_ui"]["copied_imports"].size() == 2 &&
              rollback_after["hstream_ui"]["video_roles"]["left"][0].as<std::string>() == rollback_concurrent_path &&
              rollback_after["hstream_ui"]["copied_imports"][0].as<std::string>() == rollback_old_path &&
              rollback_after["hstream_ui"]["copied_imports"][1].as<std::string>() == rollback_concurrent_path &&
              (!rollback_after["hstream_ui"]["auto_import_sources"] ||
               rollback_after["hstream_ui"]["auto_import_sources"].size() == 0) &&
              rollback_after["concurrent"]["auto_keep"].as<bool>(),
          "Auto cleanup rollback must finish before another importer can inspect or adopt the same path")) {
    return false;
  }

  game_id->setText("ui-copy-rollback-delete-failure-game");
  activate(create);
  activate(left);
  ::setenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY", "1", 1);
  ::setenv("HM_TEST_PRIVATE_CONFIG_SAVE_FAIL", "1", 1);
  ::setenv("HM_TEST_VIDEO_STAGED_REMOVE_FAIL", ".hstream-ui/left/GX020001.MP4", 1);
  video_path->setText(duplicate_a);
  activate(add_video);
  ::unsetenv("HM_TEST_VIDEO_STAGED_REMOVE_FAIL");
  ::unsetenv("HM_TEST_PRIVATE_CONFIG_SAVE_FAIL");
  ::unsetenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY");
  const fs::path rollback_delete_failure_game = fs::path(window->gameDirectoryText().toStdString());
  const fs::path rollback_delete_failure_config = rollback_delete_failure_game / "config.yaml";
  YAML::Node rollback_delete_failure_after = YAML::LoadFile(rollback_delete_failure_config.string());
  bool rollback_staging_path_exists = false;
  for (const auto& entry : fs::directory_iterator(rollback_delete_failure_game / ".hstream-ui/left")) {
    if (entry.path().filename().string().rfind(".hstream-rollback-", 0) == 0)
      rollback_staging_path_exists = true;
  }
  if (!expect(
          fs::exists(rollback_delete_failure_game / ".hstream-ui/left/GX020001.MP4") && !rollback_staging_path_exists &&
              rollback_delete_failure_after["hstream_ui"]["copied_imports"].size() == 1 &&
              !rollback_delete_failure_after["hstream_ui"]["video_roles"]["left"],
          "A staged copied-file rollback deletion failure must restore the path and ownership metadata")) {
    return false;
  }
  activate(create);
  if (!select_list_item(list, "Left  .hstream-ui/left/GX020001.MP4")) {
    return false;
  }
  activate(remove_video);
  rollback_delete_failure_after = YAML::LoadFile(rollback_delete_failure_config.string());
  if (!expect(
          !fs::exists(rollback_delete_failure_game / ".hstream-ui/left/GX020001.MP4") &&
              rollback_delete_failure_after["hstream_ui"]["copied_imports"].size() == 0,
          "An owned orphan retained after rollback must remain visible and removable through the UI")) {
    return false;
  }

  game_id->setText("ui-copy-save-failure-game");
  activate(create);
  activate(left);
  ::setenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY", "1", 1);
  ::setenv("HM_TEST_PRIVATE_CONFIG_SAVE_FAIL", "1", 1);
  video_path->setText(duplicate_a);
  activate(add_video);
  ::unsetenv("HM_TEST_PRIVATE_CONFIG_SAVE_FAIL");
  ::unsetenv("HM_TEST_VIDEO_IMPORT_FORCE_COPY");
  const fs::path save_failure_game = fs::path(window->gameDirectoryText().toStdString());
  const YAML::Node save_failure_after = YAML::LoadFile((save_failure_game / "config.yaml").string());
  const YAML::Node save_failure_roles = save_failure_after["hstream_ui"]["video_roles"];
  if (!expect(
          !fs::exists(save_failure_game / ".hstream-ui" / "left" / "GX020001.MP4") &&
              save_failure_after["hstream_ui"]["copied_imports"].size() == 0 &&
              (!save_failure_roles || !save_failure_roles["left"]),
          "Private-config save failure must remove the new copied file and its ownership metadata")) {
    return false;
  }

  game_id->setText("ui-empty-auto-game");
  activate(create);
  activate(automatic);
  video_path->setText(duplicate_a);
  activate(add_video);
  video_path->setText(duplicate_a_2);
  activate(add_video);
  video_path->setText(duplicate_a_other_camera);
  activate(add_video);
  video_path->setText(duplicate_b);
  activate(add_video);
  return expect(
             fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam1" / "GX020001.MP4"),
             "First Auto import in an empty game should create cam1 with the original file name") &&
      expect(fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam1" / "GX030001.MP4"),
             "Auto imports from the same camera folder should reuse the same camN directory") &&
      expect(fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam2" / "GX020002.MP4"),
             "Auto imports for a different vendor video id should create a separate camN directory") &&
      expect(fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "cam3" / "GX020001.MP4"),
             "Auto imports from a different camera folder should create a new camN without renaming");
}

bool set_test_calibration_status(HStreamWindow* window, const std::string& status, int control_points = 1500) {
  const fs::path game_dir(window->gameDirectoryText().toStdString());
  const fs::path config_path = game_dir / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    config["hstream_ui"]["stitching_calibration"]["control_points"] = control_points;
    config["hstream_ui"]["stitching_calibration"]["status"] = status;
  } catch (const std::exception& exception) {
    std::cerr << "Could not prepare calibration dialog test: " << exception.what() << '\n';
    return false;
  }
  const auto published = hm::stitching::publish_game_config(game_dir, YAML::Dump(config) + "\n");
  if (!published.ok()) {
    std::cerr << "Could not publish calibration dialog test config: " << published << '\n';
    return false;
  }
  return true;
}

bool test_calibration_progress_dialog(HStreamWindow* window) {
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* control_points = require_child<QSpinBox>(window, "controlPointsSpin");
  if (!start || !stop || !mode || !control_points || !set_test_calibration_status(window, "pending"))
    return false;

  mode->setCurrentIndex(mode->findData("stitch-calibration"));
  control_points->setValue(1500);
  qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  activate(start);
  for (int i = 0; i < 200 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  auto* dialog = require_child<QDialog>(window, "stitchCalibrationDialog");
  auto* headline = require_child<QLabel>(window, "stitchCalibrationHeadline");
  auto* detail = require_child<QLabel>(window, "stitchCalibrationDetail");
  auto* status_icon = require_child<QLabel>(window, "stitchCalibrationIcon");
  auto* progress = require_child<QProgressBar>(window, "stitchCalibrationProgress");
  auto* input_stage = require_child<QLabel>(window, "stitchCalibrationStage_input");
  auto* orientation_stage = require_child<QLabel>(window, "stitchCalibrationStage_orientation");
  auto* features_stage = require_child<QLabel>(window, "stitchCalibrationStage_features");
  auto* matching_stage = require_child<QLabel>(window, "stitchCalibrationStage_matching");
  auto* optimizer_stage = require_child<QLabel>(window, "stitchCalibrationStage_optimizer");
  auto* rink_stage = require_child<QLabel>(window, "stitchCalibrationStage_rink-mask");
  auto* ok = require_child<QPushButton>(window, "stitchCalibrationOkButton");
  auto* cancel = require_child<QPushButton>(window, "stitchCalibrationCancelButton");
  if (!dialog || !headline || !detail || !status_icon || !progress || !input_stage || !orientation_stage ||
      !features_stage || !matching_stage || !optimizer_stage || !rink_stage || !ok || !cancel) {
    activate(stop);
    return false;
  }
  if (!expect(dialog->isVisible(), "Calibration-required Play should open the progress popup") ||
      !expect(
          dialog->windowModality() == Qt::WindowModal && dialog->parentWidget() == window &&
              !dialog->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
          "The calibration popup should be modal only to HStream, not system-wide always-on-top") ||
      !expect(
          headline->text().contains("Calibrating stitching"), "Active popup should identify stitching calibration") ||
      !expect(
          input_stage->property("calibrationState").toString() == "active",
          "The synchronized-frame stage should be active while the runner waits") ||
      !expect(
          progress->isVisible() && progress->minimum() == 0 && progress->maximum() == 0,
          "Active calibration should show indeterminate progress") ||
      !expect(cancel->isVisible() && !ok->isVisible(), "Active calibration should offer Stop instead of OK") ||
      !expect(
          cancel->toolTip().contains("Stop the active stitching calibration") &&
              ok->toolTip().contains("Close this calibration result") && cancel->statusTip() == cancel->toolTip() &&
              ok->statusTip() == ok->toolTip(),
          "Calibration dialog actions should explain their behavior on hover")) {
    activate(stop);
    return false;
  }
  dialog->reject();
  QApplication::processEvents();
  if (!expect(dialog->isVisible(), "Escape/window rejection should not dismiss active calibration")) {
    activate(stop);
    return false;
  }
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(!dialog->isVisible(), "User cancellation should close the calibration popup without a failure"))
    return false;

  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "failure");
  activate(start);
  for (int i = 0; i < 300 && !headline->text().contains("failed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(dialog->isVisible(), "A native calibration failure should leave the popup open") ||
      !expect(
          status_icon->property("calibrationState").toString() == "failed" && !status_icon->pixmap().isNull(),
          "A failed calibration should display its red critical status icon") ||
      !expect(
          detail->text().contains("autooptimiser rejected the control-point geometry"),
          "The popup should show the native failure reason") ||
      !expect(
          orientation_stage->property("calibrationState").toString() == "complete" &&
              features_stage->property("calibrationState").toString() == "complete" &&
              matching_stage->property("calibrationState").toString() == "complete" &&
              optimizer_stage->property("calibrationState").toString() == "failed" &&
              rink_stage->property("calibrationState").toString() == "skipped",
          "Native milestones should advance completed stitching stages and keep the Program rink stage omitted") ||
      !expect(
          ok->isVisible() && ok->isEnabled() && !cancel->isVisible(),
          "A failed calibration should end with an enabled OK button") ||
      !expect(
          window->pipelineStateText() == "PLAYING",
          "A reported rink/calibration error may remain inspectable while playback exits or continues")) {
    activate(stop);
    qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
    return false;
  }
  activate(ok);
  if (!expect(!dialog->isVisible(), "OK should close a failed calibration popup")) {
    activate(stop);
    qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
    return false;
  }
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "exit");
  activate(start);
  for (int i = 0; i < 300 && (window->pipelineStateText() != "STOPPED" || !headline->text().contains("failed")); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(dialog->isVisible(), "An early calibration process exit should open a persistent failure popup") ||
      !expect(
          detail->text().contains("ended before it finished") && detail->text().contains("exit 9"),
          "An early exit should show its exit diagnostics") ||
      !expect(ok->isVisible(), "An early-exit failure should be dismissible with OK")) {
    qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
    return false;
  }
  activate(ok);

  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "diagnostic-exit");
  activate(start);
  for (int i = 0; i < 300 && (window->pipelineStateText() != "STOPPED" || !headline->text().contains("failed")); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(dialog->isVisible(), "A diagnostic calibration exit should leave the failure popup open") ||
      !expect(
          detail->text().contains("unsafe or implausibly large canvas") &&
              detail->text().contains("FAILED_PRECONDITION: OpenCV transform has unsafe canvas extent") &&
              detail->text().contains("1 frame-set candidate"),
          "The failure popup must analyze a final stderr diagnostic that has no trailing newline")) {
    qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
    return false;
  }
  activate(ok);

  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS", "30");
  activate(start);
  for (int i = 0; i < 200 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const fs::path superseded_config_path = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  {
    auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(superseded_config_path.parent_path());
    if (!config_lock.ok()) {
      std::cerr << "Could not lock superseded calibration test config: " << config_lock.status() << '\n';
      return false;
    }
    YAML::Node superseded = YAML::LoadFile(superseded_config_path.string());
    YAML::Node calibration = superseded["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "pending";
    calibration["stale_from"] = "input";
    calibration["artifacts_invalidated"] = false;
    calibration.remove("invalidation_id");
    const auto published =
        hm::stitching::publish_game_config(superseded_config_path.parent_path(), YAML::Dump(superseded) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not publish superseded calibration test config: " << published << '\n';
      return false;
    }
  }
  for (int i = 0; i < 400 && !detail->text().contains("inputs changed while calibration was running"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node superseded_completion = YAML::LoadFile(superseded_config_path.string());
  const YAML::Node superseded_calibration = superseded_completion["hstream_ui"]["stitching_calibration"];
  const bool superseded_completion_ok = expect(
      detail->text().contains("inputs changed while calibration was running") &&
          superseded_calibration["status"].as<std::string>() == "pending" &&
          superseded_calibration["stale_from"].as<std::string>() == "input" &&
          !superseded_calibration["artifacts_invalidated"].as<bool>(),
      "Calibration completion must not erase a newer upstream dependency invalidation");
  activate(stop);
  activate(ok);
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS");
  if (!superseded_completion_ok)
    return false;

  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_PLAYBACK_RESTART_DELAY_MS", "750");
  activate(start);
  for (int i = 0; i < 400 && !headline->text().contains("Restarting playback"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node waiting_for_restart =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  const bool waits_for_restart =
      expect(
          dialog->isVisible() && headline->text().contains("Restarting playback") && progress->isVisible(),
          "Calibration completion must keep the progress popup open while playback restarts") &&
      expect(
          waiting_for_restart["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "pending",
          "The UI must not declare calibration playback running before the restart-complete event");
  for (int i = 0; i < 400 &&
       (dialog->isVisible() ||
        !window->logText().contains("one-pass stitching calibration complete; continuous stitched preview running"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  YAML::Node completed = YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  YAML::Node completed_status;
  const bool has_completed_status =
      lookup_yaml_path(completed, {"hstream_ui", "stitching_calibration", "status"}, &completed_status);
  YAML::Node completed_rink_status;
  const bool has_completed_rink_status =
      lookup_yaml_path(completed, {"hstream_ui", "stitching_calibration", "rink_mask_status"}, &completed_rink_status);
  const bool success_ok = expect(
                              !dialog->isVisible(),
                              "Successful stitching-only calibration should close the popup automatically") &&
      waits_for_restart &&
      expect(window->pipelineStateText() == "PLAYING", "The pipeline should continue after the popup auto-closes") &&
      expect(has_completed_status && completed_status.as<std::string>() == "complete",
             "The final stitching milestone should persist completed stitching calibration") &&
      expect(has_completed_rink_status && completed_rink_status.as<std::string>() == "omitted",
             "Stitching-only completion must record that it did not build the Program rink mask") &&
      expect(rink_stage->property("calibrationState").toString() == "skipped" &&
                 rink_stage->text().contains("Program only") &&
                 detail->text().contains("Program will validate or build its rink mask") &&
                 !detail->text().contains("ice-surface calibration are ready"),
             "Stitching-only completion should present the downstream rink-mask stage as omitted");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_PLAYBACK_RESTART_DELAY_MS");
  if (!success_ok || !set_test_calibration_status(window, "complete"))
    return false;

  mode->setCurrentIndex(mode->findData("program"));
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS", "40");
  qputenv(
      "HSTREAM_UI_TEST_PRECALIBRATION_STDERR",
      "INVALID_ARGUMENT: unrelated pre-calibration runtime status\n"
      "Skipping pooled stitching calibration: FAILED_PRECONDITION: OpenCV transform has unsafe canvas extent");
  qputenv("HSTREAM_UI_TEST_AFTER_PRECALIBRATION_STDERR_DELAY_MS", "40");
  activate(start);
  for (int i = 0; i < 200 && !dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool program_discovery_visible =
      expect(dialog->isVisible(), "Program playback should show calibration discovered by the running pipeline") &&
      expect(
          headline->text().contains("Calibrating stitching"),
          "Program playback should use the same stitching calibration progress popup") &&
      expect(
          window->logText().contains("running pipeline discovered stitching calibration"),
          "Program playback should log why it opened the calibration progress popup") &&
      expect(
          HStreamWindowTestAccess::calibrationFailureAnalysis(window, "runtime-discovery fixture")
                  .contains("FAILED_PRECONDITION: OpenCV transform has unsafe canvas extent") &&
              !HStreamWindowTestAccess::calibrationFailureAnalysis(window, "runtime-discovery fixture")
                   .contains("unrelated pre-calibration runtime status"),
          "Runtime-discovered calibration must preserve stderr diagnostics received before its first milestone");
  for (int i = 0; i < 400 &&
       (dialog->isVisible() ||
        !window->logText().contains("one-pass stitching calibration complete; continuous program playback running"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node program_completed =
      YAML::LoadFile((fs::path(window->gameDirectoryText().toStdString()) / "config.yaml").string());
  const bool program_discovery_completed =
      expect(!dialog->isVisible(), "Successful Program calibration should close the progress popup automatically") &&
      expect(
          window->pipelineStateText() == "PLAYING",
          "Program playback should continue after runtime-discovered calibration completes") &&
      expect(
          rink_stage->property("calibrationState").toString() == "complete" &&
              detail->text().contains("ice-surface calibration are ready") &&
              program_completed["hstream_ui"]["stitching_calibration"]["rink_mask_status"].as<std::string>("") ==
                  "complete",
          "Program calibration should still generate and report its required rink mask");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS");
  qunsetenv("HSTREAM_UI_TEST_PRECALIBRATION_STDERR");
  qunsetenv("HSTREAM_UI_TEST_AFTER_PRECALIBRATION_STDERR_DELAY_MS");

  if (!set_test_calibration_status(window, "complete"))
    return false;
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS", "150");
  activate(start);
  const fs::path superseded_program_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  {
    auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(superseded_program_config.parent_path());
    if (!config_lock.ok()) {
      std::cerr << "Could not lock Program discovery race config: " << config_lock.status() << '\n';
      return false;
    }
    YAML::Node superseded = YAML::LoadFile(superseded_program_config.string());
    YAML::Node calibration = superseded["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "pending";
    calibration["stale_from"] = "input";
    calibration["artifacts_invalidated"] = false;
    calibration["invalidation_id"] = "newer-program-invalidation";
    const auto published =
        hm::stitching::publish_game_config(superseded_program_config.parent_path(), YAML::Dump(superseded) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not publish Program discovery race config: " << published << '\n';
      return false;
    }
  }
  for (int i = 0; i < 300 && !window->logText().contains("runtime-discovered calibration was superseded"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node after_superseded_program = YAML::LoadFile(superseded_program_config.string());
  const YAML::Node superseded_program_calibration = after_superseded_program["hstream_ui"]["stitching_calibration"];
  const bool superseded_program_ok =
      expect(
          !dialog->isVisible(),
          "A superseded Program run must not adopt or show calibration owned by a newer invalidation") &&
      expect(
          window->logText().contains("runtime-discovered calibration was superseded"),
          "Program discovery should report that its reserved owner was superseded") &&
      expect(
          superseded_program_calibration["status"].as<std::string>() == "pending" &&
              superseded_program_calibration["invalidation_id"].as<std::string>() == "newer-program-invalidation" &&
              !superseded_program_calibration["artifacts_invalidated"].as<bool>(),
          "A runtime-discovered calibration event must not overwrite a newer invalidation owner");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS");
  if (!superseded_program_ok)
    return false;

  if (auto* log = require_child<QTextEdit>(window, "runtimeLog"))
    log->clear();
  else
    return false;
  return program_discovery_visible && program_discovery_completed && set_test_calibration_status(window, "pending");
}

bool test_pipeline_buttons(HStreamWindow* window) {
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* pause = require_child<QPushButton>(window, "pausePipelineButton");
  auto* restart = require_child<QPushButton>(window, "restartStageButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* control_points = require_child<QSpinBox>(window, "controlPointsSpin");
  auto* calibration_frame_count = require_child<QSpinBox>(window, "calibrationFrameCountSpin");
  auto* stitch_frame_time = require_child<QTimeEdit>(window, "stitchFrameTimeEdit");
  auto* control_point_matcher = require_child<QComboBox>(window, "controlPointMatcherCombo");
  auto* mapping_backend = require_child<QComboBox>(window, "mappingBackendCombo");
  auto* projection = require_child<QComboBox>(window, "stitchProjectionCombo");
  auto* panini_compression = require_child<QDoubleSpinBox>(window, "generalPaniniCompressionSpin");
  auto* panini_top_squeeze = require_child<QDoubleSpinBox>(window, "generalPaniniTopSqueezeSpin");
  auto* panini_bottom_squeeze = require_child<QDoubleSpinBox>(window, "generalPaniniBottomSqueezeSpin");
  auto* projection_auto_fov = require_child<QCheckBox>(window, "projectionAutoFovCheck");
  auto* projection_horizontal_fov = require_child<QDoubleSpinBox>(window, "projectionHorizontalFovSpin");
  auto* projection_auto_canvas = require_child<QCheckBox>(window, "projectionAutoCanvasCheck");
  auto* projection_auto_crop = require_child<QCheckBox>(window, "projectionAutoCropCheck");
  auto* stitch_max_output_width = require_child<QSpinBox>(window, "stitchMaxOutputWidthSpin");
  auto* run_autooptimizer = require_child<QCheckBox>(window, "runAutooptimizerCheck");
  auto* save_preset_button = require_child<QPushButton>(window, "savePresetButton");
  auto* control_point_matcher_label = require_child<QLabel>(window, "controlPointMatcherLabel");
  auto* mapping_backend_label = require_child<QLabel>(window, "mappingBackendLabel");
  auto* projection_label = require_child<QLabel>(window, "stitchProjectionLabel");
  auto* stitch_max_output_width_label = require_child<QLabel>(window, "stitchMaxOutputWidthLabel");
  std::array<QLabel*, 3> projection_parameter_labels = {
      require_child<QLabel>(window, "projectionParameter1Label"),
      require_child<QLabel>(window, "projectionParameter2Label"),
      require_child<QLabel>(window, "projectionParameter3Label")};
  const std::array<QDoubleSpinBox*, 3> projection_parameter_spins = {
      panini_compression, panini_top_squeeze, panini_bottom_squeeze};
  auto* clean_stitching = require_child<QPushButton>(window, "cleanStitchingButton");
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* bring_up_shadows = require_child<QSlider>(window, "cameraSlider_Bring_Up_Shadows");
  auto* render_video = require_child<QCheckBox>(window, "renderVideoCheck");
  auto* show_player_tracking = require_child<QCheckBox>(window, "showPlayerTrackingCheck");
  auto* show_play_tracking = require_child<QCheckBox>(window, "showPlayTrackingCheck");
  auto* show_rink_mask = require_child<QCheckBox>(window, "showRinkMaskCheck");
  auto* drivegpt_csv = require_child<QCheckBox>(window, "drivegptCsvCheck");
  auto* log = require_child<QTextEdit>(window, "runtimeLog");
  auto* clear_log = require_child<QPushButton>(window, "clearLogButton");
  auto* main_log_splitter = require_child<QSplitter>(window, "mainLogSplitter");
  auto* setup_preview_splitter = require_child<QSplitter>(window, "setupPreviewSplitter");
  auto* output_routing = require_child<QWidget>(window, "outputRoutingGroup");
  auto* preview_tabs = require_child<QTabWidget>(window, "previewTabs");
  auto* pipeline_inspector = require_child<QWidget>(window, "pipelineInspectorWidget");
  auto* program_host = require_child<QWidget>(window, "programLetterboxHost");
  auto* preview_surface = require_child<QWidget>(window, "previewSurface");
  auto* preview_target = require_child<QWidget>(window, "previewRenderTarget");
  auto* stitched_surface = require_child<QWidget>(window, "stitchedPreviewSurface");
  auto* stitched_target = require_child<QWidget>(window, "stitchedPreviewRenderTarget");
  auto* stitched_host = require_child<QWidget>(window, "stitchedLetterboxHost");
  auto* camera1_host = require_child<QWidget>(window, "camera1LetterboxHost");
  auto* camera1_surface = require_child<QWidget>(window, "camera1PreviewSurface");
  auto* camera1_target = require_child<QWidget>(window, "camera1PreviewRenderTarget");
  auto* camera1_focus = require_child<QPushButton>(window, "camera1FocusButton");
  auto* camera2_host = require_child<QWidget>(window, "camera2LetterboxHost");
  auto* camera2_surface = require_child<QWidget>(window, "camera2PreviewSurface");
  auto* camera2_target = require_child<QWidget>(window, "camera2PreviewRenderTarget");
  auto* camera2_focus = require_child<QPushButton>(window, "camera2FocusButton");
  auto* camera3_host = require_child<QWidget>(window, "camera3LetterboxHost");
  auto* camera3_surface = require_child<QWidget>(window, "camera3PreviewSurface");
  auto* camera3_target = require_child<QWidget>(window, "camera3PreviewRenderTarget");
  auto* camera3_focus = require_child<QPushButton>(window, "camera3FocusButton");
  auto* external_notice = require_child<QLabel>(window, "programExternalRenderNotice");
  auto* camera1_notice = require_child<QLabel>(window, "camera1ExternalRenderNotice");
  auto* stitched_status = require_child<QLabel>(window, "stitchedPreviewStatusLabel");
  auto* preview_status = require_child<QLabel>(window, "previewStatusLabel");
  auto* program_controls = require_child<QWidget>(window, "programAssociatedControls");
  auto* program_controls_toggle = require_child<QToolButton>(window, "programControlsToggle");
  auto* stitched_controls = require_child<QWidget>(window, "stitchedAssociatedControls");
  auto* stitched_bring_up_shadows = require_child<QSlider>(window, "stitchedCameraSlider_Bring_Up_Shadows");
  auto* stitched_exposure = require_child<QSlider>(window, "stitchedCameraSlider_Exposure_x100");
  auto* stitched_lift_black_point = require_child<QCheckBox>(window, "stitchedCameraCheck_Lift_Shadow_Black_Point");
  auto* stitched_force_high_bit = require_child<QCheckBox>(window, "stitchedCameraCheck_Use_10_Bit_Grading");
  auto* stitched_precision_status = require_child<QLabel>(window, "stitchedColorPrecisionStatus");
  auto* algorithms_scroll = require_child<QScrollArea>(window, "stitchingAlgorithmsScrollArea");
  auto* algorithms_page = require_child<QWidget>(window, "stitchingAlgorithmsTab");
  auto* program_control_tabs = require_child<QTabWidget>(window, "programControlTabs");
  auto* stitched_control_tabs = require_child<QTabWidget>(window, "stitchedControlTabs");
  auto* program_controls_splitter = require_child<QSplitter>(window, "programPreviewControlsSplitter");
  auto* stitched_controls_splitter = require_child<QSplitter>(window, "stitchedPreviewControlsSplitter");
  auto* program_focus = require_child<QPushButton>(window, "programFocusButton");
  auto* stitched_focus = require_child<QPushButton>(window, "stitchedFocusButton");
  auto* top_bar = require_child<QWidget>(window, "topBarPanel");
  auto* playback_progress = require_child<QProgressBar>(window, "playbackProgress");
  auto* seek_slider = require_child<QSlider>(window, "playbackSeekSlider");
  auto* seek_back = require_child<QPushButton>(window, "playbackSeekBack10Button");
  auto* seek_forward = require_child<QPushButton>(window, "playbackSeekForward10Button");
  auto* seek_position = require_child<QLabel>(window, "playbackSeekPosition");
  auto* setup_row = require_child<QWidget>(window, "setupControlsRow");
  auto* log_panel = require_child<QWidget>(window, "logPanel");
  auto* pipeline_process = window->findChild<QProcess*>();
  if (!stop || !start || !pause || !restart || !mode || !control_points || !calibration_frame_count ||
      !stitch_frame_time ||
      !control_point_matcher || !mapping_backend || !projection || !panini_compression || !panini_top_squeeze ||
      !panini_bottom_squeeze || !projection_auto_fov || !projection_horizontal_fov || !projection_auto_canvas ||
      !projection_auto_crop || !control_point_matcher_label || !mapping_backend_label || !projection_label ||
      !stitch_max_output_width || !run_autooptimizer || !save_preset_button || !stitch_max_output_width_label ||
      !projection_parameter_labels[0] || !projection_parameter_labels[1] || !projection_parameter_labels[2] ||
      !clean_stitching || !game_id || !rotate || !max_speed_x || !bring_up_shadows || !render_video ||
      !show_player_tracking ||
      !show_play_tracking || !show_rink_mask || !drivegpt_csv || !log || !clear_log || !main_log_splitter ||
      !setup_preview_splitter || !output_routing || !preview_tabs || !pipeline_inspector || !program_host ||
      !preview_surface || !preview_target || !stitched_surface || !stitched_target || !stitched_host || !camera1_host ||
      !camera1_surface || !camera1_target || !camera1_focus || !camera2_host || !camera2_surface || !camera2_target ||
      !camera2_focus || !camera3_host || !camera3_surface || !camera3_target || !camera3_focus || !external_notice ||
      !camera1_notice || !stitched_status || !preview_status || !program_controls || !program_controls_toggle ||
      !stitched_controls || !stitched_bring_up_shadows || !stitched_exposure || !stitched_lift_black_point ||
      !stitched_force_high_bit || !stitched_precision_status || !algorithms_scroll || !algorithms_page ||
      !program_control_tabs || !stitched_control_tabs || !program_controls_splitter || !stitched_controls_splitter ||
      !program_focus || !stitched_focus || !top_bar || !setup_row || !log_panel || !playback_progress ||
      !seek_slider || !seek_back ||
      !seek_forward || !seek_position || !pipeline_process) {
    return false;
  }

  const int pipeline_inspector_index = preview_tabs->indexOf(pipeline_inspector);
  if (!expect(
          pipeline_inspector_index >= 0 && preview_tabs->tabText(pipeline_inspector_index) == "Pipeline",
          "The live GStreamer pipeline inspector should be available as a preview workspace tab")) {
    return false;
  }

  const QPixmap application_icon = window->windowIcon().pixmap(256, 256);
  const QImage application_icon_image = application_icon.toImage().convertToFormat(QImage::Format_ARGB32);
  if (!expect(
          QCoreApplication::applicationName() == "hstream-ui" &&
              QGuiApplication::applicationDisplayName() == "HStream" &&
              QGuiApplication::desktopFileName() == "hstream-ui" && !window->windowIcon().isNull() &&
              !application_icon.isNull() && application_icon.size() == QSize(256, 256) &&
              qAlpha(application_icon_image.pixel(0, 0)) == 0 && qAlpha(application_icon_image.pixel(128, 128)) == 255,
          "HStream should expose matching desktop identity and a scalable application icon with transparent corners")) {
    return false;
  }
  if (!expect_x11_application_icon(window))
    return false;
  if (!expect(
          !show_player_tracking->isChecked() && !show_play_tracking->isChecked() && !show_rink_mask->isChecked() &&
              show_player_tracking->isEnabled() && show_play_tracking->isEnabled() && show_rink_mask->isEnabled() &&
              show_player_tracking->toolTip().contains("encoded output") &&
              show_play_tracking->toolTip().contains("both GPU previews") &&
              show_rink_mask->toolTip().contains("translucent green"),
          "Render controls should expose disabled-by-default GPU-only player, play-tracker, and rink-mask layers")) {
    return false;
  }
  const QString icon_artifact_dir = qEnvironmentVariable("HSTREAM_UI_X11_ARTIFACT_DIR");
  if (!icon_artifact_dir.isEmpty() &&
      (!QDir().mkpath(icon_artifact_dir) ||
       !application_icon.save(QDir(icon_artifact_dir).filePath("hstream-app-icon.png")))) {
    return expect(false, "Could not save the HStream application-icon test artifact");
  }

  if (!expect(
          main_log_splitter->orientation() == Qt::Vertical && main_log_splitter->count() == 2,
          "Main content and runtime log should be separated by a draggable vertical splitter")) {
    return false;
  }
  if (!expect(
          program_control_tabs->minimumHeight() == 220 &&
              program_control_tabs->sizePolicy().verticalPolicy() == QSizePolicy::Expanding &&
              stitched_control_tabs->minimumHeight() == 220 &&
              stitched_control_tabs->sizePolicy().verticalPolicy() == QSizePolicy::Expanding,
          "Program and Stitched configuration areas should expand inside their side control panes")) {
    return false;
  }
  const QRect stopped_target_rect(preview_target->mapTo(window, QPoint(0, 0)), preview_target->size());
  const QRect tab_bar_rect(preview_tabs->tabBar()->mapTo(window, QPoint(0, 0)), preview_tabs->tabBar()->size());
  if (!expect(
          preview_target->isHidden() && camera1_target->isHidden() && !stopped_target_rect.intersects(tab_bar_rect),
          "Stopped native video targets must stay unmapped and geometrically below the preview tab bar")) {
    std::cerr << "stopped_target=" << stopped_target_rect.x() << ',' << stopped_target_rect.y() << ' '
              << stopped_target_rect.width() << 'x' << stopped_target_rect.height() << " tab_bar=" << tab_bar_rect.x()
              << ',' << tab_bar_rect.y() << ' ' << tab_bar_rect.width() << 'x' << tab_bar_rect.height() << '\n';
    return false;
  }
  if (!expect_x11_widget_state(
          preview_target, false, "Stopped Program target must use its Qt host as the native X11 coordinate space")) {
    return false;
  }
  const int preview_height_before_setup_collapse = preview_tabs->height();
  setup_preview_splitter->setSizes({0, setup_preview_splitter->height()});
  QApplication::processEvents();
  if (!expect(
          setup_preview_splitter->orientation() == Qt::Vertical && setup_preview_splitter->count() == 2 &&
              setup_preview_splitter->sizes().at(0) == 0 &&
              preview_tabs->height() >= preview_height_before_setup_collapse,
          "Dragging the setup splitter upward should collapse Video Sets and grow the video preview") ||
      !expect(
          output_routing->sizePolicy().verticalPolicy() == QSizePolicy::Maximum &&
              output_routing->height() <= output_routing->sizeHint().height() + 2,
          "Output Routing should use compact natural row spacing instead of stretching vertically")) {
    return false;
  }
  setup_preview_splitter->setSizes({240, 440});
  QApplication::processEvents();
  bool all_matchers_enabled = control_point_matcher->model();
  for (int index = 0; all_matchers_enabled && index < control_point_matcher->count(); ++index) {
    all_matchers_enabled =
        control_point_matcher->model()->flags(control_point_matcher->model()->index(index, 0)) & Qt::ItemIsEnabled;
  }
  const QString original_mapping_backend = mapping_backend->currentData().toString();
  const QString original_projection = projection->currentData().toString();
  window->resize(1440, 900);
  preview_tabs->setCurrentIndex(1);
  stitched_control_tabs->setCurrentIndex(2);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  QApplication::processEvents();
  if (!expect(
          stitched_control_tabs->height() >= 220 &&
              algorithms_scroll->viewport()->height() > 40,
          "The rendered Stitched configuration area should expand without crushing content")) {
    return false;
  }
  bool all_nona_projections_enabled = projection->model() && projection->count() == 22;
  for (int index = 0; all_nona_projections_enabled && index < projection->count(); ++index) {
    all_nona_projections_enabled = projection->model()->flags(projection->model()->index(index, 0)) & Qt::ItemIsEnabled;
  }
  projection->setCurrentIndex(projection->findData("general-panini"));
  QApplication::processEvents();
  const bool general_panini_framing_defaults = projection_auto_fov->isEnabled() && !projection_auto_fov->isChecked() &&
      projection_horizontal_fov->isEnabled() && projection_horizontal_fov->value() == 180.0 &&
      projection_horizontal_fov->maximum() == 319.91 && projection_auto_canvas->isEnabled() &&
      projection_auto_canvas->isChecked() && projection_auto_crop->isEnabled() && !projection_auto_crop->isChecked();
  projection_auto_fov->setChecked(true);
  QApplication::processEvents();
  const bool auto_fov_disables_fixed_value = !projection_horizontal_fov->isEnabled();
  projection_auto_fov->setChecked(false);
  projection_horizontal_fov->setValue(185.0);
  projection_auto_canvas->setChecked(false);
  projection_auto_crop->setChecked(true);
  projection->setCurrentIndex(projection->findData("rectilinear"));
  QApplication::processEvents();
  const bool rectilinear_fov_limit =
      projection_horizontal_fov->maximum() == 179.0 && projection_horizontal_fov->value() == 179.0;
  projection->setCurrentIndex(projection->findData("stereographic"));
  QApplication::processEvents();
  const bool wide_projection_fov_limit = projection_horizontal_fov->maximum() == 359.0;
  projection->setCurrentIndex(projection->findData("general-panini"));
  const bool general_panini_fov_restored =
      projection_horizontal_fov->maximum() == 319.91 && projection_horizontal_fov->value() == 185.0;
  projection_horizontal_fov->setValue(180.0);
  projection_auto_canvas->setChecked(true);
  projection_auto_crop->setChecked(false);
  QApplication::processEvents();
  const bool general_panini_parameters_visible = !panini_compression->isHidden() && !panini_top_squeeze->isHidden() &&
      !panini_bottom_squeeze->isHidden() && panini_compression->isEnabled() && panini_compression->value() == 100.0 &&
      panini_top_squeeze->value() == 0.0 && panini_bottom_squeeze->value() == 0.0 &&
      panini_compression->minimum() == 0.0 && panini_compression->maximum() == 150.0 &&
      panini_top_squeeze->minimum() == -100.0 && panini_top_squeeze->maximum() == 100.0 &&
      panini_compression->toolTip().contains("standard Panini") &&
      panini_compression->toolTip().contains("320 degrees") && panini_top_squeeze->toolTip().contains("hard squeeze") &&
      panini_top_squeeze->toolTip().contains("soft squeeze") &&
      panini_bottom_squeeze->toolTip().contains("bottom half");
  projection->setCurrentIndex(projection->findData("albers-equal-area-conic"));
  QApplication::processEvents();
  const bool albers_parameters_visible = !panini_compression->isHidden() && !panini_top_squeeze->isHidden() &&
      panini_bottom_squeeze->isHidden() && panini_compression->value() == 0.0 && panini_top_squeeze->value() == 60.0 &&
      panini_compression->property("huginParameterName") == "phi1" &&
      panini_top_squeeze->property("huginParameterName") == "phi2";
  projection->setCurrentIndex(projection->findData("biplane"));
  QApplication::processEvents();
  const bool biplane_parameters_visible = !panini_compression->isHidden() && !panini_top_squeeze->isHidden() &&
      panini_bottom_squeeze->isHidden() && panini_compression->value() == 45.0 && panini_top_squeeze->value() == 0.0 &&
      panini_top_squeeze->maximum() == 1.0 && panini_top_squeeze->decimals() == 0 &&
      panini_top_squeeze->toolTip().contains("cylindrical section");
  projection->setCurrentIndex(projection->findData("triplane"));
  QApplication::processEvents();
  const bool triplane_parameters_visible = !panini_compression->isHidden() && panini_top_squeeze->isHidden() &&
      panini_bottom_squeeze->isHidden() && panini_compression->value() == 60.0 &&
      panini_compression->maximum() == 120.0;
  projection->setCurrentIndex(projection->findData("equirectangular-panini"));
  QApplication::processEvents();
  const bool fixed_panini_parameters_hidden =
      panini_compression->isHidden() && panini_top_squeeze->isHidden() && panini_bottom_squeeze->isHidden();
  projection->setCurrentIndex(projection->findData("general-panini"));
  mapping_backend->setCurrentIndex(mapping_backend->findData("opencv-magsac"));
  QApplication::processEvents();
  const bool parameters_hidden_for_native_backend =
      panini_compression->isHidden() && panini_top_squeeze->isHidden() && panini_bottom_squeeze->isHidden();
  bool only_rectilinear_enabled = projection->currentData().toString() == "rectilinear";
  for (int index = 0; only_rectilinear_enabled && index < projection->count(); ++index) {
    const bool enabled = projection->model()->flags(projection->model()->index(index, 0)) & Qt::ItemIsEnabled;
    only_rectilinear_enabled = enabled == (projection->itemData(index).toString() == "rectilinear");
  }
  auto projection_layout_is_legible = [&]() {
    const auto parsed_projection =
        hm::stitching::ParseStitchProjection(projection->currentData().toString().toStdString());
    const size_t expected_parameter_count =
        parsed_projection.ok() && mapping_backend->currentData().toString() == "nona"
        ? hm::stitching::StitchProjectionParameters(*parsed_projection).size()
        : 0;
    bool legible = algorithms_scroll->isVisible() && algorithms_page->isVisible() &&
        (algorithms_page->height() <= algorithms_scroll->viewport()->height() ||
         algorithms_scroll->verticalScrollBar()->maximum() > 0) &&
        projection->width() >= projection->minimumSizeHint().width() &&
        projection->width() >= projection->fontMetrics().horizontalAdvance(projection->currentText()) + 36;
    for (size_t parameter_index = 0; parameter_index < projection_parameter_labels.size(); ++parameter_index) {
      QLabel* label = projection_parameter_labels[parameter_index];
      QDoubleSpinBox* spin = projection_parameter_spins[parameter_index];
      const bool expected_visible = parameter_index < expected_parameter_count;
      legible = legible && label->isVisible() == expected_visible && spin->isVisible() == expected_visible;
      if (!expected_visible)
        continue;
      legible = legible && label->width() >= label->sizeHint().width() &&
          label->height() >= label->minimumSizeHint().height() && spin->width() >= spin->minimumSizeHint().width() &&
          spin->height() >= spin->minimumSizeHint().height() && !label->geometry().intersects(spin->geometry()) &&
          algorithms_page->contentsRect().contains(label->geometry()) &&
          algorithms_page->contentsRect().contains(spin->geometry());
    }
    if (!legible) {
      std::cerr << "projection layout is not legible: backend="
                << mapping_backend->currentData().toString().toStdString()
                << " projection=" << projection->currentData().toString().toStdString()
                << " page-visible=" << algorithms_page->isVisible() << " projection-width=" << projection->width()
                << " projection-min-width=" << projection->minimumSizeHint().width()
                << " text-width=" << projection->fontMetrics().horizontalAdvance(projection->currentText()) << '\n';
      for (size_t parameter_index = 0; parameter_index < projection_parameter_labels.size(); ++parameter_index) {
        QLabel* label = projection_parameter_labels[parameter_index];
        QDoubleSpinBox* spin = projection_parameter_spins[parameter_index];
        std::cerr << "parameter " << parameter_index
                  << " expected-visible=" << (parameter_index < expected_parameter_count)
                  << " label-visible=" << label->isVisible() << " label=" << label->width() << 'x' << label->height()
                  << " label-hint=" << label->sizeHint().width() << 'x' << label->sizeHint().height()
                  << " spin-visible=" << spin->isVisible() << " spin=" << spin->width() << 'x' << spin->height()
                  << " spin-min-hint=" << spin->minimumSizeHint().width() << 'x' << spin->minimumSizeHint().height()
                  << '\n';
      }
    }
    return legible;
  };
  bool all_projection_layouts_legible = true;
  bool all_projection_artifacts_captured = true;
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  for (int index = 0; index < projection->count(); ++index) {
    projection->setCurrentIndex(index);
    QApplication::processEvents();
    const QString projection_name = projection->currentData().toString();
    all_projection_layouts_legible = all_projection_layouts_legible && projection_layout_is_legible();
    all_projection_artifacts_captured = all_projection_artifacts_captured &&
        capture_widget_artifact(algorithms_page, QString("stitching-algorithms-nona-%1.png").arg(projection_name));
  }
  const std::array<std::pair<QString, QString>, 2> native_backends = {{
      {QStringLiteral("opencv-magsac"), QStringLiteral("MAGSAC++")},
      {QStringLiteral("opencv-affine-ransac"), QStringLiteral("RANSAC")},
  }};
  for (const auto& [backend_name, backend_label] : native_backends) {
    const int backend_index = mapping_backend->findData(backend_name);
    mapping_backend->setCurrentIndex(mapping_backend->findData(backend_name));
    QApplication::processEvents();
    all_projection_layouts_legible = all_projection_layouts_legible && backend_index >= 0 &&
        mapping_backend->currentData().toString() == backend_name && mapping_backend->currentText() == backend_label &&
        projection->currentData().toString() == "rectilinear" && projection_layout_is_legible();
    all_projection_artifacts_captured = all_projection_artifacts_captured &&
        capture_widget_artifact(algorithms_page, QString("stitching-algorithms-%1-rectilinear.png").arg(backend_name));
  }
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData(original_projection));
  mapping_backend->setCurrentIndex(mapping_backend->findData(original_mapping_backend));
  QApplication::processEvents();
  if (!general_panini_framing_defaults || !auto_fov_disables_fixed_value || !rectilinear_fov_limit ||
      !wide_projection_fov_limit || !general_panini_fov_restored || !general_panini_parameters_visible ||
      !albers_parameters_visible || !biplane_parameters_visible || !triplane_parameters_visible ||
      !fixed_panini_parameters_hidden || !parameters_hidden_for_native_backend || !all_projection_layouts_legible ||
      !all_projection_artifacts_captured) {
    std::cerr << "projection-control diagnostics: panini-defaults=" << general_panini_framing_defaults
              << " auto-disables-fixed=" << auto_fov_disables_fixed_value
              << " rectilinear-limit=" << rectilinear_fov_limit << " wide-limit=" << wide_projection_fov_limit
              << " panini-restored=" << general_panini_fov_restored
              << " panini-parameters=" << general_panini_parameters_visible
              << " albers-parameters=" << albers_parameters_visible
              << " biplane-parameters=" << biplane_parameters_visible
              << " triplane-parameters=" << triplane_parameters_visible
              << " fixed-panini-hidden=" << fixed_panini_parameters_hidden
              << " native-hidden=" << parameters_hidden_for_native_backend
              << " layouts=" << all_projection_layouts_legible << " artifacts=" << all_projection_artifacts_captured
              << '\n';
  }
  if (!expect(
          program_controls->isAncestorOf(max_speed_x) && program_controls->isAncestorOf(bring_up_shadows) &&
              stitched_controls->isAncestorOf(rotate) && stitched_controls->isAncestorOf(stitched_bring_up_shadows) &&
              stitched_controls->isAncestorOf(stitched_exposure) &&
              stitched_controls->isAncestorOf(stitched_lift_black_point) &&
              stitched_controls->isAncestorOf(stitched_force_high_bit) &&
              stitched_controls->isAncestorOf(stitched_precision_status) && !camera1_host->isAncestorOf(max_speed_x) &&
              !camera1_host->isAncestorOf(bring_up_shadows) && !camera1_host->isAncestorOf(rotate) &&
              stitched_controls->isAncestorOf(control_point_matcher) &&
              stitched_controls->isAncestorOf(mapping_backend) && stitched_controls->isAncestorOf(projection) &&
              stitched_controls->isAncestorOf(control_points) && stitched_controls->isAncestorOf(stitch_frame_time) &&
              stitched_controls->isAncestorOf(stitch_max_output_width) &&
              stitched_controls->isAncestorOf(run_autooptimizer) && program_control_tabs->count() == 4 &&
              stitched_control_tabs->count() == 3 && stitched_control_tabs->tabText(1) == "Color & Precision" &&
              stitched_control_tabs->tabText(2) == "Algorithms" &&
              program_controls_splitter->orientation() == Qt::Horizontal &&
              stitched_controls_splitter->orientation() == Qt::Horizontal &&
              control_point_matcher_label->text() == "Control-point matcher" &&
              mapping_backend_label->text() == "Mapping backend" && projection_label->text() == "Projection" &&
              stitch_max_output_width_label->text() == "Max stitched width" && stitch_max_output_width->value() == 0 &&
              stitch_max_output_width->maximum() == std::numeric_limits<int>::max() &&
              clean_stitching->text() == "Clean Stitching" && stitch_frame_time->isEnabled() == false &&
              !run_autooptimizer->isChecked() && !run_autooptimizer->isEnabled() &&
              mapping_backend->currentData().toString() == "opencv-magsac" && control_point_matcher->count() == 4 &&
              control_point_matcher->itemText(0) == "SuperPoint + LightGlue" &&
              control_point_matcher->itemText(1) == "DeDoDe + LightGlue" &&
              control_point_matcher->itemText(2) == "LoFTR (EfficientLoFTR outdoor)" &&
              control_point_matcher->itemText(3) == "AKAZE + M-LDB + Hamming" && all_matchers_enabled &&
              mapping_backend->count() == 3 && mapping_backend->itemText(0) == "NONA" &&
              mapping_backend->itemText(1) == "MAGSAC++" && mapping_backend->itemText(2) == "RANSAC" &&
              projection->findData("general-panini") >= 0 && all_nona_projections_enabled && only_rectilinear_enabled &&
              general_panini_parameters_visible && albers_parameters_visible && biplane_parameters_visible &&
              triplane_parameters_visible && fixed_panini_parameters_hidden && parameters_hidden_for_native_backend &&
              general_panini_framing_defaults && auto_fov_disables_fixed_value && rectilinear_fov_limit &&
              wide_projection_fov_limit && general_panini_fov_restored && !projection_auto_fov->isEnabled() &&
              !projection_horizontal_fov->isEnabled() && !projection_auto_canvas->isEnabled() &&
              !projection_auto_crop->isEnabled() && all_projection_layouts_legible && all_projection_artifacts_captured,
          "Algorithm controls must expose compatible projections in the earliest preview tab whose frames reflect "
          "their pipeline stage")) {
    return false;
  }
  const bool save_enabled_before_inactive_optimizer_toggle = save_preset_button->isEnabled();
  run_autooptimizer->setChecked(true);
  QApplication::processEvents();
  if (!expect(
          !run_autooptimizer->isChecked() && !run_autooptimizer->isEnabled() &&
              save_preset_button->isEnabled() == save_enabled_before_inactive_optimizer_toggle,
          "The inactive optimizer control must reject changes under OpenCV backends without dirtying the preset")) {
    return false;
  }
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  QApplication::processEvents();
  if (!expect(
      run_autooptimizer->isChecked() && run_autooptimizer->isEnabled(),
      "Selecting the NONA mapping backend must enable its required panorama autooptimizer")) {
    return false;
  }
  run_autooptimizer->setChecked(false);
  QApplication::processEvents();
  if (!expect(
          mapping_backend->currentData().toString() == "opencv-magsac",
          "Turning the autooptimizer off while NONA is selected must return to the default MAGSAC++ backend")) {
    return false;
  }
  calibration_frame_count->setValue(1);
  QApplication::processEvents();
  if (!expect(
          stitch_frame_time->isEnabled(), "Single-frame stitching calibration should enable reference-frame time")) {
    return false;
  }
  calibration_frame_count->setValue(4);
  QApplication::processEvents();
  if (!expect(
          !stitch_frame_time->isEnabled(),
          "Multi-frame stitching calibration should gray out reference-frame time")) {
    return false;
  }

  preview_tabs->setCurrentIndex(1);
  stitched_control_tabs->setCurrentIndex(2);
  QApplication::processEvents();
  if (!capture_interaction_artifact(window, "stitching-algorithms-controls.png"))
    return false;
  preview_tabs->setCurrentIndex(0);
  QApplication::processEvents();
  const std::array<std::pair<QWidget*, QPushButton*>, 5> stopped_focus_controls = {{
      {preview_target, program_focus},
      {stitched_target, stitched_focus},
      {camera1_target, camera1_focus},
      {camera2_target, camera2_focus},
      {camera3_target, camera3_focus},
  }};
  const bool all_stopped_focus_controls_ready =
      std::all_of(stopped_focus_controls.begin(), stopped_focus_controls.end(), [](const auto& item) {
        return item.second->parentWidget() == item.first && item.second->size() == QSize(24, 24) &&
            item.second->isHidden() && !item.second->isEnabled();
      });
  if (!expect(
          all_stopped_focus_controls_ready && program_focus->parentWidget() == preview_target &&
              program_focus->size() == QSize(24, 24) &&
              program_focus->x() == preview_target->width() - program_focus->width() - 6 && program_focus->y() == 6 &&
              program_focus->toolTip().contains("Expand the Program preview") &&
              program_focus->accessibleName() == "Focus video" && program_focus->isHidden() &&
              !program_focus->isEnabled(),
          "Every compact focus control must stay hidden until its preview has presented a GPU frame") ||
      !expect_x11_widget_state(
          program_focus,
          false,
          "The stopped focus control must remain an unmapped native child of the video target",
          false)) {
    return false;
  }
  QTest::mouseDClick(preview_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          top_bar->isVisible() && setup_row->isVisible() && log_panel->isVisible() &&
              preview_tabs->tabBar()->isVisible() && program_controls->isVisible() && program_focus->isHidden(),
          "A real double-click while stopped must not enter an empty all-black focus layout")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "stopped-double-click-no-op.png"))
    return false;
  if (!expect(
          window->findChild<QLineEdit*>("pluginPropertyEdit") == nullptr,
          "The inert generic plugin field should not be presented as a working video control")) {
    return false;
  }
  if (!expect(
          window->findChild<QSlider*>("cameraSlider_Exposure_EV_x10") == nullptr &&
              window->findChild<QSlider*>("cameraSlider_Left_Brightness_Multiplier_x100") == nullptr,
          "Controls without a native GPU pipeline consumer must not claim association with a preview")) {
    return false;
  }
  window->resize(1440, 900);
  QApplication::processEvents();
  const QSize minimum_hint = window->minimumSizeHint();
  if (!expect(
          minimum_hint.width() <= 1440 && minimum_hint.height() <= 900,
          "The normal UI minimum size must fit the supported 1440x900 viewport")) {
    std::cerr << "minimumSizeHint=" << minimum_hint.width() << 'x' << minimum_hint.height() << '\n';
    const std::array<QWidget*, 8> diagnostic_widgets = {
        window->centralWidget(),
        top_bar,
        main_log_splitter,
        setup_row,
        preview_tabs,
        program_control_tabs,
        stitched_control_tabs,
        log_panel,
    };
    for (QWidget* widget : diagnostic_widgets) {
      const QSize child_hint = widget->minimumSizeHint();
      std::cerr << widget->objectName().toStdString() << " minimumSizeHint=" << child_hint.width() << 'x'
                << child_hint.height() << " minimumSize=" << widget->minimumWidth() << 'x' << widget->minimumHeight()
                << " size=" << widget->width() << 'x' << widget->height() << '\n';
    }
    return false;
  }
  const QString invalid_pair_config_path = QDir(window->gameDirectoryText()).filePath("config.yaml");
  const QString invalid_pair_artifact_path = QDir(window->gameDirectoryText()).filePath("seam_file.png");
  QFile invalid_pair_config(invalid_pair_config_path);
  if (!invalid_pair_config.open(QIODevice::ReadOnly))
    return false;
  const QByteArray invalid_pair_config_before = invalid_pair_config.readAll();
  invalid_pair_config.close();
  QFile existing_artifact(invalid_pair_artifact_path);
  const bool artifact_existed = existing_artifact.exists();
  QByteArray artifact_before;
  if (artifact_existed) {
    if (!existing_artifact.open(QIODevice::ReadOnly))
      return false;
    artifact_before = existing_artifact.readAll();
    existing_artifact.close();
  }
  QFile invalid_pair_artifact(invalid_pair_artifact_path);
  if (!invalid_pair_artifact.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      invalid_pair_artifact.write("valid stitching artifact") < 0) {
    return false;
  }
  invalid_pair_artifact.close();
  {
    const QSignalBlocker backend_signals(mapping_backend);
    const QSignalBlocker optimizer_signals(run_autooptimizer);
    mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
    run_autooptimizer->setChecked(false);
  }
  const int invalid_pair_clean_commands = window->logText().count("stitching calibration clean command");
  activate(start);
  QFile invalid_pair_config_after_file(invalid_pair_config_path);
  QFile invalid_pair_artifact_after_file(invalid_pair_artifact_path);
  const bool read_invalid_pair_files = invalid_pair_config_after_file.open(QIODevice::ReadOnly) &&
      invalid_pair_artifact_after_file.open(QIODevice::ReadOnly);
  const QByteArray invalid_pair_config_after =
      read_invalid_pair_files ? invalid_pair_config_after_file.readAll() : QByteArray();
  const QByteArray invalid_pair_artifact_after =
      read_invalid_pair_files ? invalid_pair_artifact_after_file.readAll() : QByteArray();
  invalid_pair_config_after_file.close();
  invalid_pair_artifact_after_file.close();
  const bool invalid_pair_rejected = expect(
      read_invalid_pair_files && window->pipelineStateText() == "STOPPED" &&
          window->logText().contains("NONA mapping backend requires Run panorama autooptimizer") &&
          window->logText().count("stitching calibration clean command") == invalid_pair_clean_commands &&
          invalid_pair_config_after == invalid_pair_config_before &&
          invalid_pair_artifact_after == "valid stitching artifact",
      "An invalid NONA-without-autooptimizer state must be rejected before config publication or artifact cleanup");
  if (artifact_existed) {
    QFile restore_artifact(invalid_pair_artifact_path);
    if (!restore_artifact.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        restore_artifact.write(artifact_before) != artifact_before.size()) {
      return false;
    }
  } else {
    QFile::remove(invalid_pair_artifact_path);
  }
  mapping_backend->setCurrentIndex(mapping_backend->findData("opencv-magsac"));
  run_autooptimizer->setChecked(false);
  if (!invalid_pair_rejected)
    return false;
  const bool architecture_supports_x11_embedding =
#if defined(__x86_64__)
      true;
#else
      false;
#endif
  if (!expect(
          hm::ui_internal::supports_x11_embedding("xcb") == architecture_supports_x11_embedding &&
              !hm::ui_internal::supports_x11_embedding("wayland") &&
              !hm::ui_internal::supports_x11_embedding("offscreen") &&
              !hm::ui_internal::supports_x11_embedding("xcb", true) &&
              hm::ui_internal::preview_channel_for_tab(0, 3) == "program" &&
              hm::ui_internal::preview_channel_for_tab(1, 3) == "stitched" &&
              hm::ui_internal::preview_channel_for_tab(2, 3) == "source0" &&
              hm::ui_internal::preview_channel_for_tab(4, 3) == "source2" &&
              hm::ui_internal::preview_channel_for_tab(5, 3).isEmpty(),
          "Native preview support and tab-specific backend channel mapping should remain explicit")) {
    return false;
  }

  activate(stop);
  if (!expect(window->pipelineStateText() == "STOPPED", "Stop button should stop the pipeline")) {
    return false;
  }
  const QString valid_game_id = game_id->text();
  game_id->clear();
  activate(start);
  if (!expect(
          window->pipelineStateText() == "STOPPED" && playback_progress->isVisible() &&
              playback_progress->format().contains("ERROR") &&
              playback_progress->toolTip().contains("selected game directory could not be prepared"),
          "A game-directory validation failure must restore STOPPED and show a terminal startup error")) {
    return false;
  }
  game_id->setText(valid_game_id);
  if (!expect(!drivegpt_csv->isChecked(), "DriveGPT CSV export must default off so Program seeking is available")) {
    return false;
  }
  const QString telemetry_option_prefix = "--options=pipeline.ds-playtracker.private-properties.telemetry-csv-dir=";
  const QStringList telemetry_disabled_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  if (!expect(
          std::none_of(
              telemetry_disabled_arguments.cbegin(),
              telemetry_disabled_arguments.cend(),
              [&telemetry_option_prefix](const QString& argument) {
                return argument.startsWith(telemetry_option_prefix);
              }),
          "The default Program run must leave DriveGPT CSV disabled so seeking remains available")) {
    return false;
  }
  drivegpt_csv->setChecked(true);
  const QStringList telemetry_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  const auto telemetry_argument = std::find_if(
      telemetry_arguments.cbegin(), telemetry_arguments.cend(), [&telemetry_option_prefix](const QString& argument) {
        return argument.startsWith(telemetry_option_prefix);
      });
  const QString telemetry_work_directory = telemetry_argument == telemetry_arguments.cend()
      ? QString()
      : telemetry_argument->mid(telemetry_option_prefix.size());
  if (!expect(
          !telemetry_work_directory.isEmpty() &&
              QDir::cleanPath(telemetry_work_directory) != QDir::cleanPath(window->gameDirectoryText()) &&
              QFileInfo(telemetry_work_directory).fileName() == window->gameIdText(),
          "The DriveGPT checkbox should stage metadata under HStream working storage, never in the HM game directory")) {
    return false;
  }
  drivegpt_csv->setChecked(false);
  if (!expect(control_points->value() == 1500, "Stitching calibration CP default should be 1500") ||
      !expect(
          stitch_frame_time->time() == QTime(0, 0, 0) && !stitch_frame_time->isEnabled() &&
              stitched_controls->isAncestorOf(stitch_frame_time),
          "Stitch-frame time should default to 00:00:00 inside the stitched calibration controls")) {
    return false;
  }
  calibration_frame_count->setValue(1);
  QApplication::processEvents();

  preview_tabs->setCurrentIndex(1);
  stitched_control_tabs->setCurrentIndex(2);
  QApplication::processEvents();
  window->activateWindow();
  stitch_frame_time->setFocus(Qt::OtherFocusReason);
  QTest::qWait(10);
  auto* stitch_frame_line_edit = stitch_frame_time->findChild<QLineEdit*>();
  stitch_frame_time->setCurrentSection(QDateTimeEdit::MSecSection);
  QTest::keyClick(stitch_frame_time, Qt::Key_Up);
  QTest::keyClick(stitch_frame_time, Qt::Key_Tab);
  QApplication::processEvents();
  if (!expect(
          stitch_frame_time->displayedSections().testFlag(QDateTimeEdit::MSecSection) &&
              stitch_frame_time->time() == QTime(0, 0, 0, 1),
          QString(
              "The default stitch-frame editor should accept millisecond keyboard input (format=%1, time=%2, "
              "section=%3, focus=%4, text=%5)")
              .arg(stitch_frame_time->displayFormat())
              .arg(stitch_frame_time->time().toString("HH:mm:ss.zzz"))
              .arg(static_cast<int>(stitch_frame_time->currentSection()))
              .arg(stitch_frame_time->hasFocus())
              .arg(stitch_frame_line_edit ? stitch_frame_line_edit->text() : QString("missing"))
              .toStdString())) {
    return false;
  }
  mode->setFocus();
  stitch_frame_time->setTime(QTime(0, 0, 0));
  QApplication::processEvents();
  if (!expect(
          stitch_frame_time->displayFormat() == "HH:mm:ss",
          "An unfocused zero stitch-frame value should display as 00:00:00")) {
    return false;
  }

  const fs::path stitch_time_transition_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  {
    YAML::Node completed_at_zero = YAML::LoadFile(stitch_time_transition_config.string());
    YAML::Node calibration = completed_at_zero["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points->value();
    calibration["status"] = "complete";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    calibration.remove("invalidation_id");
    YAML::Node stitching = completed_at_zero["stitching"];
    if (stitching && stitching.IsMap())
      stitching.remove("stitch_frame_time");
    const auto published = hm::stitching::publish_game_config(
        stitch_time_transition_config.parent_path(), YAML::Dump(completed_at_zero) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not prepare the zero-to-nonzero stitch-frame Play regression: " << published << '\n';
      return false;
    }
  }
  auto* save_preset = require_child<QPushButton>(window, "savePresetButton");
  mode->setCurrentIndex(mode->findData("program"));
  stitch_frame_time->setTime(QTime(0, 0, 7));
  QApplication::processEvents();
  if (!save_preset ||
      !expect(save_preset->isEnabled(), "Changing stitch-frame time should be unsaved before Play is pressed")) {
    return false;
  }
  const int nonzero_argument_count = window->logText().count("--stitch-frame-time=00:00:07");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS", "500");
  activate(start);
  const YAML::Node pending_nonzero = YAML::LoadFile(stitch_time_transition_config.string());
  const YAML::Node pending_nonzero_calibration = pending_nonzero["hstream_ui"]["stitching_calibration"];
  const bool zero_to_nonzero_play_ok =
      expect(
          window->logText().count("--stitch-frame-time=00:00:07") == nonzero_argument_count + 1,
          "Play must pass a newly edited nonzero stitch-frame time to hstream-cli without requiring Save Preset") &&
      expect(
          pending_nonzero["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:07",
          "Play must persist a newly edited non-default stitch-frame time in config.yaml") &&
      expect(
          pending_nonzero_calibration["status"].as<std::string>() == "pending" &&
              pending_nonzero_calibration["stale_from"].as<std::string>() == "input" &&
              pending_nonzero_calibration["artifacts_invalidated"].as<bool>() &&
              pending_nonzero_calibration["invalidation_id"].IsScalar(),
          "Changing stitch-frame time at Play must invalidate input calibration before launching playback") &&
      expect(!save_preset->isEnabled(), "Play should capture the persisted stitch-frame time as the saved value");
  for (int i = 0; i < 400 &&
       !window->logText().contains("one-pass stitching calibration complete; continuous program playback running");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node completed_nonzero = YAML::LoadFile(stitch_time_transition_config.string());
  const bool nonzero_playback_restart_ok =
      expect(
          window->pipelineStateText() == "PLAYING" &&
              window->logText().contains(
                  "one-pass stitching calibration complete; continuous program playback running"),
          "Program Play must stay running after recalibrating at a newly edited stitch-frame time") &&
      expect(
          completed_nonzero["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "complete" &&
              completed_nonzero["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:07",
          "Playback restart must persist completed calibration at the newly edited stitch-frame time");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS");
  if (!zero_to_nonzero_play_ok || !nonzero_playback_restart_ok)
    return false;
  stitch_frame_time->setTime(QTime(0, 0, 0));
  QApplication::processEvents();

  {
    YAML::Node legacy_completed = YAML::LoadFile(stitch_time_transition_config.string());
    YAML::Node calibration = legacy_completed["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points->value();
    calibration["status"] = "complete";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    calibration.remove("invalidation_id");
    YAML::Node stitching = legacy_completed["stitching"];
    if (stitching && stitching.IsMap()) {
      stitching.remove("stitch_frame_time");
      stitching.remove("max_output_width");
    }
    const auto published = hm::stitching::publish_game_config(
        stitch_time_transition_config.parent_path(), YAML::Dump(legacy_completed) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not prepare the legacy max-width Play regression: " << published << '\n';
      return false;
    }
  }
  const int max_width_clean_commands = window->logText().count("stitching calibration clean command");
  stitch_max_output_width->setValue(4096);
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS", "500");
  qputenv("HSTREAM_UI_TEST_CANVAS_CHECK", "regenerate");
  activate(start);
  qunsetenv("HSTREAM_UI_TEST_CANVAS_CHECK");
  const YAML::Node pending_max_width = YAML::LoadFile(stitch_time_transition_config.string());
  const YAML::Node pending_max_width_calibration = pending_max_width["hstream_ui"]["stitching_calibration"];
  const bool legacy_max_width_invalidated =
      expect(
          pending_max_width["stitching"]["max_output_width"].as<int>() == 4096,
          "Play must persist a newly edited max stitched width without requiring Save Preset") &&
      expect(
          pending_max_width_calibration["status"].as<std::string>() == "pending" &&
              pending_max_width_calibration["stale_from"].as<std::string>() == "canvas" &&
              pending_max_width_calibration["artifacts_invalidated"].as<bool>() &&
              pending_max_width_calibration["invalidation_id"].IsScalar(),
          "Changing max stitched width from a legacy completed config must invalidate canvas artifacts before playback") &&
      expect(
          window->logText().count("stitching calibration clean command") == max_width_clean_commands + 1 &&
              window->logText().contains("stitching calibration dependency canvas is stale"),
          "Max stitched width changes must clean cached canvas-dependent artifacts");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS");
  if (!legacy_max_width_invalidated)
    return false;

  {
    YAML::Node malformed_width = YAML::LoadFile(stitch_time_transition_config.string());
    YAML::Node calibration = malformed_width["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points->value();
    calibration["status"] = "complete";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    calibration.remove("invalidation_id");
    malformed_width["stitching"]["max_output_width"] = "bad-width";
    const auto published = hm::stitching::publish_game_config(
        stitch_time_transition_config.parent_path(), YAML::Dump(malformed_width) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not prepare malformed max-width Play regression: " << published << '\n';
      return false;
    }
  }
  stitch_max_output_width->setValue(2048);
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS", "500");
  activate(start);
  const YAML::Node repaired_max_width = YAML::LoadFile(stitch_time_transition_config.string());
  const bool malformed_max_width_repaired =
      expect(
          repaired_max_width["stitching"]["max_output_width"].as<int>() == 2048,
          "Play must replace malformed existing max stitched width with the active UI value") &&
      expect(
          repaired_max_width["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "pending",
          "Play must invalidate calibration after replacing malformed max stitched width");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS");
  if (!malformed_max_width_repaired)
    return false;

  {
    YAML::Node conflicting_width = YAML::LoadFile(stitch_time_transition_config.string());
    YAML::Node calibration = conflicting_width["hstream_ui"]["stitching_calibration"];
    calibration["control_points"] = control_points->value();
    calibration["status"] = "complete";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    calibration.remove("invalidation_id");
    conflicting_width["stitching"]["max_output_width"] = 4096;
    conflicting_width["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = 2048;
    const auto published = hm::stitching::publish_game_config(
        stitch_time_transition_config.parent_path(), YAML::Dump(conflicting_width) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not prepare conflicting max-width Play regression: " << published << '\n';
      return false;
    }
  }
  stitch_max_output_width->setValue(4096);
  const int conflicting_alias_clean_commands = window->logText().count("stitching calibration clean command");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS", "500");
  activate(start);
  const YAML::Node repaired_conflict_width = YAML::LoadFile(stitch_time_transition_config.string());
  const YAML::Node repaired_conflict_calibration = repaired_conflict_width["hstream_ui"]["stitching_calibration"];
  const bool conflicting_max_width_normalized =
      expect(
          repaired_conflict_width["stitching"]["max_output_width"].as<int>() == 4096 &&
              !lookup_yaml_path(
                  repaired_conflict_width, {"pipeline", "hmstitcher", "properties", "max-output-width"}, nullptr),
          "Play must remove conflicting native max-width aliases while keeping the canonical value") &&
      expect(
          repaired_conflict_calibration["status"].as<std::string>() == "complete" &&
              !repaired_conflict_calibration["stale_from"].IsDefined() &&
              window->logText().count("stitching calibration clean command") == conflicting_alias_clean_commands,
          "Play must normalize a lower-precedence max-width alias without invalidating the effective canvas");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS");
  if (!conflicting_max_width_normalized)
    return false;

  stitch_max_output_width->setValue(0);
  QApplication::processEvents();

  mode->setCurrentIndex(mode->findData("program"));
  if (!expect(
          control_points->isEnabled(),
          "Program mode must allow changing calibration control points before the full pipeline starts")) {
    return false;
  }
  log->append("clear-log-test-marker");
  activate(clear_log);
  if (!expect(log->toPlainText().isEmpty(), "Clear Log must remove the visible runtime log output")) {
    return false;
  }
  const int fresh_program_clean_commands = window->logText().count("stitching calibration clean command");
  qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
  qputenv("HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW", "1");
  qputenv("HSTREAM_UI_TEST_PREVIEW_TIMEOUT_MS", "20");
  qputenv("HSTREAM_UI_TEST_PREVIEW_READY_AFTER", "1");
  qputenv("HSTREAM_UI_TEST_STARTUP_DELAY_MS", "150");
  for (QWidget* surface : {preview_surface, stitched_surface, camera1_surface, camera2_surface, camera3_surface}) {
    surface->setProperty("previewRendererState", "ready");
  }
  activate(start);
  for (int i = 0; i < 100 && !playback_progress->format().contains("validating saved stitching artifacts"); ++i) {
    QApplication::processEvents();
    QTest::qWait(5);
  }
  qunsetenv("HSTREAM_UI_TEST_STARTUP_DELAY_MS");
  if (!expect(
          playback_progress->isVisible() && playback_progress->minimum() == 0 && playback_progress->maximum() == 0 &&
              playback_progress->format().contains("STARTING") &&
              playback_progress->format().contains("validating saved stitching artifacts") &&
              playback_progress->toolTip().contains("Stage: stitching") &&
              window->logText().contains(
                  "startup [stitching]: Discovering source chapters and validating saved stitching artifacts") &&
              !window->logText().contains("HSTREAM_STARTUP"),
          "A configured or calibrating run must explain each pre-first-frame startup stage without protocol noise")) {
    return false;
  }
  if (!expect(
          !stitch_max_output_width->isEnabled(),
          "Max stitched width must be locked while the running pipeline uses the previously captured value")) {
    return false;
  }
  const QString initial_program_preview_state = preview_surface->property("previewRendererState").toString();
  if (!expect(
          (initial_program_preview_state == "idle" || initial_program_preview_state == "activating") &&
              stitched_surface->property("previewRendererState").toString() == "idle" &&
              camera1_surface->property("previewRendererState").toString() == "idle" &&
              camera2_surface->property("previewRendererState").toString() == "idle" &&
              camera3_surface->property("previewRendererState").toString() == "idle",
          "Starting a new pipeline should reset every GPU preview before the backend-ready handshake activates "
          "Program")) {
    return false;
  }
  for (int i = 0; i < 200 &&
       (!window->logText().contains("one-pass stitching calibration complete; continuous program playback running") ||
        window->pipelineStateText() != "PLAYING");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 200 && !window->logText().contains("GPU preview ready channel=program generation="); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          playback_progress->isVisible() && playback_progress->minimum() == 0 && playback_progress->maximum() == 1000 &&
              playback_progress->value() == 70 && playback_progress->format().contains("00:00:42 / 00:10:00") &&
              playback_progress->format().contains("42.75 FPS") &&
              playback_progress->format().contains("ETA 00:04:39") &&
              playback_progress->toolTip().contains("Remaining: 00:09:18") &&
              playback_progress->toolTip().contains("ETA: 00:04:39") &&
              playback_progress->toolTip().contains("Output FPS: 42.75 (average 40.50)") &&
              playback_progress->toolTip().contains("Processing speed: 2.00x") &&
              playback_progress->toolTip().contains("Stage: 0") &&
              playback_progress->toolTip().contains("Active pipelines: 1") &&
              !window->logText().contains("HSTREAM_PROGRESS"),
          "An active run should show exact backend playback progress without adding protocol noise to the log")) {
    return false;
  }
  if (!expect(
          seek_slider->isEnabled() && seek_back->isEnabled() && seek_forward->isEnabled() &&
              seek_position->text() == "00:00:42 / 00:10:00",
          "Local-render-only Program playback should expose position seeking after duration is known")) {
    return false;
  }
  pipeline_process->write("@test-set-backend-position 47000000000\n");
  for (int i = 0; i < 100 && !window->logText().contains("test backend position set to 47000000000"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-stall-seek\n");
  for (int i = 0; i < 100 && !window->logText().contains("test seek acknowledgement stalled"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  activate(seek_forward);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@seek-relative 10000000000 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !pause->isEnabled() && !seek_slider->isEnabled() && !seek_back->isEnabled() && !seek_forward->isEnabled() &&
              !window->logText().contains("playback seek complete at 00:00:57"),
          "A pending asynchronous seek should disable Pause and every transport control until completion")) {
    return false;
  }
  pipeline_process->write("@test-complete-seek\n");
  for (int i = 0; i < 100 && !window->logText().contains("playback seek complete at 00:00:57"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("stdin:@seek-relative 10000000000 1") &&
              window->logText().contains("playback seek complete at 00:00:57") && pause->isEnabled(),
          "The +10s control should use the backend's fresh 47-second position, complete at 57 seconds, and then "
          "restore Pause")) {
    return false;
  }
  const int pipeline_running_count_before_seek_recovery = window->logText().count("Pipeline running");
  pipeline_process->write("@test-timeout-seek\n");
  for (int i = 0; i < 100 && !window->logText().contains("test seek reconstruction timeout armed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  activate(seek_forward);
  for (int i = 0; i < 100 && !window->logText().contains("playback seek failed: pipeline-recreate-timeout"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !pause->isEnabled() && !seek_slider->isEnabled() && !seek_back->isEnabled() && !seek_forward->isEnabled() &&
              !program_control_tabs->isEnabled() && !stitched_control_tabs->isEnabled(),
          "A reconstruction timeout must keep transport and live tuning disabled until AppCtx recovery")) {
    return false;
  }
  for (int i = 0; i < 100 && window->logText().count("Pipeline running") <= pipeline_running_count_before_seek_recovery;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("Pipeline running") > pipeline_running_count_before_seek_recovery &&
              !pause->isEnabled() && !seek_slider->isEnabled() && !program_control_tabs->isEnabled() &&
              !window->logText().contains("playback recovered after a timed-out seek reconstruction"),
          "A PLAYING transition alone must not clear recovery before replacement media is processed")) {
    return false;
  }
  for (int i = 0; i < 100 && !window->logText().contains("playback recovered after a timed-out seek reconstruction");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          pause->isEnabled() && seek_slider->isEnabled() && seek_back->isEnabled() && seek_forward->isEnabled() &&
              program_control_tabs->isEnabled() && stitched_control_tabs->isEnabled(),
          "The explicit reconstruction recovery event must safely restore transport and live tuning")) {
    return false;
  }
  seek_slider->setLayoutDirection(Qt::LeftToRight);
  seek_slider->setInvertedAppearance(false);
  const SliderStyleGeometry default_seek_geometry = slider_style_geometry(seek_slider);
  if (!expect(default_seek_geometry.available_span > 4, "The seek slider must expose an interior handle travel span"))
    return false;
  seek_slider->setRange(0, default_seek_geometry.available_span);
  const int known_interior_seek_value = default_seek_geometry.available_span / 3;
  seek_slider->setValue(known_interior_seek_value);
  const QPoint known_interior_handle_center = slider_style_geometry(seek_slider).handle_center;
  const int interior_seek_commands_before = window->logText().count("stdin:@seek ");
  QTest::mousePress(
      seek_slider, Qt::LeftButton, Qt::NoModifier, QPoint(seek_slider->width() - 1, seek_slider->height() / 2), 0);
  QTest::mouseMove(seek_slider, QPoint(seek_slider->width() * 4 / 5, seek_slider->height() / 2), 0);
  const bool interior_release_differs_from_last_motion = seek_slider->value() != known_interior_seek_value;
  const qint64 known_interior_seek_target_ns = static_cast<qint64>(
      static_cast<long double>(known_interior_seek_value) * 600'000'000'000.0L /
      static_cast<long double>(default_seek_geometry.available_span));
  QTest::mouseRelease(seek_slider, Qt::LeftButton, Qt::NoModifier, known_interior_handle_center, 0);
  const QString known_interior_seek_command = QString("stdin:@seek %1 ").arg(known_interior_seek_target_ns);
  for (int i = 0; i < 100 &&
       (window->logText().count("stdin:@seek ") == interior_seek_commands_before ||
        !window->logText().contains(known_interior_seek_command) || !seek_slider->isEnabled());
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          seek_slider->rect().contains(known_interior_handle_center) && interior_release_differs_from_last_motion &&
              window->logText().count("stdin:@seek ") == interior_seek_commands_before + 1 &&
              window->logText().contains(known_interior_seek_command),
          "Releasing at a known styled interior handle center must override the last motion and issue exactly one "
          "seek to that known value")) {
    return false;
  }
  seek_slider->setRange(0, 100000);
  HStreamWindowTestAccess::refreshPlaybackSeekControls(window);
  const int absolute_seek_commands_before_drag = window->logText().count("stdin:@seek ");
  const QPoint seek_drag_start(seek_slider->width() / 5, seek_slider->height() / 2);
  const QPoint seek_drag_finish(seek_slider->width() * 3 / 4, seek_slider->height() / 2);
  QTest::mousePress(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_start, 0);
  bool drag_reached_left_endpoint = false;
  bool drag_reached_right_endpoint = false;
  bool drag_preview_reached_left_endpoint = false;
  bool drag_preview_reached_right_endpoint = false;
  for (int index = 0; index < 500; ++index) {
    const int x = 1 + (index * std::max(1, seek_slider->width() - 2) / 499);
    QTest::mouseMove(seek_slider, QPoint(x, seek_slider->height() / 2), 0);
    if (index == 0) {
      drag_reached_left_endpoint = seek_slider->value() == seek_slider->minimum();
      drag_preview_reached_left_endpoint = seek_position->text() == "00:00:00 / 00:10:00";
    }
    if (index == 499) {
      drag_reached_right_endpoint = seek_slider->value() == seek_slider->maximum();
      drag_preview_reached_right_endpoint = seek_position->text() == "00:10:00 / 00:10:00";
    }
  }
  const bool drag_remained_local = window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag;
  QTest::mouseRelease(
      seek_slider, Qt::LeftButton, Qt::NoModifier, QPoint(seek_slider->width() + 20, seek_slider->height() / 2), 0);
  QApplication::processEvents();
  const qint64 displayed_playback_position_ns = HStreamWindowTestAccess::playbackPositionNs(window);
  const qint64 displayed_playback_duration_ns = HStreamWindowTestAccess::playbackDurationNs(window);
  const int expected_restored_seek_value = displayed_playback_duration_ns > 0
      ? static_cast<int>(std::llround(
            static_cast<long double>(displayed_playback_position_ns) * seek_slider->maximum() /
            static_cast<long double>(displayed_playback_duration_ns)))
      : seek_slider->minimum();
  const QString expected_restored_seek_text = displayed_playback_duration_ns > 0
      ? QString("%1 / %2").arg(
            format_test_video_time_ns(displayed_playback_position_ns),
            format_test_video_time_ns(displayed_playback_duration_ns))
      : "00:00:00 / --:--:--";
  const bool outside_release_cancelled = !seek_slider->isSliderDown() &&
      seek_slider->value() == expected_restored_seek_value && seek_position->text() == expected_restored_seek_text &&
      window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag;
  QTest::mousePress(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_start, 0);
  QTest::mouseMove(seek_slider, seek_drag_finish, 0);
  seek_slider->setEnabled(false);
  const bool disable_cancelled_drag = !seek_slider->isSliderDown();
  seek_slider->setEnabled(true);
  QTest::mouseRelease(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_finish, 0);
  QApplication::processEvents();
  const bool disabled_drag_sent_no_seek = seek_position->text() == expected_restored_seek_text &&
      window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag;
  QTest::mousePress(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_start, 0);
  QTest::mouseMove(seek_slider, seek_drag_finish, 0);
  QEvent ungrab_event(QEvent::UngrabMouse);
  QApplication::sendEvent(seek_slider, &ungrab_event);
  const bool ungrab_cancelled_drag = !seek_slider->isSliderDown();
  QTest::mouseRelease(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_finish, 0);
  QApplication::processEvents();
  const bool ungrabbed_drag_sent_no_seek = seek_position->text() == expected_restored_seek_text &&
      window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag;
  QTest::mousePress(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_drag_start, 0);
  QTest::mouseMove(seek_slider, seek_drag_finish, 0);
  const bool release_position_differs_from_last_motion = seek_slider->value() != seek_slider->maximum();
  const QPoint seek_release_position(seek_slider->width() - 1, seek_slider->height() / 2);
  constexpr qint64 released_seek_target_ns = 600'000'000'000LL;
  QTest::mouseRelease(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_release_position, 0);
  const QString released_seek_command = QString("stdin:@seek %1 ").arg(released_seek_target_ns);
  for (int i = 0; i < 100 &&
       (window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag ||
        !window->logText().contains(released_seek_command));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          drag_remained_local && drag_reached_left_endpoint && drag_reached_right_endpoint &&
              drag_preview_reached_left_endpoint && drag_preview_reached_right_endpoint && outside_release_cancelled &&
              disable_cancelled_drag && disabled_drag_sent_no_seek && ungrab_cancelled_drag &&
              ungrabbed_drag_sent_no_seek && release_position_differs_from_last_motion &&
              window->logText().count("stdin:@seek ") == absolute_seek_commands_before_drag + 1 &&
              window->logText().contains(released_seek_command),
          "Slider motion must remain local, release outside/disable/ungrab must cancel safely, and release over the "
          "right endpoint must override the last motion and issue exactly one seek to the video end")) {
    return false;
  }
  const fs::path fresh_program_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const YAML::Node fresh_program_saved = YAML::LoadFile(fresh_program_config.string());
  YAML::Node fresh_program_status;
  const bool has_fresh_program_status =
      lookup_yaml_path(fresh_program_saved, {"hstream_ui", "stitching_calibration", "status"}, &fresh_program_status);
  const bool
      fresh_program_tracked =
          expect(
              window->logText().count("stitching calibration clean command") == fresh_program_clean_commands,
              "A fresh Program run without artifacts should not issue a redundant clean command (before=" +
                  std::to_string(fresh_program_clean_commands) +
                  ", after=" + std::to_string(window->logText().count("stitching calibration clean command")) + ")") &&
      expect(has_fresh_program_status && fresh_program_status.IsScalar() &&
                 fresh_program_status.as<std::string>() == "complete",
             "A fresh Program one-pass calibration should persist completed state") &&
      expect(window->logText().contains("GPU preview backend ready channel=program generation=2"),
             "Program startup must acknowledge the selected GPU preview without a tab change") &&
      expect(preview_tabs->tabText(0) == QString::fromUtf8("Program (1920×1080)"),
             "Program must show its negotiated per-run frame resolution in the tab title") &&
      expect(window->logText().contains("GPU preview first-frame wait exceeded channel=program") &&
                 window->logText().contains("GPU preview requested channel=program generation=3 reason=recovery") &&
                 window->logText().contains("GPU preview ready channel=program generation="),
             "A delayed Program first frame must recover by reactivating the same tab") &&
      expect(!window->logText().contains("stdin:@set-preview-active none") && !preview_surface->isHidden() &&
                 !preview_target->isHidden(),
             "First-frame recovery must never deactivate or hide the selected Program preview");
  QTest::mouseDClick(preview_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(!top_bar->isVisible(), "The inspector transition regression must begin in focused Program video")) {
    return false;
  }
  const int preview_deactivations_before_inspector = window->logText().count("stdin:@set-preview-active none");
  const int program_activations_before_inspector = window->logText().count("stdin:@set-preview-active program");
  pipeline_process->write("@test-stall-preview-disable\n");
  for (int i = 0; i < 100 && !window->logText().contains("test preview disable stalled"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  preview_tabs->setCurrentIndex(pipeline_inspector_index);
  for (int i = 0; i < 100 &&
       window->logText().count("stdin:@set-preview-active none") < preview_deactivations_before_inspector + 4;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && preview_tabs->currentIndex() == pipeline_inspector_index &&
              window->logText().count("stdin:@set-preview-active none") >= preview_deactivations_before_inspector + 4 &&
              window->logText().count("GPU preview inspector idle acknowledgement delayed; retrying") >= 3 &&
              window->logText().count("stdin:@set-preview-active program") == program_activations_before_inspector,
          "A stalled Pipeline inspector transition must retry GPU quiescence without waking hidden Program video")) {
    return false;
  }
  const int preview_deactivations_before_stalled_inspector_render_off =
      window->logText().count("stdin:@set-preview-active none");
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0;
       i < 100 && !window->logText().contains("while Pipeline inspector is selected; continuing idle retries");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && preview_tabs->currentIndex() == pipeline_inspector_index &&
              window->logText().count("stdin:@set-preview-active none") >=
                  preview_deactivations_before_stalled_inspector_render_off + 4 &&
              window->logText().contains("while Pipeline inspector is selected; continuing idle retries") &&
              window->logText().count("stdin:@set-preview-active program") == program_activations_before_inspector,
          "Render-off recovery on a stalled Pipeline inspector must resume idle retries without waking Program")) {
    return false;
  }
  pipeline_process->write("@test-resume-preview-disable\n");
  for (int i = 0; i < 100 &&
       (!window->logText().contains("test preview disable resumed") ||
        !window->logText().contains("GPU preview idle for Pipeline inspector generation="));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("test preview disable resumed") &&
              window->logText().contains("GPU preview idle for Pipeline inspector generation=") &&
              window->logText().count("stdin:@set-preview-active program") == program_activations_before_inspector,
          "Pipeline inspector retries must settle in an acknowledged idle state once the backend resumes")) {
    return false;
  }
  const int preview_deactivations_before_runtime_recreation = window->logText().count("stdin:@set-preview-active none");
  pipeline_process->write("@test-preview-runtime-ready program 900\n");
  for (int i = 0; i < 100 &&
       window->logText().count("stdin:@set-preview-active none") < preview_deactivations_before_runtime_recreation + 1;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-preview-active none") >=
                  preview_deactivations_before_runtime_recreation + 1 &&
              window->logText().count("stdin:@set-preview-active program") == program_activations_before_inspector,
          "A recreated backend must be returned to inspector idle without transiently reactivating Program")) {
    return false;
  }
  const int preview_deactivations_before_inspector_render_cycle =
      window->logText().count("stdin:@set-preview-active none");
  const int program_activations_before_inspector_render_cycle =
      window->logText().count("stdin:@set-preview-active program");
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0; i < 100 &&
       window->logText().count("stdin:@set-preview-active none") <
           preview_deactivations_before_inspector_render_cycle + 1;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0; i < 100 &&
       window->logText().count("stdin:@set-preview-active none") <
           preview_deactivations_before_inspector_render_cycle + 2;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && preview_tabs->currentIndex() == pipeline_inspector_index &&
              window->logText().count("stdin:@set-preview-active program") ==
                  program_activations_before_inspector_render_cycle &&
              window->logText().count("stdin:@set-preview-active none") >=
                  preview_deactivations_before_inspector_render_cycle + 2 &&
              window->logText().contains("GPU preview idle for Pipeline inspector generation="),
          "Re-enabling Render on the Pipeline inspector must preserve its idle GPU state without waking a hidden "
          "Program branch")) {
    return false;
  }
  preview_tabs->setCurrentIndex(0);
  for (int i = 0;
       i < 100 && window->logText().count("stdin:@set-preview-active program") == program_activations_before_inspector;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-preview-active none") >= preview_deactivations_before_inspector + 4 &&
              window->logText().count("stdin:@set-preview-active program") ==
                  program_activations_before_inspector + 1 &&
              top_bar->isVisible() && !preview_surface->isHidden() && !preview_target->isHidden(),
          "The Pipeline inspector tab must quiesce hidden GPU rendering across backend recreation, and returning "
          "to Program must request a fresh recoverable preview generation")) {
    return false;
  }
  if (!expect_x11_widget_state(
          preview_target, true, "Playing Program target must remain aligned with its Qt video host")) {
    return false;
  }
  const int pipeline_start_count = window->logText().count("pipeline started pid=");
  const int ready_count_before_runtime_toggle = window->logText().count("GPU preview ready channel=program");
  const int disabled_count_before_runtime_toggle = window->logText().count("GPU preview disabled generation=");
  if (!expect(
          render_video->isEnabled() && setup_preview_splitter->sizes().at(0) == 0 && program_focus->isVisible() &&
              program_focus->isEnabled() && program_controls->isHidden() && program_controls_toggle->isVisible(),
          "A live embedded preview should enable focus and collapse setup and associated controls for more video "
          "space")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-video-layout.png"))
    return false;
  QTest::mouseClick(show_player_tracking, Qt::LeftButton);
  QTest::mouseClick(show_play_tracking, Qt::LeftButton);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("preview overlays players=1 play=1 rink=1 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 4 1 1 1") &&
              window->logText().contains("preview overlays players=1 play=1 rink=1 apply=live"),
          "Live overlay checkboxes must use the topology-independent backend command and confirm applied state")) {
    return false;
  }
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0;
       i < 100 && window->logText().count("GPU preview disabled generation=") <= disabled_count_before_runtime_toggle;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !render_video->isChecked() && render_video->isEnabled() && preview_target->isHidden() &&
              program_focus->isHidden() && !show_player_tracking->isEnabled() && !show_play_tracking->isEnabled() &&
              !show_rink_mask->isEnabled() && !seek_slider->isEnabled() && setup_preview_splitter->sizes().at(0) > 0 &&
              window->logText().contains("stdin:@set-preview-active none") &&
              window->logText().contains("stdin:@set-render-audio-muted 1") &&
              window->logText().count("pipeline started pid=") == pipeline_start_count,
          "Turning rendering off while playing must quiesce video and local monitor audio, restore the setup layout, "
          "and keep the same pipeline process")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-render-disabled.png"))
    return false;
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0; i < 100 &&
       (window->logText().count("GPU preview ready channel=program") <= ready_count_before_runtime_toggle ||
        window->logText().count("preview overlays players=1 play=1 rink=1 apply=live") < 2);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && !preview_target->isHidden() && program_focus->isVisible() &&
              program_focus->isEnabled() && show_player_tracking->isEnabled() && show_play_tracking->isEnabled() &&
              show_rink_mask->isEnabled() && setup_preview_splitter->sizes().at(0) == 0 &&
              window->logText().contains("reason=render-toggle") &&
              window->logText().contains("stdin:@set-render-audio-muted 0") &&
              window->logText().contains("stdin:@set-preview-overlays 5 1 1 1") &&
              window->logText().count("preview overlays players=1 play=1 rink=1 apply=live") >= 2 &&
              window->logText().count("pipeline started pid=") == pipeline_start_count,
          "Turning rendering back on must restore preview, audio, and checked overlays without restarting the "
          "pipeline")) {
    return false;
  }

  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && !window->logText().contains("test preview overlay rejection armed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("reason=injected-rejection"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 6 1 1 0") &&
              window->logText().contains(
                  "preview overlays players=1 play=1 rink=1 apply=failed reason=injected-rejection"),
          "A rejected overlay update must restore the last backend-confirmed checkbox state")) {
    return false;
  }

  qputenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", "40");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && !window->logText().contains("test preview overlay delay armed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString overlay_timeout_log =
      "preview overlays players=1 play=1 rink=1 apply=failed reason=acknowledgement-timeout";
  const int overlay_timeouts_before_delay = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts_before_delay; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          show_rink_mask->isChecked() && window->logText().contains("stdin:@set-preview-overlays 7 1 1 0"),
          "A timed-out overlay update must roll its checkbox back to the last confirmed state")) {
    qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
    return false;
  }
  const int confirmed_overlay_logs_before_late_ack =
      window->logText().count("preview overlays players=1 play=1 rink=1 apply=live");
  const int overlay_delay_arms_before_reconciliation = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("test preview overlay delay armed") == overlay_delay_arms_before_reconciliation;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       (!window->logText().contains(
            "preview overlay acknowledgement arrived after rollback generation=7; reconciling") ||
        !window->logText().contains("stdin:@set-preview-overlays 8 1 1 1") ||
        window->logText().count(overlay_timeout_log) < overlay_timeouts_before_delay + 2);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           "late preview overlay acknowledgement matches confirmed state generation=8; settled");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && show_rink_mask->isChecked() &&
              window->logText().contains(
                  "preview overlay acknowledgement arrived after rollback generation=7; reconciling") &&
              window->logText().contains("stdin:@set-preview-overlays 8 1 1 1") &&
              window->logText().contains(
                  "late preview overlay acknowledgement matches confirmed state generation=8; settled") &&
              window->logText().count("preview overlays players=1 play=1 rink=1 apply=live") ==
                  confirmed_overlay_logs_before_late_ack &&
              !window->logText().contains("stdin:@set-preview-overlays 9 ") &&
              !window->logText().contains("preview overlays players=1 play=1 rink=0 apply=live"),
          "Two successive late acknowledgements must restore the backend once without resurrecting the choice or "
          "creating an unbounded reconciliation loop")) {
    return false;
  }

  const int overlay_delay_arms_before_rejected_reconciliation =
      window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("test preview overlay delay armed") == overlay_delay_arms_before_rejected_reconciliation;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int overlay_timeouts_before_rejected_reconciliation = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0;
       i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts_before_rejected_reconciliation;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") < 2; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           "preview overlays players=1 play=1 rink=0 apply=backend-state reason=injected-rejection");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QApplication::processEvents();
  QTest::qWait(100);
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && !show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 9 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 10 1 1 1") &&
              window->logText().contains(
                  "preview overlays players=1 play=1 rink=0 apply=backend-state reason=injected-rejection") &&
              !window->logText().contains("stdin:@set-preview-overlays 11 "),
          "A rejected bounded reconciliation must expose the backend's actual stale overlay state without looping")) {
    return false;
  }

  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 11 1 1 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int overlay_delay_arms_before_late_rejection = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("test preview overlay delay armed") == overlay_delay_arms_before_late_rejection;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int overlay_timeouts_before_late_rejection = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts_before_late_rejection;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("test preview overlay delay armed") < overlay_delay_arms_before_late_rejection + 2;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") < 3; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int backend_state_logs_before_late_rejection =
      window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state");
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       (window->logText().count(overlay_timeout_log) < overlay_timeouts_before_late_rejection + 2 ||
        !window->logText().contains("stdin:@set-preview-overlays 13 1 1 1"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", "200");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("test preview overlay delay armed") < overlay_delay_arms_before_late_rejection + 3;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") < 4; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 14 1 1 0"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") < 5; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           "late failed preview overlay reconciliation generation=13; preserving backend state");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state") ==
           backend_state_logs_before_late_rejection;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QApplication::processEvents();
  QTest::qWait(100);
  qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && !show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 12 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 13 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 14 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 15 1 1 1") &&
              window->logText().contains(
                  "late failed preview overlay reconciliation generation=13; preserving backend state") &&
              window->logText().contains(
                  "preview overlays players=1 play=1 rink=0 apply=backend-state reason=injected-rejection") &&
              !window->logText().contains("stdin:@set-preview-overlays 16 "),
          "A superseding user request and its rejected restore must preserve the unresolved backend fallback without "
          "looping")) {
    return false;
  }

  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 16 1 1 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", "40");
  int overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  int overlay_timeouts = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  int overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       (!window->logText().contains("stdin:@set-preview-overlays 18 1 1 1") ||
        window->logText().count(overlay_timeout_log) < overlay_timeouts + 2);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_timeouts = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int backend_state_logs_before_unresolved_timeout =
      window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state");
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state") ==
           backend_state_logs_before_unresolved_timeout;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  QApplication::processEvents();
  QTest::qWait(100);
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && !show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 16 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 17 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 18 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 19 1 1 0") &&
              window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state") ==
                  backend_state_logs_before_unresolved_timeout + 1 &&
              !window->logText().contains("stdin:@set-preview-overlays 20 "),
          "A normal request timeout must preserve an older unresolved reconciliation until its late failure adopts "
          "the known backend state without another retry")) {
    qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
    return false;
  }

  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 20 1 1 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_timeouts = window->logText().count(overlay_timeout_log);
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && window->logText().count(overlay_timeout_log) == overlay_timeouts; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", "1000");
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 22 1 1 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 23 1 1 0"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           "late failed preview overlay reconciliation generation=22; preserving backend state");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_delay_arms = window->logText().count("test preview overlay delay armed");
  pipeline_process->write("@test-delay-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay delay armed") == overlay_delay_arms; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(show_rink_mask, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@set-preview-overlays 24 1 1 1"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  overlay_rejection_arms = window->logText().count("test preview overlay rejection armed");
  pipeline_process->write("@test-reject-preview-overlays\n");
  for (int i = 0; i < 100 && window->logText().count("test preview overlay rejection armed") == overlay_rejection_arms;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int backend_state_logs_before_repeated_supersession =
      window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state");
  for (int i = 0; i < 200 &&
       window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state") ==
           backend_state_logs_before_repeated_supersession;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  pipeline_process->write("@test-complete-preview-overlays\n");
  pipeline_process->write("@test-complete-preview-overlays\n");
  QApplication::processEvents();
  QTest::qWait(100);
  qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
  if (!expect(
          show_player_tracking->isChecked() && show_play_tracking->isChecked() && !show_rink_mask->isChecked() &&
              window->logText().contains("stdin:@set-preview-overlays 20 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 21 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 22 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 23 1 1 0") &&
              window->logText().contains("stdin:@set-preview-overlays 24 1 1 1") &&
              window->logText().contains("stdin:@set-preview-overlays 25 1 1 1") &&
              window->logText().contains(
                  "late failed preview overlay reconciliation generation=22; preserving backend state") &&
              window->logText().count("preview overlays players=1 play=1 rink=0 apply=backend-state") ==
                  backend_state_logs_before_repeated_supersession + 1 &&
              !window->logText().contains("stdin:@set-preview-overlays 26 "),
          "Repeated user supersessions must retain a known backend fallback until the newest failed request is "
          "reconciled without looping")) {
    return false;
  }

  const int disabled_count_before_paused_toggle = window->logText().count("GPU preview disabled generation=");
  activate(pause);
  if (!expect(
          window->pipelineStateText() == "PAUSED" && playback_progress->isVisible() &&
              playback_progress->toolTip().contains("Pipeline: PAUSED") &&
              playback_progress->toolTip().contains("ETA: Paused") &&
              playback_progress->toolTip().contains("Processing speed: Paused") &&
              !playback_progress->toolTip().contains("Processing speed: 2.00x"),
          "Pausing should retain progress without presenting stale ETA or speed"))
    return false;
  const int seek_commands_before_paused_drag = window->logText().count("stdin:@seek ");
  const QPoint paused_drag_motion_position(seek_slider->width() * 2 / 5, seek_slider->height() / 2);
  const QPoint paused_seek_position(0, seek_slider->height() / 2);
  QTest::mousePress(seek_slider, Qt::LeftButton, Qt::NoModifier, seek_slider->rect().center(), 0);
  QTest::mouseMove(seek_slider, paused_drag_motion_position, 0);
  constexpr qint64 paused_seek_target_ns = 0;
  QTest::mouseRelease(seek_slider, Qt::LeftButton, Qt::NoModifier, paused_seek_position, 0);
  QApplication::processEvents();
  if (!expect(
          seek_slider->isEnabled() && seek_back->isEnabled() && seek_forward->isEnabled() &&
              seek_slider->value() == seek_slider->minimum() && seek_position->text() == "00:00:00 / 00:10:00" &&
              window->logText().count("stdin:@seek ") == seek_commands_before_paused_drag &&
              window->logText().contains("playback seek queued for resume") &&
              seek_slider->toolTip().contains("will be applied when playback resumes"),
          "Paused playback must accept and display a slider target without sending backend work")) {
    return false;
  }
  const int queued_seek_logs_before_relative = window->logText().count("playback seek queued for resume");
  activate(seek_forward);
  QApplication::processEvents();
  const qint64 replaced_paused_seek_target_ns =
      std::min<qint64>(600'000'000'000LL, paused_seek_target_ns + 10'000'000'000LL);
  if (!expect(
          window->logText().count("stdin:@seek ") == seek_commands_before_paused_drag &&
              window->logText().count("playback seek queued for resume") == queued_seek_logs_before_relative + 1,
          "A paused +10s request must replace the deferred slider target without contacting the backend")) {
    return false;
  }
  pipeline_process->write("@test-stall-seek\n");
  const quint64 progress_generation_before_resumed_seek = HStreamWindowTestAccess::playbackResetGeneration(window);
  const int progress_reset_commands_before_resumed_seek = window->logText().count("stdin:@reset-progress-rate");
  activate(pause);
  const QString resumed_seek_command = QString("stdin:@seek %1 ").arg(replaced_paused_seek_target_ns);
  for (int i = 0; i < 100 &&
       (!window->logText().contains("test seek acknowledgement stalled") ||
        window->logText().count("stdin:@seek ") == seek_commands_before_paused_drag ||
        !window->logText().contains(resumed_seek_command));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool accepted_resumed_progress = HStreamWindowTestAccess::handlePlaybackProgressOutput(
      window,
      QString(
          "HSTREAM_PROGRESS processed_ns=43000000000 total_ns=600000000000 remaining_ns=557000000000 "
          "eta_ns=1114000000000 speed_x=0.500000 fraction=0.071667 stage=0 instance=aggregate instances=2 "
          "generation=%1")
          .arg(progress_generation_before_resumed_seek));
  QApplication::processEvents();
  if (!expect(
          accepted_resumed_progress && !pause->isEnabled() &&
              window->logText().count("stdin:@reset-progress-rate") == progress_reset_commands_before_resumed_seek &&
              playback_progress->toolTip().contains("Pipeline: PLAYING") &&
              playback_progress->toolTip().contains("ETA: Warming up") &&
              playback_progress->toolTip().contains("Processing speed: Warming up") &&
              !playback_progress->toolTip().contains("ETA: 00:18:34") &&
              !playback_progress->toolTip().contains("Processing speed: 0.50x"),
          "Progress arriving between Resume and its deferred seek acknowledgement must remain warming and must not "
          "expose a cross-pause rate")) {
    return false;
  }
  pipeline_process->write("@test-complete-seek\n");
  for (int i = 0; i < 100 &&
       (!window->logText().contains("playback seek complete at 00:00:10") || !pause->isEnabled() ||
        window->logText().count("stdin:@reset-progress-rate") <= progress_reset_commands_before_resumed_seek);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("playback seek complete at 00:00:10") && pause->isEnabled() &&
              window->logText().count("stdin:@reset-progress-rate") == progress_reset_commands_before_resumed_seek + 1,
          "Completing a stalled deferred seek must issue exactly one progress-rate reset and restore transport "
          "controls")) {
    return false;
  }

  activate(pause);
  const qint64 rejected_paused_seek_base_ns = HStreamWindowTestAccess::playbackPositionNs(window);
  activate(seek_forward);
  QApplication::processEvents();
  const qint64 rejected_paused_seek_target_ns =
      std::min<qint64>(600'000'000'000LL, rejected_paused_seek_base_ns + 10'000'000'000LL);
  const int seek_commands_before_rejected_resume = window->logText().count("stdin:@seek ");
  pipeline_process->write("@test-reject-seek\n");
  const int progress_reset_commands_before_rejected_resume = window->logText().count("stdin:@reset-progress-rate");
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0;
       i < 20 && preview_status->text() != "GPU preview will finish disabling when the paused pipeline resumes";
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool paused_render_off_ok = !render_video->isChecked() && preview_target->isHidden() &&
      program_focus->isHidden() && setup_preview_splitter->sizes().at(0) > 0 &&
      window->logText().count("GPU preview disabled generation=") == disabled_count_before_paused_toggle &&
      preview_status->text() == "GPU preview will finish disabling when the paused pipeline resumes";
  if (!expect(
          paused_render_off_ok,
          "Render-off while paused must immediately unmap native targets and defer its acknowledgement safely")) {
    return false;
  }
  activate(pause);
  const QString rejected_resumed_seek_command = QString("stdin:@seek %1 ").arg(rejected_paused_seek_target_ns);
  for (int i = 0; i < 100 &&
       (window->logText().count("stdin:@seek ") == seek_commands_before_rejected_resume ||
        !window->logText().contains(rejected_resumed_seek_command) ||
        !window->logText().contains("playback seek rejected: nonlocal-output-active") ||
        window->logText().count("stdin:@reset-progress-rate") <= progress_reset_commands_before_rejected_resume);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@seek ") == seek_commands_before_rejected_resume + 1 &&
              window->logText().contains(rejected_resumed_seek_command) &&
              window->logText().contains("playback seek rejected: nonlocal-output-active") &&
              window->logText().count("stdin:@reset-progress-rate") ==
                  progress_reset_commands_before_rejected_resume + 1 &&
              playback_progress->toolTip().contains("Pipeline: PLAYING") &&
              playback_progress->toolTip().contains("ETA: Warming up") &&
              playback_progress->toolTip().contains("Processing speed: Warming up") &&
              !playback_progress->toolTip().contains("Processing speed: 0.50x"),
          "Resuming must issue only the latest deferred seek and reset backend rate sampling even when it is "
          "rejected")) {
    return false;
  }
  for (int i = 0;
       i < 100 && window->logText().count("GPU preview disabled generation=") <= disabled_count_before_paused_toggle;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->pipelineStateText() == "PLAYING" && !render_video->isChecked() && preview_target->isHidden(),
          "Resuming must allow the pending render-off request to quiesce the backend")) {
    return false;
  }
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0; i < 100 &&
       (preview_target->isHidden() || preview_target->property("previewRendererState").toString() != "ready");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && !preview_target->isHidden() && setup_preview_splitter->sizes().at(0) == 0,
          "Rendering must reactivate normally after a paused render-off request completes")) {
    return false;
  }

  pipeline_process->write("@test-stall-progress-reset\n");
  for (int i = 0; i < 100 && !window->logText().contains("test progress reset stalled"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int reset_commands_before_race = window->logText().count("stdin:@reset-progress-rate");
  activate(pause);
  activate(pause);
  for (int i = 0; i < 100 && window->logText().count("stdin:@reset-progress-rate") < reset_commands_before_race + 1;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  activate(pause);
  activate(pause);
  for (int i = 0; i < 100 && window->logText().count("stdin:@reset-progress-rate") < reset_commands_before_race + 2;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->pipelineStateText() == "PLAYING" && window->logText().contains("stdin:@reset-progress-rate 2") &&
              window->logText().contains("stdin:@reset-progress-rate 3") &&
              playback_progress->toolTip().contains("ETA: Warming up") &&
              playback_progress->toolTip().contains("Processing speed: Warming up") &&
              !playback_progress->toolTip().contains("Processing speed: 0.50x"),
          "A stale reset acknowledgement must not expose contaminated progress during rapid pause/resume")) {
    return false;
  }

  qputenv("HSTREAM_UI_TEST_PROGRESS_RESET_TIMEOUT_MS", "10");
  pipeline_process->write("@test-drop-progress-resets\n");
  for (int i = 0; i < 100 && !window->logText().contains("test progress resets dropped"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int reset_commands_before_timeout = window->logText().count("stdin:@reset-progress-rate");
  activate(pause);
  activate(pause);
  for (int i = 0; i < 100 &&
       (!window->logText().contains(
            "playback speed reset was not acknowledged; using recovered adjacent-sample rate") ||
        window->logText().count("stdin:@reset-progress-rate") < reset_commands_before_timeout + 3);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool reset_fallback_observed =
      window->logText().contains("playback speed reset was not acknowledged; using recovered adjacent-sample rate") &&
      window->logText().count("stdin:@reset-progress-rate") >= reset_commands_before_timeout + 3;
  pipeline_process->write("@test-resume-progress-resets\n");
  qunsetenv("HSTREAM_UI_TEST_PROGRESS_RESET_TIMEOUT_MS");
  if (!expect(
          reset_fallback_observed && window->pipelineStateText() == "PLAYING",
          "A dropped reset acknowledgement should retry finitely and fall back without wedging playback")) {
    return false;
  }

  pipeline_process->write("@test-stall-preview-disable\n");
  for (int i = 0; i < 100 && !window->logText().contains("test preview disable stalled"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseClick(render_video, Qt::LeftButton);
  preview_tabs->setCurrentIndex(1);
  for (int i = 0; i < 100 && !window->logText().contains("GPU preview disable failed (the backend did not acknowledge");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 100 &&
       (stitched_target->isHidden() || stitched_target->property("previewRendererState").toString() != "ready");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && preview_tabs->currentIndex() == 1 && preview_target->isHidden() &&
              !stitched_target->isHidden() && setup_preview_splitter->sizes().at(0) == 0 &&
              window->logText().count("GPU preview disable acknowledgement delayed; retrying") >= 3 &&
              window->logText().contains("restoring rendering"),
          "A missing render-off acknowledgement must restore the visible tab and reconcile its backend channel")) {
    return false;
  }
  pipeline_process->write("@test-resume-preview-disable\n");
  for (int i = 0; i < 100 && !window->logText().contains("test preview disable resumed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  preview_tabs->setCurrentIndex(0);
  for (int i = 0; i < 100 &&
       (preview_target->isHidden() || preview_target->property("previewRendererState").toString() != "ready");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  const QSize program_normal_host_size = program_host->size();
  QTest::mouseDClick(preview_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          !top_bar->isVisible() && !setup_row->isVisible() && !log_panel->isVisible() &&
              !preview_tabs->tabBar()->isVisible() && !program_controls->isVisible() && program_host->isVisible() &&
              !preview_status->isVisible() && playback_progress->isVisible() && !window->isFullScreen() &&
              program_focus->toolTip().contains("Restore the normal HStream layout") &&
              program_focus->accessibleName() == "Restore HStream controls",
          "A real double-click on a ready GPU preview should focus it across the HStream app area")) {
    return false;
  }
  const QPoint focused_button_center_before_resize = program_focus->mapTo(window, program_focus->rect().center());
  window->resize(1500, 920);
  QApplication::processEvents();
  if (!expect_x11_widget_state(
          preview_target, true, "A focused playing target must preserve its native parent and geometry after resize") ||
      !expect_x11_widget_state(
          program_focus, true, "A focused resize must remap exactly one native restore control at its new geometry") ||
      !expect(
          program_host->width() >= program_normal_host_size.width() &&
              program_host->height() >= program_normal_host_size.height() &&
              std::abs(preview_target->width() * 9 - preview_target->height() * 16) <= 16 &&
              program_focus->x() == preview_target->width() - program_focus->width() - 6 && program_focus->y() == 6,
          "Focused Program video must grow at 16:9 with its restore control pinned to the top-right")) {
    return false;
  }
  if (!expect_composed_focus_control(
          window,
          program_focus,
          focused_button_center_before_resize,
          "A focused resize must clear the old native maximize glyph and paint exactly one at the new position")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-focused-resized.png"))
    return false;

  const int ready_count_before_focused_disable = window->logText().count("GPU preview ready channel=program");
  render_video->setChecked(false);
  for (int i = 0; i < 100 && !preview_target->isHidden(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !render_video->isChecked() && top_bar->isVisible() && setup_row->isVisible() && log_panel->isVisible() &&
              preview_tabs->tabBar()->isVisible() && preview_target->isHidden() && program_focus->isHidden() &&
              setup_preview_splitter->sizes().at(0) > 0,
          "Disabling rendering while focused must first restore the complete UI and then unmap the native target")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-focused-render-disabled.png"))
    return false;

  render_video->setChecked(true);
  for (int i = 0;
       i < 100 && window->logText().count("GPU preview ready channel=program") <= ready_count_before_focused_disable;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QTest::mouseDClick(preview_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          render_video->isChecked() && !top_bar->isVisible() && !preview_target->isHidden() &&
              program_focus->isVisible(),
          "Re-enabling rendering after a focused disable must allow the ready preview to enter focus again")) {
    return false;
  }
  const quint64 focused_preview_generation = preview_target->property("previewRendererGeneration").toULongLong();
  pipeline_process->write(
      QString("@test-preview-status source2 unavailable %1\n").arg(focused_preview_generation).toUtf8());
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           QString("GPU preview unavailable channel=source2 generation=%1").arg(focused_preview_generation));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !top_bar->isVisible() && !preview_target->isHidden() && program_focus->isVisible(),
          "An unavailable inactive camera must not exit a healthy focused Program preview")) {
    return false;
  }
  QTest::mouseClick(program_focus, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          top_bar->isVisible() && setup_row->isVisible() && log_panel->isVisible() &&
              preview_tabs->tabBar()->isVisible() && program_controls->isHidden() &&
              program_controls_toggle->isVisible() && program_focus->isVisible(),
          "A real click on the high-contrast restore control should restore the normal UI")) {
    return false;
  }
  if (!expect_x11_widget_state(
          program_focus, true, "Restoring the normal UI must keep its native focus control mapped and stacked")) {
    return false;
  }
  if (!expect_composed_focus_control(
          window, program_focus, std::nullopt, "Restoring the normal UI must leave a visible maximize glyph")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-restored.png"))
    return false;

  pipeline_process->write(QString("@test-preview-status program failed %1\n").arg(focused_preview_generation).toUtf8());
  for (int i = 0; i < 100 &&
       !window->logText().contains(
           QString("GPU preview failed channel=program generation=%1").arg(focused_preview_generation));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          top_bar->isVisible() && preview_tabs->tabBar()->isVisible() && preview_target->isHidden() &&
              setup_preview_splitter->sizes().at(0) > 0 && program_controls->isVisible(),
          "Failure of the active GPU preview must restore the normal setup and associated-control layout")) {
    return false;
  }
  window->resize(1440, 900);
  QApplication::processEvents();
  struct FocusPreviewCase {
    int tab;
    const char* channel;
    QWidget* host;
    QWidget* target;
    QPushButton* button;
  };
  const std::array<FocusPreviewCase, 4> additional_focus_cases = {{
      {1, "stitched", stitched_host, stitched_target, stitched_focus},
      {2, "source0", camera1_host, camera1_target, camera1_focus},
      {3, "source1", camera2_host, camera2_target, camera2_focus},
      {4, "source2", camera3_host, camera3_target, camera3_focus},
  }};
  QWidget* previous_target = preview_target;
  for (const FocusPreviewCase& focus_case : additional_focus_cases) {
    preview_tabs->setCurrentIndex(focus_case.tab);
    const QString ready_marker = QString("GPU preview ready channel=%1 generation=").arg(focus_case.channel);
    for (int i = 0; i < 100 &&
         (focus_case.target->isHidden() || focus_case.button->isHidden() ||
          focus_case.target->property("previewRendererState").toString() != "ready" ||
          !window->logText().contains(ready_marker));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const QSize normal_host_size = focus_case.host->size();
    if (!expect_x11_widget_state(
            previous_target, false, "The previously selected preview target must be unmapped", false) ||
        !expect_x11_widget_state(focus_case.target, true, "Each selected preview target must map inside its Qt host") ||
        !expect(
            focus_case.button->isVisible() && focus_case.button->isEnabled() &&
                focus_case.button->x() == focus_case.target->width() - focus_case.button->width() - 6 &&
                focus_case.button->y() == 6,
            "Every ready preview must expose an enabled top-right maximize control")) {
      return false;
    }
    QTest::mouseDClick(focus_case.target, Qt::LeftButton);
    QApplication::processEvents();
    const bool preview_footer_hidden = focus_case.tab != 1 || !stitched_status->isVisible();
    if (!expect(
            focus_case.host->isVisible() && !preview_tabs->tabBar()->isVisible() && !top_bar->isVisible() &&
                focus_case.button->isVisible() && focus_case.host->width() >= normal_host_size.width() &&
                focus_case.host->height() >= normal_host_size.height() &&
                preview_footer_hidden &&
                std::abs(focus_case.target->width() * 9 - focus_case.target->height() * 16) <= 16 &&
                focus_case.button->x() == focus_case.target->width() - focus_case.button->width() - 6 &&
                focus_case.button->y() == 6,
            "Every ready Stitched/camera preview must maximize at 16:9 without displacing its restore control")) {
      return false;
    }
    QTest::mouseClick(focus_case.button, Qt::LeftButton);
    QApplication::processEvents();
    if (!expect(
            preview_tabs->tabBar()->isVisible() && top_bar->isVisible() && focus_case.host->isVisible() &&
                focus_case.button->isVisible(),
            "Every preview maximize control must restore the complete normal layout")) {
      return false;
    }
    previous_target = focus_case.target;
  }
  if (!expect(
          preview_tabs->tabText(1) == QString::fromUtf8("Stitched (4096×1080)"),
          "The Stitched tab must show the negotiated canvas resolution when that output presents a frame")) {
    return false;
  }
  preview_tabs->setCurrentIndex(2);
  for (int i = 0; i < 100 && (camera1_target->isHidden() || camera1_focus->isHidden()); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!fresh_program_tracked)
    std::cerr << window->logText().toStdString() << '\n';
  QTest::mouseDClick(camera1_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          !top_bar->isVisible() && camera1_focus->isVisible(),
          "The stop-while-focused regression must begin on a healthy preview after Program failed")) {
    return false;
  }
  activate(stop);
  for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(playback_progress->isHidden(), "Stopping should hide playback progress until the next run")) {
    return false;
  }
  if (!expect(
          preview_surface->property("previewRendererState").toString() == "idle" &&
              stitched_surface->property("previewRendererState").toString() == "idle" &&
              camera1_surface->property("previewRendererState").toString() == "idle" &&
              camera2_surface->property("previewRendererState").toString() == "idle" &&
              camera3_surface->property("previewRendererState").toString() == "idle",
          "Finishing a pipeline should clear every GPU renderer state")) {
    return false;
  }
  if (!expect(
          preview_tabs->tabText(0) == "Program" && preview_tabs->tabText(1) == "Stitched",
          "Stopping Program must remove per-run frame resolutions from both tab titles")) {
    return false;
  }
  if (!expect(
          preview_target->isHidden() && camera1_target->isHidden(),
          "Finishing a pipeline must unmap native video targets so stopped-state UI controls cannot be obscured")) {
    return false;
  }
  if (!expect(
          top_bar->isVisible() && setup_row->isVisible() && log_panel->isVisible() &&
              preview_tabs->tabBar()->isVisible() && !program_controls->isVisible() && camera1_focus->isHidden() &&
              setup_preview_splitter->sizes().at(0) > 0,
          "Stopping while focused on Camera 1 must restore the normal camera-tab UI before unmapping native preview "
          "windows")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "stopped-after-focused-stop.png"))
    return false;
  qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
  qunsetenv("HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW");
  qunsetenv("HSTREAM_UI_TEST_PREVIEW_TIMEOUT_MS");
  qunsetenv("HSTREAM_UI_TEST_PREVIEW_READY_AFTER");
  if (!fresh_program_tracked) {
    return false;
  }

  const int completed_runs_before = window->logText().count("pipeline finished exit=0 status=normal");
  qputenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS", "0");
  activate(start);
  for (int i = 0; i < 200 && window->logText().count("pipeline finished exit=0 status=normal") == completed_runs_before;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS");
  if (!expect(
          window->pipelineStateText() == "STOPPED" && playback_progress->isVisible() &&
              playback_progress->minimum() == 0 && playback_progress->maximum() == 1000 &&
              playback_progress->value() == 1000 && playback_progress->format().contains("COMPLETED") &&
              playback_progress->format().contains("42.75 FPS") &&
              playback_progress->format().contains("ETA 00:00:00") &&
              playback_progress->property("playbackState").toString() == "completed" &&
              playback_progress->toolTip().contains("Pipeline: COMPLETED"),
          "A clean natural exit should leave a full, distinct COMPLETED progress result visible")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playback-completed.png")) {
    return false;
  }

  const int calibration_index = mode->findData("stitch-calibration");
  mode->setCurrentIndex(calibration_index);
  if (!expect(
          !preview_tabs->isTabEnabled(0) && preview_tabs->currentIndex() == 1,
          "Stitching Calibration mode should expose Stitched instead of the omitted Program graph")) {
    return false;
  }
  control_points->setValue(750);
  qputenv("HSTREAM_UI_TEST_CLEAN_RESULT", "failure");
  activate(start);
  qunsetenv("HSTREAM_UI_TEST_CLEAN_RESULT");
  const fs::path failed_clean_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const YAML::Node failed_clean_state = YAML::LoadFile(failed_clean_config.string());
  const YAML::Node failed_clean_calibration = failed_clean_state["hstream_ui"]["stitching_calibration"];
  if (!expect(
          window->pipelineStateText() == "STOPPED" &&
              failed_clean_calibration["status"].as<std::string>() == "pending" &&
              failed_clean_calibration["stale_from"].as<std::string>() == "features" &&
              !failed_clean_calibration["artifacts_invalidated"].as<bool>(),
          "Calibration must durably record its stale dependency before artifact cleanup starts")) {
    return false;
  }
  const int guarded_calibration_commands_before = window->logText().count("--clean-expected-invalidation-id=");
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 50 && !window->logText().contains("HM_MAX_CONTROL_POINTS=750"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0) {
    for (int i = 0; i < 100 && stitched_surface->isHidden(); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
  }
  const QString runtime_log = window->logText();
  const int latest_command_offset = runtime_log.lastIndexOf("pipeline command ");
  const QString latest_calibration_command =
      latest_command_offset >= 0 ? runtime_log.mid(latest_command_offset).section('\n', 0, 0) : QString();
  const QRegularExpressionMatch preview_windows_match =
      QRegularExpression(R"(--ui-preview-windows=([^\s]+))").match(latest_calibration_command);
  QStringList preview_channels;
  if (preview_windows_match.hasMatch()) {
    for (const QString& mapping : preview_windows_match.captured(1).split(','))
      preview_channels.push_back(mapping.section(':', 0, 0));
    std::sort(preview_channels.begin(), preview_channels.end());
  }
  QStringList expected_calibration_channels{"source0", "source1", "source2", "stitched"};
  std::sort(expected_calibration_channels.begin(), expected_calibration_channels.end());
  const bool x11_calibration_preview_ok = preview_channels == expected_calibration_channels &&
      !preview_channels.contains("program") && latest_calibration_command.contains("--ui-preview-active=stitched") &&
      !stitched_surface->isHidden() && camera1_surface->isHidden() && camera2_surface->isHidden() &&
      camera3_surface->isHidden();
  if (QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0 && !x11_calibration_preview_ok) {
    std::cerr << "calibration preview visibility stitched=" << !stitched_surface->isHidden()
              << " camera1=" << !camera1_surface->isHidden() << " camera2=" << !camera2_surface->isHidden()
              << " camera3=" << !camera3_surface->isHidden() << '\n';
    std::cerr << window->logText().right(4000).toStdString() << '\n';
  }
  if (!expect(
          window->logText().contains("ds_hockey_app_config.yaml"),
          "Calibration should use the one-pass application config") ||
      !expect(
          window->logText().contains("--clean-from-control-points"),
          "Changed calibration CP count should invalidate only control-point-dependent stitching artifacts") ||
      !expect(
          window->logText().count("--clean-expected-invalidation-id=") == guarded_calibration_commands_before + 4,
          "Pending calibration must guard both its cleanup and its non-forced main process with the invalidation ID") ||
      !expect(
          window->logText().contains("stitching calibration control points changed 1500 -> 750"),
          "Calibration CP change should be logged") ||
      !expect(
          window->logText().contains("--enable-sinks=RENDER"),
          "One-pass calibration should retain the logical render sink for local audio monitoring") ||
      !expect(
          window->logText().contains("--show") && !window->logText().contains("--show-stitching"),
          "Calibration should route the normal render sink without enabling stitcher debug windows") ||
      !expect(
          window->logText().contains("pipeline.streammux.batch-size=2") &&
              window->logText().contains("pipeline.streammux.sync-inputs=0") &&
              window->logText().contains("pipeline.streammux.batched-push-timeout=2147483647") &&
              window->logText().contains("pipeline.streammux.frame-num-reset-on-stream-reset=0") &&
              window->logText().contains("pipeline.streammux.frame-num-reset-on-eos=0") &&
              window->logText().contains("pipeline.hmstitcher.show=0") &&
              window->logText().contains("--stitching-calibration-only"),
          "Stitched preview should batch both cameras on the stitching-only pipeline without Program stages") ||
      !expect(
          window->logText().contains("HM_MAX_CONTROL_POINTS=750"),
          "One-pass calibration should pass the selected control-point limit") ||
      !expect(
          QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0
              ? x11_calibration_preview_ok
              : !window->logText().contains("--render-window-id=") &&
                  window->logText().contains("HM_RENDER_SINK=nv3dsink") &&
                  window->logText().contains("--show-scaled=0.3012048193") &&
                  !window->logText().contains("--source-render-window-ids=") && stitched_surface->isHidden() &&
                  camera1_surface->isHidden() && camera2_surface->isHidden() && camera3_surface->isHidden(),
          "Only the X11 test backend should expose the selected embedded stitched preview window") ||
      !expect(
          window->logText().contains("ANSI blue runner line"), "ANSI-colored runner output should remain visible") ||
      !expect(
          !window->logText().contains(QChar(0x1b)), "ANSI control characters should not appear in plain log text") ||
      !expect(log->toHtml().contains("#81a1c1"), "ANSI foreground color should render as rich log text") ||
      !expect(window->pipelineStateText() == "PLAYING", "Test runner should keep calibration process running")) {
    return false;
  }
  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    const YAML::Node saved = YAML::LoadFile(config.string());
    YAML::Node saved_control_points;
    const bool has_saved_control_points =
        lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "control_points"}, &saved_control_points);
    YAML::Node saved_status;
    const bool has_saved_status =
        lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "status"}, &saved_status);
    YAML::Node saved_stale_from;
    const bool has_saved_stale_from =
        lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "stale_from"}, &saved_stale_from);
    YAML::Node saved_artifacts_invalidated;
    const bool has_saved_artifacts_invalidated = lookup_yaml_path(
        saved, {"hstream_ui", "stitching_calibration", "artifacts_invalidated"}, &saved_artifacts_invalidated);
    YAML::Node saved_invalidation_id;
    const bool has_saved_invalidation_id =
        lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "invalidation_id"}, &saved_invalidation_id);
    YAML::Node saved_run_autooptimizer;
    const bool has_saved_run_autooptimizer =
        lookup_yaml_path(saved, {"stitching", "run_autooptimizer"}, &saved_run_autooptimizer);
    if (!expect(
            has_saved_control_points && saved_control_points.IsScalar() && saved_control_points.as<int>() == 750,
            "Calibration CP count should be saved to private config") ||
        !expect(
            has_saved_status && saved_status.IsScalar() && saved_status.as<std::string>() == "pending",
            "Calibration CP state should remain pending while the calibration process is running") ||
        !expect(
            has_saved_run_autooptimizer && saved_run_autooptimizer.IsScalar() && !saved_run_autooptimizer.as<bool>(),
            "Stitching calibration should persist the disabled-by-default panorama optimizer setting") ||
        !expect(
            has_saved_stale_from && saved_stale_from.as<std::string>() == "features" &&
                has_saved_artifacts_invalidated && saved_artifacts_invalidated.as<bool>(),
            "Calibration state should persist the applied control-point dependency boundary") ||
        !expect(
            has_saved_invalidation_id && saved_invalidation_id.IsScalar() &&
                window->logText().contains(QString("HSTREAM_CALIBRATION_INVALIDATION_ID=%1")
                                               .arg(QString::fromStdString(saved_invalidation_id.as<std::string>()))),
            "The one-pass runtime must receive the pending calibration invalidation ID")) {
      return false;
    }
  }

  rotate->setValue(74);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=74 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("stdin:@set-property hmstitcher0 stitched-output-epoch="),
          "Live stitch rotation should be sent to the running pipeline over stdin") ||
      !expect(
          window->logText().contains("camera control Stitch_Rotate_Degrees=74 apply=pending") &&
              window->logText().contains("camera control Stitch_Rotate_Degrees=74 apply=live"),
          "Live stitch rotation should only report success after the pipeline acknowledges it")) {
    return false;
  }

  activate(pause);
  if (!expect(window->pipelineStateText() == "PAUSED", "Pause button should pause the process")) {
    return false;
  }
  activate(pause);
  if (!expect(window->pipelineStateText() == "PLAYING", "Pause button should resume the process")) {
    return false;
  }
  activate(stop);
  for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->pipelineStateText() == "STOPPED" && preview_tabs->tabText(1) == "Stitched",
          "Stop should terminate calibration and remove its per-run frame resolution from the tab title")) {
    return false;
  }
  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    const YAML::Node saved = YAML::LoadFile(config.string());
    YAML::Node saved_status;
    const bool has_saved_status =
        lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "status"}, &saved_status);
    if (!expect(
            has_saved_status && saved_status.IsScalar() && saved_status.as<std::string>() == "pending",
            "User-stopped calibration should remain pending so the next run can resume")) {
      return false;
    }
  }
  const int resume_clean_commands = window->logText().count("stitching calibration clean command");
  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    const QString launched_game_id = game_id->text();
    const QString switched_game_id = "ui-switched-during-calibration";
    const fs::path switched_config =
        fs::path(qgetenv("HM_GAME_DIR").toStdString()) / switched_game_id.toStdString() / "config.yaml";
    const fs::path active_runtime_dir = config.parent_path() / ".hstream-ui";
    const fs::path switched_runtime_dir = switched_config.parent_path() / ".hstream-ui";
    auto runtime_snapshot_count = [](const fs::path& dir) {
      if (!fs::exists(dir))
        return size_t{0};
      return static_cast<size_t>(
          std::count_if(fs::directory_iterator(dir), fs::directory_iterator(), [](const fs::directory_entry& entry) {
            return entry.path().filename().string().rfind("play_tracker_runtime_", 0) == 0;
          }));
    };
    const size_t active_runtime_snapshots_before = runtime_snapshot_count(active_runtime_dir);
    const size_t switched_runtime_snapshots_before = runtime_snapshot_count(switched_runtime_dir);
    const int playtracker_commands_before =
        window->logText().count("stdin:@set-property dsplaytracker0 runtime-tuning-config-file=");
    qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
    const int pipeline_commands_before = window->logText().count("pipeline command ");
    activate(start);
    game_id->setText(switched_game_id);
    for (int i = 0; i < 200 &&
         (!window->logText().contains("one-pass stitching calibration complete; continuous stitched preview running") ||
          window->pipelineStateText() != "PLAYING");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_transition = YAML::LoadFile(config.string());
    YAML::Node transitioned_status;
    const bool has_transitioned_status =
        lookup_yaml_path(after_transition, {"hstream_ui", "stitching_calibration", "status"}, &transitioned_status);
    YAML::Node transitioned_invalidation_id;
    const bool has_transitioned_invalidation_id = lookup_yaml_path(
        after_transition, {"hstream_ui", "stitching_calibration", "invalidation_id"}, &transitioned_invalidation_id);
    const int original_max_speed_x = max_speed_x->value();
    max_speed_x->setValue(original_max_speed_x + 1);
    for (int i = 0; i < 50 &&
         !window->logText().contains(
             QString("camera control Max_Speed_X_x10=%1 apply=save/restart").arg(original_max_speed_x + 1));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const bool calibration_omitted_playtracker_runtime =
        runtime_snapshot_count(active_runtime_dir) == active_runtime_snapshots_before &&
        runtime_snapshot_count(switched_runtime_dir) == switched_runtime_snapshots_before &&
        window->logText().count("stdin:@set-property dsplaytracker0 runtime-tuning-config-file=") ==
            playtracker_commands_before &&
        window->logText().contains(
            QString("camera control Max_Speed_X_x10=%1 apply=save/restart").arg(original_max_speed_x + 1));
    game_id->setText(launched_game_id);
    max_speed_x->setValue(original_max_speed_x);
    for (int i = 0; i < 50 &&
         !window->logText().contains(
             QString("camera control Max_Speed_X_x10=%1 apply=save/restart").arg(original_max_speed_x));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (!expect(
            window->logText().count("stitching calibration clean command") == resume_clean_commands &&
                window->logText().contains(
                    "stitching calibration resuming from stale dependency features without cleaning cached inputs"),
            "Stop then Play should resume control-point calibration without cleaning upstream dependencies") ||
        !expect(
            window->logText().contains("hmstitcher: one-pass stitching configuration complete"),
            "Successful one-pass calibration should publish its completion marker") ||
        !expect(
            window->logText().count("pipeline command ") == pipeline_commands_before + 1,
            "Calibration and continuous stitched preview should use one application process") ||
        !expect(
            window->logText().contains("pipeline.streammux.batch-size=2") &&
                window->logText().contains("pipeline.streammux.sync-inputs=0") &&
                window->logText().contains("pipeline.streammux.batched-push-timeout=2147483647") &&
                window->logText().contains("pipeline.streammux.frame-num-reset-on-stream-reset=0") &&
                window->logText().contains("pipeline.streammux.frame-num-reset-on-eos=0") &&
                window->logText().contains("pipeline.hmstitcher.show=0") &&
                window->logText().contains("--stitching-calibration-only"),
            "Continuous preview should stay on the stitching-only graph without Program processing") ||
        !expect(window->pipelineStateText() == "PLAYING", "Continuous stitched preview should remain running") ||
        !expect(
            has_transitioned_status && transitioned_status.IsScalar() &&
                transitioned_status.as<std::string>() == "complete" && has_transitioned_invalidation_id &&
                transitioned_invalidation_id.IsScalar() && !transitioned_invalidation_id.as<std::string>().empty(),
            "Calibration should retain its completed generation owner while continuous preview keeps running") ||
        !expect(
            !fs::exists(switched_config),
            "Calibration completion should remain associated with the game that launched the run") ||
        !expect(
            calibration_omitted_playtracker_runtime,
            "Stitching-only calibration should not publish playtracker runtime configuration")) {
      qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
      activate(stop);
      return false;
    }
    const auto hugin_generation = write_live_hugin_generation_fixture(config.parent_path());
    if (!hugin_generation.ok()) {
      std::cerr << "Could not write live-rotation Hugin fixture: " << hugin_generation.status() << '\n';
      return false;
    }
    {
      auto generation_fixture_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!generation_fixture_lock.ok()) {
        std::cerr << "Could not lock live-rotation generation fixture: " << generation_fixture_lock.status() << '\n';
        return false;
      }
      YAML::Node generation_fixture = YAML::LoadFile(config.string());
      generation_fixture["rink"]["stitched_output_generation"] =
          "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin_generation->size()) + "\n" +
          *hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
      const auto published =
          hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(generation_fixture) + "\n");
      if (!published.ok()) {
        std::cerr << "Could not publish live-rotation generation fixture: " << published << '\n';
        return false;
      }
    }
    std::atomic<bool> config_lock_acquired{false};
    std::atomic<bool> config_lock_failed{false};
    std::thread config_locker([&]() {
      auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!config_lock.ok()) {
        config_lock_failed = true;
        return;
      }
      config_lock_acquired = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(450));
    });
    for (int i = 0; i < 100 && !config_lock_acquired && !config_lock_failed; ++i)
      QTest::qWait(5);
    QElapsedTimer responsiveness_timer;
    responsiveness_timer.start();
    qint64 responsive_callback_ms = -1;
    QTimer::singleShot(180, window, [&]() { responsive_callback_ms = responsiveness_timer.elapsed(); });
    rotate->setValue(73);
    for (int i = 0; i < 80 && responsive_callback_ms < 0; ++i) {
      QApplication::processEvents();
      QTest::qWait(5);
    }
    const bool authorization_was_async = !config_lock_failed && config_lock_acquired &&
        HStreamWindowTestAccess::liveRotationAuthorizationPending(window) && responsive_callback_ms >= 0 &&
        responsive_callback_ms < 325;
    config_locker.join();
    for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=73 apply=live"); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (!expect(
            authorization_was_async &&
                window->logText().contains("stdin:@set-property hmstitcher0 stitched-output-epoch=") &&
                window->logText().contains("camera control Stitch_Rotate_Degrees=73 apply=live"),
            "Completed live rotation authorization must not block the UI on the game config transaction")) {
      qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
      activate(stop);
      return false;
    }
    pipeline_process->write("@test-delay-runtime-control 250\n");
    for (int i = 0; i < 100 && !window->logText().contains("test runtime control delay armed"); ++i) {
      QApplication::processEvents();
      QTest::qWait(5);
    }
    const int commit_rotation_commands_before =
        window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=");
    rotate->setValue(72);
    for (int i = 0; i < 100 &&
         window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=") ==
             commit_rotation_commands_before;
         ++i) {
      QApplication::processEvents();
      QTest::qWait(5);
    }
    std::atomic<bool> commit_lock_acquired{false};
    std::thread commit_locker([&]() {
      auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!config_lock.ok())
        return;
      commit_lock_acquired = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(850));
    });
    for (int i = 0; i < 100 && !commit_lock_acquired; ++i)
      QTest::qWait(5);
    QElapsedTimer commit_responsiveness_timer;
    commit_responsiveness_timer.start();
    qint64 commit_responsive_callback_ms = -1;
    QTimer::singleShot(650, window, [&]() { commit_responsive_callback_ms = commit_responsiveness_timer.elapsed(); });
    for (int i = 0; i < 200 && commit_responsive_callback_ms < 0; ++i) {
      QApplication::processEvents();
      QTest::qWait(5);
    }
    const bool commit_was_async = commit_lock_acquired &&
        HStreamWindowTestAccess::liveRotationAuthorizationPending(window) && commit_responsive_callback_ms >= 0 &&
        commit_responsive_callback_ms < 775;
    commit_locker.join();
    for (int i = 0; i < 100 && !window->logText().contains("camera control Stitch_Rotate_Degrees=72 apply=live"); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (!commit_was_async || !window->logText().contains("camera control Stitch_Rotate_Degrees=72 apply=live")) {
      std::cerr << "commit async diagnostic: result=" << commit_was_async << " lock=" << commit_lock_acquired
                << " pending=" << HStreamWindowTestAccess::liveRotationAuthorizationPending(window)
                << " callback-ms=" << commit_responsive_callback_ms << '\n'
                << window->logText().right(2500).toStdString() << '\n';
    }
    if (!expect(
            commit_was_async && window->logText().contains("camera control Stitch_Rotate_Degrees=72 apply=live"),
            "Live rotation finalization must not block the UI on the game config transaction")) {
      qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
      activate(stop);
      return false;
    }
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    for (int i = 0; i < 200 && HStreamWindowTestAccess::liveRotationAuthorizationPending(window); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");

    const YAML::Node after_preview_stop = YAML::LoadFile(config.string());
    YAML::Node stopped_status;
    const bool has_stopped_status =
        lookup_yaml_path(after_preview_stop, {"hstream_ui", "stitching_calibration", "status"}, &stopped_status);
    if (!expect(
            has_stopped_status && stopped_status.IsScalar() && stopped_status.as<std::string>() == "complete",
            "Stopping the post-calibration preview should preserve completed calibration state") ||
        !expect(
            stitched_status->text() == "Stitched canvas preview",
            "Stopping a completed calibration preview should clear the active stitched status")) {
      return false;
    }

    {
      auto fixture_cleanup_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!fixture_cleanup_lock.ok()) {
        std::cerr << "Could not lock live-rotation generation fixture cleanup: " << fixture_cleanup_lock.status()
                  << '\n';
        return false;
      }
      YAML::Node fixture_cleanup = YAML::LoadFile(config.string());
      fixture_cleanup["rink"].remove("stitched_output_generation");
      fixture_cleanup["rink"].remove("stitched_output_persisted_rotation_degrees");
      fixture_cleanup["rink"].remove("stitched_output_pending_generation");
      fixture_cleanup["rink"].remove("stitched_output_pending_authorization_id");
      fixture_cleanup["rink"].remove("stitched_output_pending_owner_process");
      fixture_cleanup["rink"].remove("stitched_output_pending_previous_generation");
      fixture_cleanup["rink"].remove("stitched_output_pending_previous_authorization_id");
      fixture_cleanup["rink"].remove("stitched_output_pending_previous_owner_process");
      fixture_cleanup["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
      const auto published =
          hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(fixture_cleanup) + "\n");
      if (!published.ok()) {
        std::cerr << "Could not clean live-rotation generation fixture: " << published << '\n';
        return false;
      }
    }

    {
      auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!config_lock.ok()) {
        std::cerr << "Could not lock interrupted input-stage calibration state: " << config_lock.status() << '\n';
        return false;
      }
      YAML::Node interrupted = YAML::LoadFile(config.string());
      YAML::Node calibration = interrupted["hstream_ui"]["stitching_calibration"];
      calibration["control_points"] = 750;
      calibration["status"] = "pending";
      calibration["stale_from"] = "input";
      calibration["artifacts_invalidated"] = true;
      calibration["invalidation_id"] = "interrupted-input-run";
      const auto published = hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(interrupted) + "\n");
      if (!published.ok()) {
        std::cerr << "Could not publish interrupted input-stage calibration state: " << published << '\n';
        return false;
      }
    }
    const int full_clean_commands_before = window->logText().count(" --clean --clean-expected-invalidation-id=");
    control_points->setValue(775);
    qputenv("HSTREAM_UI_TEST_CLEAN_INVALIDATE_INPUT", "1");
    activate(start);
    qunsetenv("HSTREAM_UI_TEST_CLEAN_INVALIDATE_INPUT");
    const YAML::Node superseded_clean = YAML::LoadFile(config.string());
    const YAML::Node superseded_calibration = superseded_clean["hstream_ui"]["stitching_calibration"];
    if (!expect(
            window->pipelineStateText() == "STOPPED" &&
                superseded_calibration["status"].as<std::string>() == "pending" &&
                superseded_calibration["stale_from"].as<std::string>() == "input" &&
                !superseded_calibration["artifacts_invalidated"].as<bool>() &&
                window->logText().count(" --clean --clean-expected-invalidation-id=") ==
                    full_clean_commands_before + 1 &&
                window->logText().contains(
                    "stitching calibration cleanup was superseded by a newer dependency invalidation"),
            "A CP change after an interrupted input-stage run must fully clean, while a newer input invalidation wins")) {
      return false;
    }
    control_points->setValue(750);

    const int forced_restarts_before = window->logText().count("--force-reconfigure");
    const int calibration_completions_before =
        window->logText().count("one-pass stitching calibration complete; continuous stitched preview running");
    qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
    activate(restart);
    for (int i = 0; i < 200 &&
         (!window->logText().contains(
              "stitching calibration restart requested; rebuilding the complete dependency graph") ||
          window->logText().count("--force-reconfigure") == forced_restarts_before ||
          window->logText().count("one-pass stitching calibration complete; continuous stitched preview running") ==
              calibration_completions_before ||
          window->pipelineStateText() != "PLAYING");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const bool forced_restart_ok =
        expect(
            window->logText().count("--force-reconfigure") > forced_restarts_before,
            "Restart Stage should explicitly force the complete calibration dependency graph") &&
        expect(
            window->logText().contains(
                "stitching calibration restart requested; rebuilding the complete dependency graph"),
            "Restart Stage should identify the full rebuild in the log") &&
        expect(
            window->logText().contains("--force-reconfigure --clean-expected-invalidation-id="),
            "Restart Stage should not repeat its synchronous artifact clean inside the forced pipeline");
    activate(stop);
    qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
    if (!forced_restart_ok) {
      return false;
    }

    const int clean_commands_before = window->logText().count("stitching calibration clean command");
    const std::vector<std::vector<int>> rejection_scoreboard_polygon = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    const auto rejection_hugin_generation = write_live_hugin_generation_fixture(config.parent_path());
    if (!rejection_hugin_generation.ok()) {
      std::cerr << "Could not write live-rotation rejection Hugin fixture: " << rejection_hugin_generation.status()
                << '\n';
      return false;
    }
    {
      auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
      if (!config_lock.ok()) {
        std::cerr << "Could not lock live-rotation rejection fixture: " << config_lock.status() << '\n';
        return false;
      }
      YAML::Node rejection_fixture = YAML::LoadFile(config.string());
      rejection_fixture["rink"]["stitched_output_generation"] =
          "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(rejection_hugin_generation->size()) + "\n" +
          *rejection_hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
      rejection_fixture["rink"]["scoreboard"]["perspective_polygon"] = rejection_scoreboard_polygon;
      const auto published =
          hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(rejection_fixture) + "\n");
      if (!published.ok()) {
        std::cerr << "Could not publish live-rotation rejection fixture: " << published << '\n';
        return false;
      }
    }
    qputenv("HSTREAM_UI_TEST_REJECT_RUNTIME_CONTROL", "1");
    activate(start);
    for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    rotate->setValue(71);
    for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=71 apply=failed");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_rejected_rotation = YAML::LoadFile(config.string());
    const bool rejected_rotation_preserved_config =
        !after_rejected_rotation["rink"]["stitched_output_pending_generation"].IsDefined() &&
        after_rejected_rotation["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>() ==
            rejection_scoreboard_polygon;
    auto* fixed_edge_link = require_child<QSlider>(window, "cameraSlider_Link_Fixed_Edge_Rotation_Left_Right");
    auto* fixed_edge_left = require_child<QSlider>(window, "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10");
    if (!fixed_edge_link || !fixed_edge_left) {
      activate(stop);
      return false;
    }
    fixed_edge_link->setValue(1);
    fixed_edge_left->setValue(310);
    for (int i = 0; i < 50 &&
         !window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=save/restart");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }

    qputenv("HSTREAM_UI_TEST_RUNTIME_REJECTION_DELAY_MS", "300");
    const auto rejection_predecessor = hm::stitching::authorize_live_stitched_output_rotation(
        config.parent_path().string(), 4.0, "ui-rejection-predecessor");
    activate(start);
    for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    rotate->setValue(69);
    bool rejection_authorization_published = false;
    for (int i = 0; i < 100 && !rejection_authorization_published; ++i) {
      QApplication::processEvents();
      QTest::qWait(5);
      const YAML::Node pending = YAML::LoadFile(config.string());
      rejection_authorization_published = pending["rink"]["stitched_output_pending_generation"].IsDefined() &&
          pending["rink"]["stitched_output_pending_authorization_id"].IsDefined() &&
          pending["rink"]["stitched_output_pending_authorization_id"].as<std::string>() != "ui-rejection-predecessor";
    }
    auto rejection_exit_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
    const bool exit_requested_during_rejection = rejection_exit_lock.ok() &&
        HStreamWindowTestAccess::requestPipelineProcessExit(window) == QByteArray("@test-exit\n").size();
    for (int i = 0; i < 150 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (rejection_exit_lock.ok())
      rejection_exit_lock->reset();
    for (int i = 0; i < 200 && HStreamWindowTestAccess::liveRotationAuthorizationPending(window); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_rejection_exit = YAML::LoadFile(config.string());
    const bool rejection_exit_unwound = rejection_predecessor.ok() && *rejection_predecessor &&
        rejection_authorization_published && exit_requested_during_rejection &&
        window->pipelineStateText() == "STOPPED" &&
        !HStreamWindowTestAccess::liveRotationAuthorizationPending(window) &&
        !after_rejection_exit["rink"]["stitched_output_pending_generation"].IsDefined() &&
        !after_rejection_exit["rink"]["stitched_output_pending_authorization_id"].IsDefined() &&
        after_rejection_exit["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>() ==
            rejection_scoreboard_polygon;
    if (!rejection_exit_unwound) {
      std::cerr << "rejection-exit state: predecessor=" << (rejection_predecessor.ok() && *rejection_predecessor)
                << " published=" << rejection_authorization_published
                << " exit-requested=" << exit_requested_during_rejection
                << " pipeline=" << window->pipelineStateText().toStdString()
                << " worker-pending=" << HStreamWindowTestAccess::liveRotationAuthorizationPending(window)
                << "\nconfig:\n"
                << YAML::Dump(after_rejection_exit) << "\nlog:\n"
                << window->logText().right(6000).toStdString() << '\n';
    }
    qunsetenv("HSTREAM_UI_TEST_RUNTIME_REJECTION_DELAY_MS");
    qunsetenv("HSTREAM_UI_TEST_REJECT_RUNTIME_CONTROL");
    if (!expect(
            window->logText().count("stitching calibration clean command") == clean_commands_before,
            "A completed calibration should reopen continuous preview without recalibrating") ||
        !expect(
            window->logText().contains("camera control Stitch_Rotate_Degrees=71 apply=pending") &&
                window->logText().contains("camera control Stitch_Rotate_Degrees=71 apply=failed") &&
                !window->logText().contains("camera control Stitch_Rotate_Degrees=71 apply=live"),
            "Rejected runtime controls should not be reported as live") ||
        !expect(
            rejected_rotation_preserved_config,
            "Rejected live rotation must cancel its exact authorization without deleting scoreboard geometry") ||
        !expect(
            rejection_exit_unwound,
            "Pipeline exit during rejection rollback must unwind restored predecessor authority") ||
        !expect(
            window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=save/restart") &&
                !window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=pending") &&
                !window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=failed"),
            "Calibration should not send fixed-edge controls to omitted Program stages")) {
      return false;
    }

    qputenv("HSTREAM_UI_TEST_SHORT_LIVE_ROTATION_WRITE", "1");
    render_video->setChecked(false);
    const int disable_recoveries_before_write_error = window->logText().count("GPU preview disable failed (");
    activate(start);
    for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    rotate->setValue(70);
    for (int i = 0; i < 100 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    for (int i = 0; i < 200 && HStreamWindowTestAccess::liveRotationAuthorizationPending(window); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_failed_rotation_write = YAML::LoadFile(config.string());
    const bool write_error_stopped_for_reconciliation = window->pipelineStateText() == "STOPPED" &&
        !HStreamWindowTestAccess::liveRotationAuthorizationPending(window) &&
        !after_failed_rotation_write["rink"]["stitched_output_pending_generation"].IsDefined() &&
        !after_failed_rotation_write["rink"]["stitched_output_pending_authorization_id"].IsDefined();
    const bool rendering_stayed_unchecked = !render_video->isChecked();
    const bool target_stayed_unmapped = preview_target->isHidden();
    const bool backend_reported_disabled =
        window->logText().contains("GPU preview backend ready with rendering disabled generation=");
    const bool embedded_backend_state_is_valid =
        !hm::ui_internal::supports_x11_embedding(QGuiApplication::platformName()) || backend_reported_disabled;
    const int disable_recoveries_after_write_error = window->logText().count("GPU preview disable failed (");
    const bool write_error_kept_rendering_disabled = rendering_stayed_unchecked && target_stayed_unmapped &&
        embedded_backend_state_is_valid &&
        disable_recoveries_after_write_error == disable_recoveries_before_write_error;
    qunsetenv("HSTREAM_UI_TEST_SHORT_LIVE_ROTATION_WRITE");
    render_video->setChecked(true);
    if (!write_error_kept_rendering_disabled) {
      std::cerr << "write-error render state: checked=" << !rendering_stayed_unchecked
                << " target-mapped=" << !target_stayed_unmapped << " backend-disabled=" << backend_reported_disabled
                << " disable-recoveries=" << disable_recoveries_before_write_error << "->"
                << disable_recoveries_after_write_error << '\n';
    }
    if (window->pipelineStateText() != "STOPPED")
      activate(stop);
    if (!expect(
            write_error_stopped_for_reconciliation,
            "A failed live-rotation command write must stop playback before authorization rollback") ||
        !expect(
            write_error_kept_rendering_disabled,
            "An unrelated write error after acknowledged render-off must not re-enable or remap GPU preview")) {
      return false;
    }
  }

  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    YAML::Node completed = YAML::LoadFile(config.string());
    completed["hstream_ui"]["stitching_calibration"]["control_points"] = 750;
    completed["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    completed["hstream_ui"]["stitching_calibration"].remove("stale_from");
    completed["hstream_ui"]["stitching_calibration"].remove("artifacts_invalidated");
    {
      std::ofstream out(config);
      out << completed << '\n';
    }

    log->clear();
    mode->setCurrentIndex(mode->findData("program"));
    control_points->setValue(775);
    const int clean_commands_before = window->logText().count("stitching calibration clean command");
    qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
    activate(start);
    for (int i = 0; i < 200 &&
         (!window->logText().contains("one-pass stitching calibration complete; continuous program playback running") ||
          window->pipelineStateText() != "PLAYING");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_program_cp_change = YAML::LoadFile(config.string());
    const YAML::Node after_program_calibration = after_program_cp_change["hstream_ui"]["stitching_calibration"];
    const bool program_cp_recalibrated =
        expect(
            window->logText().count("stitching calibration clean command") == clean_commands_before + 1 &&
                window->logText().contains("stitching calibration control points changed 750 -> 775") &&
                window->logText().contains("--clean-from-control-points"),
            "Changing CP in Program mode must invalidate control-point-dependent artifacts") &&
        expect(
            after_program_calibration["control_points"].as<int>() == 775 &&
                after_program_calibration["status"].as<std::string>() == "complete",
            "Program CP recalibration must persist the selected count and completed state") &&
        expect(
            window->pipelineStateText() == "PLAYING",
            "Program playback must continue in the same process after a CP-triggered recalibration");
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
    if (!program_cp_recalibrated) {
      return false;
    }
  }

  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    YAML::Node invalidated = YAML::LoadFile(config.string());
    invalidated["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    {
      std::ofstream out(config);
      out << invalidated << "\n";
    }

    log->clear();
    mode->setCurrentIndex(mode->findData("program"));
    const int clean_commands_before = window->logText().count("stitching calibration clean command");
    const int program_completions_before =
        window->logText().count("one-pass stitching calibration complete; continuous program playback running");
    qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
    activate(start);
    for (int i = 0; i < 200 &&
         (window->logText().count("one-pass stitching calibration complete; continuous program playback running") ==
              program_completions_before ||
          window->pipelineStateText() != "PLAYING");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const YAML::Node after_program_calibration = YAML::LoadFile(config.string());
    YAML::Node program_status;
    const bool has_program_status =
        lookup_yaml_path(after_program_calibration, {"hstream_ui", "stitching_calibration", "status"}, &program_status);
    const int clean_commands_after = window->logText().count("stitching calibration clean command");
    const bool program_recalibrated =
        expect(
            clean_commands_after == clean_commands_before + 1,
            "Program playback should clean stale stitch artifacts when video inputs invalidate calibration") &&
        expect(
            has_program_status && program_status.IsScalar() && program_status.as<std::string>() == "complete",
            "Program one-pass calibration should mark the replacement video inputs complete") &&
        expect(
            window->logText().contains("pipeline.streammux.batch-size=2") &&
                window->logText().contains("pipeline.streammux.sync-inputs=0") &&
                window->logText().contains("pipeline.streammux.batched-push-timeout=2147483647") &&
                window->logText().contains("pipeline.streammux.frame-num-reset-on-stream-reset=0") &&
                window->logText().contains("pipeline.streammux.frame-num-reset-on-eos=0") &&
                window->logText().contains("pipeline.hmstitcher.show=0"),
            "Program one-pass calibration should synchronize both stitcher inputs") &&
        expect(
            window->pipelineStateText() == "PLAYING",
            "Program playback should continue in the same process after recalibrating replacement inputs");
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
    if (!program_recalibrated) {
      return false;
    }
  }

  mode->setCurrentIndex(mode->findData("program"));
  qputenv("HM_RENDER_SINK", "nv3dsink");
  const bool x11_test_backend = QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0;
  const int embedded_commands_before_external_run = window->logText().count("--render-window-id=");
  const int show_commands_before_external_run = window->logText().count("--show");
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("HM_RENDER_SINK=nv3dsink"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (x11_test_backend) {
    for (int i = 0; i < 100 && preview_surface->isHidden(); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
  }
  if (!expect(
          window->logText().count("--show") >= show_commands_before_external_run + 1,
          "Program runs should retain the logical render sink for audio monitoring") ||
      !expect(
          window->logText().count("--render-window-id=") == embedded_commands_before_external_run,
          "nv3dsink run must not promise an embedded preview") ||
      !expect(
          window->logText().contains("USE_NEW_NVSTREAMMUX=yes"),
          "UI runner should default to the DeepStream 9.1 new stream mux") ||
      !expect(
          window->logText().contains("HM_RENDER_SINK=nv3dsink"),
          "UI runner should preserve the self-managed desktop render sink") ||
      !expect(
          window->logText().contains("HM_NO_SCOREBOARD=") && !window->logText().contains("HM_NO_SCOREBOARD=1") &&
              window->logText().contains("scoreboard selector opened in Qt") &&
              window->logText().contains("scoreboard selection complete; pipeline continuing"),
          "Program playback should leave scoreboard selection enabled and launch its native selector") ||
      !expect(
          x11_test_backend ? !window->logText().contains("separate DeepStream window")
                           : window->logText().contains("separate DeepStream window"),
          "UI must distinguish embedded X11 preview from self-managed render-window mode") ||
      !expect(
          external_notice->parentWidget() == program_host &&
              (x11_test_backend ? !preview_surface->isHidden() : preview_surface->isHidden()),
          "Preview surface visibility must match the selected embedded or external render mode")) {
    return false;
  }
  window->resize(window->width(), 1200);
  main_log_splitter->setSizes({800, 350});
  preview_tabs->setCurrentIndex(2);
  QApplication::processEvents();
  QTest::qWait(10);
  if (!expect(
          x11_test_backend || external_notice->geometry() == program_host->rect(),
          "External-render notice should resize and move with its preview tab when the log splitter moves") ||
      !expect(
          camera1_notice->parentWidget() == camera1_host &&
              (x11_test_backend || camera1_notice->geometry() == camera1_host->rect()),
          "Camera-render notice should remain owned and resized by its tab when the log splitter moves")) {
    return false;
  }
  preview_tabs->setCurrentIndex(0);
  window->resize(1440, 900);
  QApplication::processEvents();
  activate(stop);
  qunsetenv("HM_RENDER_SINK");

  qputenv("HM_RENDER_SINK", "nveglglessink");
  const int embedded_commands_before_unsupported_egl = window->logText().count("--render-window-id=");
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("HM_RENDER_SINK=nveglglessink"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool explicit_embedding_preserved =
      expect(
          window->logText().count("--render-window-id=") == embedded_commands_before_unsupported_egl,
          "Explicit nveglglessink mode should not receive a non-X11 native window handle") &&
      expect(
          window->logText().contains("HM_RENDER_SINK=nveglglessink"),
          "UI runner should preserve an explicit external render sink");
  activate(stop);
  qunsetenv("HM_RENDER_SINK");
  if (!explicit_embedding_preserved) {
    return false;
  }

  const int fake_sink_commands_before = window->logText().count("--enable-sinks=FAKE");
  const int render_sink_commands_before = window->logText().count("--enable-sinks=RENDER");
  const int embedded_commands_before = window->logText().count("--render-window-id=");
  const int source_embedded_commands_before = window->logText().count("--source-render-window-ids=");
  const int gpu_preview_commands_before = window->logText().count("--ui-preview-windows=");
  const int inactive_preview_commands_before = window->logText().count("--ui-preview-active=none");
  const int muted_launches_before = window->logText().count("HSTREAM_RENDER_AUDIO_MUTED=1");
  render_video->setChecked(false);
  activate(start);
  for (int i = 0; i < 100 &&
       (window->pipelineStateText() != "PLAYING" ||
        window->logText().count("HSTREAM_RENDER_AUDIO_MUTED=1") <= muted_launches_before);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const int fake_sink_commands_after = window->logText().count("--enable-sinks=FAKE");
  const int render_sink_commands_after = window->logText().count("--enable-sinks=RENDER");
  const int gpu_preview_commands_after = window->logText().count("--ui-preview-windows=");
  const int inactive_preview_commands_after = window->logText().count("--ui-preview-active=none");
  const bool disabled_sink_selection =
      fake_sink_commands_after == fake_sink_commands_before + (x11_test_backend ? 0 : 2) &&
      render_sink_commands_after == render_sink_commands_before + (x11_test_backend ? 2 : 0);
  if (!disabled_sink_selection) {
    std::cerr << "disabled render sink counts: platform=" << QGuiApplication::platformName().toStdString()
              << " fake=" << fake_sink_commands_before << "->" << fake_sink_commands_after
              << " render=" << render_sink_commands_before << "->" << render_sink_commands_after << '\n';
  }
  const int expected_dormant_preview_launches =
      x11_test_backend ? render_sink_commands_after - render_sink_commands_before : 0;
  const bool dormant_preview_provisioned =
      gpu_preview_commands_after == gpu_preview_commands_before + expected_dormant_preview_launches &&
      inactive_preview_commands_after == inactive_preview_commands_before + expected_dormant_preview_launches;
  if (!dormant_preview_provisioned) {
    std::cerr << "disabled preview provisioning counts: platform=" << QGuiApplication::platformName().toStdString()
              << " windows=" << gpu_preview_commands_before << "->" << gpu_preview_commands_after
              << " inactive=" << inactive_preview_commands_before << "->" << inactive_preview_commands_after << '\n';
  }
  const bool rendering_disabled =
      expect(
          disabled_sink_selection,
          "A disabled X11 preview should retain the dormant render branch; non-X11 should use a fake sink") &&
      expect(
          window->logText().count("HSTREAM_RENDER_AUDIO_MUTED=1") == muted_launches_before + 1,
          "A run started with Render video off must construct its local monitor-audio branch muted") &&
      expect(
          window->logText().count("--render-window-id=") == embedded_commands_before,
          "Disabling video rendering should not attach a native preview window") &&
      expect(
          window->logText().count("--source-render-window-ids=") == source_embedded_commands_before,
          "Disabling video rendering should not attach native source-camera preview windows") &&
      expect(
          dormant_preview_provisioned,
          "An X11 run that starts disabled should provision dormant GPU branches so rendering can be enabled live") &&
      expect(
          external_notice->text() == "Video rendering is disabled",
          "The active preview tab should explain that rendering is disabled") &&
      expect(
          camera1_notice->text() == "Video rendering is disabled",
          "Camera tabs should explain that rendering is disabled");
  activate(stop);
  render_video->setChecked(true);
  if (!rendering_disabled) {
    return false;
  }

  qputenv("USE_NEW_NVSTREAMMUX", "no");
  qputenv("HM_NO_SCOREBOARD", "0");
  activate(start);
  for (int i = 0; i < 50 &&
       (!window->logText().contains("USE_NEW_NVSTREAMMUX=no") || !window->logText().contains("HM_NO_SCOREBOARD=0"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool environment_overrides_preserved =
      expect(
          window->logText().contains("USE_NEW_NVSTREAMMUX=no"),
          "UI runner should preserve an explicit legacy stream mux override") &&
      expect(
          window->logText().contains("HM_NO_SCOREBOARD=0"),
          "UI runner should preserve an explicit interactive scoreboard override");
  activate(stop);
  qunsetenv("USE_NEW_NVSTREAMMUX");
  qunsetenv("HM_NO_SCOREBOARD");
  if (!environment_overrides_preserved) {
    return false;
  }

  activate(restart);
  const bool restart_logged =
      expect(window->logText().contains("stage restart requested"), "Restart button should log a stage restart");
  activate(stop);
  for (int i = 0; i < 100 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!restart_logged) {
    return false;
  }

  const QByteArray original_runner = qgetenv("HSTREAM_UI_TEST_RUNNER");
  const int process_errors_before = window->logText().count("pipeline process error");
  qputenv("HSTREAM_UI_TEST_RUNNER", "/tmp/hstream-ui-missing-runner");
  activate(start);
  for (int i = 0; i < 50 && window->logText().count("pipeline process error") == process_errors_before; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HSTREAM_UI_TEST_RUNNER", original_runner);
  const std::string failed_progress_details =
      " visible=" + std::string(playback_progress->isVisible() ? "true" : "false") +
      " range=" + std::to_string(playback_progress->minimum()) + ".." + std::to_string(playback_progress->maximum()) +
      " value=" + std::to_string(playback_progress->value()) + " format=" + playback_progress->format().toStdString() +
      " state=" + playback_progress->property("playbackState").toString().toStdString() +
      " tooltip=" + playback_progress->toolTip().toStdString();
  const bool absolute_failure_clean =
      expect(window->pipelineStateText() == "STOPPED", "Failed runner should restore stopped state") &&
      expect(
          window->logText().count("pipeline process error") == process_errors_before + 1,
          "Failed runner should log a new process error") &&
      expect(
          playback_progress->isVisible() && playback_progress->minimum() == 0 && playback_progress->maximum() == 1000 &&
              playback_progress->value() == 1000 && playback_progress->format().contains("ERROR") &&
              playback_progress->property("playbackState").toString() == "error" &&
              playback_progress->toolTip().contains("Pipeline: ERROR"),
          "A fatal process error should leave a full red ERROR progress result visible:" + failed_progress_details);
  if (!absolute_failure_clean)
    return false;
  if (!capture_interaction_artifact(window, "playback-error.png")) {
    return false;
  }

  const QByteArray original_path = qgetenv("PATH");
  qputenv("HSTREAM_UI_TEST_RUNNER", "hstream-ui-missing-runner");
  qputenv("PATH", "/usr/bin:/bin");
  qputenv("HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW", "1");
  qputenv("HSTREAM_UI_TEST_BYPASS_SETSID", "1");
  // Click without processing events so the synchronous startup setup can be
  // inspected before QProcess delivers its queued FailedToStart signal.
  start->click();
  const bool targets_safe_before_failed_start = preview_target->isHidden() && stitched_target->isHidden() &&
      camera1_target->isHidden() && program_focus->isHidden();
  for (int i = 0; i < 100 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool asynchronous_failure_clean =
      expect(
          targets_safe_before_failed_start,
          "Embedded startup must keep native targets and focus controls unmapped until a GPU frame is ready") &&
      expect(
          window->pipelineStateText() == "STOPPED",
          "Asynchronous QProcess FailedToStart should restore stopped state") &&
      expect(
          preview_target->isHidden() && stitched_target->isHidden() && camera1_target->isHidden(),
          "Asynchronous QProcess FailedToStart must unmap every native preview target");
  qputenv("HSTREAM_UI_TEST_RUNNER", original_runner);
  qputenv("PATH", original_path);
  qunsetenv("HSTREAM_UI_TEST_FORCE_EMBEDDED_PREVIEW");
  qunsetenv("HSTREAM_UI_TEST_BYPASS_SETSID");
  return asynchronous_failure_clean;
}

bool test_output_controls(HStreamWindow* window) {
  auto* spare = require_child<QCheckBox>(window, "outputToggle_spare-rtmp");
  auto* archive = require_child<QCheckBox>(window, "outputToggle_archive-file");
  auto* stitched_archive = require_child<QCheckBox>(window, "outputToggle_archive-stitched");
  auto* drivegpt_csv = require_child<QCheckBox>(window, "drivegptCsvCheck");
  auto* archive_path = require_child<QLabel>(window, "archiveOutputPath");
  auto* stitched_archive_path = require_child<QLabel>(window, "stitchedArchiveOutputPath");
  auto* game_id_edit = require_child<QLineEdit>(window, "gameIdEdit");
  auto* youtube_redirect = require_child<QPushButton>(window, "redirectYoutubeButton");
  auto* add_rtsp = require_child<QPushButton>(window, "addRtspButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* seek_slider = require_child<QSlider>(window, "playbackSeekSlider");
  auto* seek_forward = require_child<QPushButton>(window, "playbackSeekForward10Button");
  if (!spare || !archive || !stitched_archive || !drivegpt_csv || !archive_path || !stitched_archive_path ||
      !game_id_edit || !youtube_redirect || !add_rtsp || !start || !stop || !mode || !seek_slider || !seek_forward) {
    return false;
  }

  activate(spare);
  if (!expect(window->outputStateText("spare-rtmp") == "ENABLED", "Spare RTMP toggle should enable the output")) {
    return false;
  }

  activate(youtube_redirect);
  if (!expect(
          window->outputStateText("youtube-primary") == "REDIRECTED",
          "Redirect button should mark YouTube output redirected")) {
    return false;
  }

  activate(add_rtsp);
  if (!expect(
          window->outputStateText("rtsp-dynamic-1") == "ENABLED",
          "Add RTSP button should create an enabled RTSP output")) {
    return false;
  }

  QTemporaryDir output_root;
  if (!output_root.isValid()) {
    return false;
  }
  const QByteArray original_output_root = qgetenv("HM_OUTPUT_WORK_DIR");
  qputenv("HM_OUTPUT_WORK_DIR", "relative-output-test");
  archive->setChecked(true);
  const QString original_game_id = game_id_edit->text();
  game_id_edit->setText("archive-relative-path-test");
  const QString relative_planned_path =
      QDir(QDir(QDir::currentPath()).filePath("relative-output-test/archive-relative-path-test"))
          .filePath("tracking_output-with-audio.mkv");
  const bool relative_override_resolved = expect(
      archive_path->text().contains(relative_planned_path),
      "A relative HM_OUTPUT_WORK_DIR should resolve from the backend working directory in both the UI and backend");
  mode->setCurrentIndex(mode->findData("stitch-calibration"));
  const QString calibration_planned_path =
      QDir(QDir(QDir::currentPath()).filePath("relative-output-test/archive-relative-path-test"))
          .filePath("stitched_output-with-audio.mkv");
  const QStringList calibration_archive_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  const bool calibration_archive_path_unmodified = std::none_of(
      calibration_archive_arguments.cbegin(), calibration_archive_arguments.cend(), [](const QString& argument) {
        return argument.startsWith("--options=pipeline.sink2.output-file=") ||
            argument.startsWith("--options=video_out.output_video_path=");
      });
  const bool calibration_archive_routed = expect(
      archive_path->text().contains(calibration_planned_path) &&
          calibration_archive_arguments.contains("--enable-sinks=RENDER,ENCODE_STITCHED_FILE") &&
          calibration_archive_path_unmodified && !stitched_archive->isEnabled() &&
          !calibration_archive_arguments.join(' ').contains("RTMP") &&
          !calibration_archive_arguments.join(' ').contains("RTSP"),
      "Stitching Calibration Archive File must record the stitched sink without overriding native or canonical "
      "custom archive paths or enabling streams");
  mode->setCurrentIndex(mode->findData("program"));
  stitched_archive->setChecked(true);
  const QStringList dual_archive_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  const bool dual_archive_routed = expect(
      stitched_archive->isEnabled() && dual_archive_arguments.join(' ').contains("ENCODE_FILE") &&
          dual_archive_arguments.join(' ').contains("ENCODE_STITCHED_FILE") &&
          stitched_archive_path->text().contains("stitched_output-with-audio.mkv"),
      "Program mode must independently route Program and stitched archives when both toggles are checked");
  stitched_archive->setChecked(false);

  qputenv("HM_OUTPUT_WORK_DIR", output_root.path().toLocal8Bit());
  game_id_edit->setText("archive-label-refresh-test");
  const QString alternate_path =
      QDir(QDir(output_root.path()).filePath("archive-label-refresh-test")).filePath("tracking_output-with-audio.mkv");
  const bool path_refreshes_with_game = expect(
      archive_path->text().contains(alternate_path),
      "Archive path should refresh immediately when the game ID changes");
  game_id_edit->setText(original_game_id);
  const QString planned_path =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("tracking_output-with-audio.mkv");
  const QString expected_path =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("custom-archive.mkv");
  const QString expected_job_log = expected_path + ".log";
  const QString restarted_recovery_path =
      QDir(QFileInfo(expected_path).absolutePath()).filePath("custom-archive-finalization-failed.mkv");
  QDir().mkpath(QFileInfo(expected_path).absolutePath());
  QFile::remove(planned_path);
  QFile::remove(expected_job_log);
  QFile::remove(restarted_recovery_path);
  QFile interrupted_archive(planned_path);
  if (!interrupted_archive.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      interrupted_archive.write("archive from interrupted UI session") < 0) {
    return false;
  }
  interrupted_archive.close();
  QFile existing_archive(expected_path);
  if (!existing_archive.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      existing_archive.write("interrupted custom archive") < 0) {
    return false;
  }
  existing_archive.close();
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", expected_path.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVER_EXISTING", "1");
  const bool path_visible_before_start = expect(
      archive_path->text().contains(planned_path),
      "Archive toggle should show the exact output path before playback starts");
  activate(start);
  for (int i = 0; i < 100 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 100 && !window->logText().contains(QString("HM_OUTPUT_WORK_DIR=%1").arg(output_root.path()));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool path_prepared = expect(
      QDir(QFileInfo(planned_path).absolutePath()).exists() && archive_path->text().contains(expected_path) &&
          window->logText().contains(QString("archive output: %1").arg(planned_path)) &&
          window->logText().contains(QString("archive backend resolved output: %1").arg(expected_path)) &&
          window->logText().contains(QString("HM_OUTPUT_WORK_DIR=%1").arg(output_root.path())) &&
          window->logText().contains(QRegularExpression("HSTREAM_ARCHIVE_RUN_ID=[0-9]+-[0-9a-f-]+")),
      "Archive playback should show the backend's exact resolved path and pass a deterministic output directory");
  const int seek_commands_before_nonlocal_click = window->logText().count("stdin:@seek");
  activate(seek_forward);
  QApplication::processEvents();
  const bool nonlocal_seek_blocked = expect(
      !seek_slider->isEnabled() && !seek_forward->isEnabled() &&
          window->logText().count("stdin:@seek") == seek_commands_before_nonlocal_click,
      "Archive/RTMP/RTSP playback must disable seeking and send no seek command");
  QFile recovered_interrupted_archive(restarted_recovery_path);
  const bool recovered_interrupted_archive_opened = recovered_interrupted_archive.open(QIODevice::ReadOnly);
  const bool interrupted_archive_preserved = expect(
      recovered_interrupted_archive_opened &&
          recovered_interrupted_archive.readAll() == QByteArray("interrupted custom archive") &&
          !QFileInfo::exists(expected_path) && QFileInfo(planned_path).size() > 0 &&
          archive_path->text().contains(restarted_recovery_path) &&
          window->logText().contains(
              QString("pre-existing archive work file preserved for recovery: %1").arg(restarted_recovery_path)),
      "Backend setup must preserve its actual custom work MKV without moving the UI's guessed default path");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool missing_new_output_reported = expect(
      window->logText().contains(QString("archive output was not created; expected: %1").arg(expected_path)),
      "Archive playback must not claim that the safely recovered prior file came from the current run");
  QFile persisted_job_log(expected_job_log);
  const bool persisted_job_log_opened = persisted_job_log.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString persisted_job_log_text =
      persisted_job_log_opened ? QString::fromUtf8(persisted_job_log.readAll()) : QString();
  const QFileDevice::Permissions persisted_job_log_permissions = QFileInfo(expected_job_log).permissions();
  const QFileDevice::Permissions non_owner_permissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
      QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
  const bool job_log_persisted = expect(
      persisted_job_log_opened &&
          (persisted_job_log_permissions & (QFileDevice::ReadOwner | QFileDevice::WriteOwner)) ==
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
          (persisted_job_log_permissions & non_owner_permissions) == 0 &&
          persisted_job_log_text.contains("pipeline command") &&
          persisted_job_log_text.contains(QString("archive backend resolved output: %1").arg(expected_path)) &&
          persisted_job_log_text.contains("pipeline finished") &&
          persisted_job_log_text.contains(QString("archive output was not created; expected: %1").arg(expected_path)),
      "Each archive job must persist the UI log beside its resolved work video with owner-only permissions");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVER_EXISTING");

  const QString incomplete_exit_path = QDir(QFileInfo(expected_path).absolutePath()).filePath("incomplete-exit.mkv");
  const QString incomplete_exit_log = incomplete_exit_path + ".log";
  const QString incomplete_exit_log_guard = incomplete_exit_log + ".hstream-pin";
  QFile::remove(incomplete_exit_path);
  QFile::remove(incomplete_exit_log);
  QFile::remove(incomplete_exit_log_guard);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", incomplete_exit_path.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_WRITE", "1");
  qputenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS", "9");
  activate(start);
  for (int i = 0; i < 400 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_WRITE");
  qunsetenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS");
  QFile incomplete_exit_log_file(incomplete_exit_log);
  const bool incomplete_exit_log_opened = incomplete_exit_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QByteArray incomplete_exit_log_content =
      incomplete_exit_log_opened ? incomplete_exit_log_file.readAll() : QByteArray();
  const bool incomplete_exit_log_guarded = expect(
      window->outputStateText("archive-file") == "INCOMPLETE" && QFileInfo(incomplete_exit_path).size() > 0 &&
          incomplete_exit_log_content.contains("pipeline finished exit=9") &&
          QFileInfo::exists(incomplete_exit_log_guard),
      "An unsuccessful run with a nonempty work video must retain the log identity guard for next-start recovery");

  bool same_filesystem_log_rollback = true;
#ifdef Q_OS_UNIX
  const auto guarded_same_filesystem_fallback = [&](const QString& basename,
                                                    const char* failure_environment,
                                                    bool expect_foreign_guard,
                                                    bool expect_foreign_log,
                                                    bool force_unsupported_rename = false,
                                                    bool force_rollback_failure = false) {
    const QString resolved_source = QDir(output_root.path()).filePath(basename + ".mkv");
    const QString resolved_log = resolved_source + ".log";
    const QString resolved_guard = resolved_log + ".hstream-pin";
    QDir provisional_log_dir(QFileInfo(planned_path).absolutePath());
    const QStringList provisional_logs_before =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    QFile::remove(resolved_log);
    QFile::remove(resolved_guard);
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", resolved_source.toLocal8Bit());
    qputenv(failure_environment, "1");
    if (QByteArray(failure_environment) == "HSTREAM_UI_TEST_RENAME_PATH_DESTINATION_REPLACEMENT")
      qputenv("HSTREAM_UI_TEST_RENAME_PATH_SOURCE_REPLACEMENT", "1");
    if (force_unsupported_rename)
      qputenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1");
    if (force_rollback_failure)
      qputenv("HSTREAM_UI_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE", resolved_log.toLocal8Bit());
    activate(start);
    for (int i = 0; i < 200 && window->pipelineStateText() != "RUNNING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const QStringList provisional_logs_running =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    QString active_provisional_guard;
    struct stat active_provisional_guard_stat{};
    bool active_provisional_guard_pinned = false;
    for (const QString& provisional_log : provisional_logs_running) {
      if (provisional_logs_before.contains(provisional_log))
        continue;
      active_provisional_guard = provisional_log_dir.filePath(provisional_log + ".hstream-pin");
      active_provisional_guard_pinned =
          ::lstat(QFile::encodeName(active_provisional_guard).constData(), &active_provisional_guard_stat) == 0;
      break;
    }
    activate(stop);
    for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv(failure_environment);
    qunsetenv("HSTREAM_UI_TEST_RENAME_PATH_SOURCE_REPLACEMENT");
    qunsetenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
    qunsetenv("HSTREAM_UI_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE");
    const QStringList provisional_logs_after =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    bool fallback_log_retained = false;
    bool foreign_source_retained = false;
    for (const QString& provisional_log : provisional_logs_after) {
      if (provisional_logs_before.contains(provisional_log))
        continue;
      QFile fallback_log(provisional_log_dir.filePath(provisional_log));
      if (fallback_log.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString fallback_log_text = QString::fromUtf8(fallback_log.readAll());
        fallback_log_retained |=
            fallback_log_text.contains(QString("archive backend resolved output: %1").arg(resolved_source)) &&
            fallback_log_text.contains("pipeline finished");
        foreign_source_retained |= fallback_log_text == "injected foreign provisional log before source retirement";
      }
    }
    const bool source_replacement =
        QByteArray(failure_environment) == "HSTREAM_UI_TEST_RENAME_PATH_SOURCE_REPLACEMENT" ||
        QByteArray(failure_environment) == "HSTREAM_UI_TEST_RENAME_PATH_DESTINATION_REPLACEMENT";
    if (source_replacement) {
      for (const QString& provisional_guard :
           provisional_log_dir.entryList({"*.log.hstream-pin"}, QDir::Files, QDir::Name)) {
        QFile fallback_guard(provisional_log_dir.filePath(provisional_guard));
        if (!fallback_guard.open(QIODevice::ReadOnly | QIODevice::Text))
          continue;
        const QString fallback_guard_text = QString::fromUtf8(fallback_guard.readAll());
        fallback_log_retained |=
            fallback_guard_text.contains(QString("archive backend resolved output: %1").arg(resolved_source));
      }
    }
    QFile guard_file(resolved_guard);
    const bool guard_opened = guard_file.open(QIODevice::ReadOnly);
    const QByteArray guard_text = guard_opened ? guard_file.readAll() : QByteArray();
    struct stat resolved_guard_stat{};
    const bool trusted_resolved_guard_retained = active_provisional_guard_pinned &&
        ::lstat(QFile::encodeName(resolved_guard).constData(), &resolved_guard_stat) == 0 &&
        resolved_guard_stat.st_dev == active_provisional_guard_stat.st_dev &&
        resolved_guard_stat.st_ino == active_provisional_guard_stat.st_ino;
    QFile resolved_log_file(resolved_log);
    const bool resolved_log_opened = resolved_log_file.open(QIODevice::ReadOnly);
    const QByteArray resolved_log_text = resolved_log_opened ? resolved_log_file.readAll() : QByteArray();
    const bool cleanup_transactions_absent =
        QDir(output_root.path())
            .entryList({".hstream-cleanup-v2-*"}, QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
            .isEmpty();
    struct stat retained_provisional_guard_stat{};
    const bool trusted_source_retained = active_provisional_guard_pinned &&
        ::lstat(QFile::encodeName(active_provisional_guard).constData(), &retained_provisional_guard_stat) == 0 &&
        retained_provisional_guard_stat.st_dev == active_provisional_guard_stat.st_dev &&
        retained_provisional_guard_stat.st_ino == active_provisional_guard_stat.st_ino;
    const bool destination_replacement =
        QByteArray(failure_environment) == "HSTREAM_UI_TEST_RENAME_PATH_DESTINATION_REPLACEMENT";
    const bool result = (expect_foreign_log ? resolved_log_text == "injected foreign resolved log after rename"
                                            : !QFileInfo::exists(resolved_log)) &&
        (source_replacement ? (foreign_source_retained && trusted_source_retained) : fallback_log_retained) &&
        (expect_foreign_guard
             ? guard_text == "injected foreign resolved log guard"
             : (destination_replacement ? trusted_resolved_guard_retained : !QFileInfo::exists(resolved_guard))) &&
        (!force_rollback_failure || cleanup_transactions_absent);
    if (!result) {
      std::cerr << "same-filesystem fallback failed for " << basename.toStdString()
                << " resolved-log-exists=" << QFileInfo::exists(resolved_log)
                << " resolved-guard-exists=" << QFileInfo::exists(resolved_guard)
                << " cleanup-transactions-absent=" << cleanup_transactions_absent
                << " fallback-log-retained=" << fallback_log_retained
                << " foreign-source-retained=" << foreign_source_retained
                << " trusted-source-retained=" << trusted_source_retained << '\n';
    }
    if (source_replacement) {
      QFile::remove(active_provisional_guard);
      for (const QString& provisional_log : provisional_logs_after) {
        if (provisional_logs_before.contains(provisional_log))
          continue;
        QFile provisional_file(provisional_log_dir.filePath(provisional_log));
        if (provisional_file.open(QIODevice::ReadOnly) &&
            provisional_file.readAll() == "injected foreign provisional log before source retirement") {
          provisional_file.close();
          QFile::remove(provisional_file.fileName());
        }
      }
    }
    QFile::remove(resolved_log);
    QFile::remove(resolved_guard);
    return result;
  };
  same_filesystem_log_rollback = expect(
      guarded_same_filesystem_fallback(
          "same-filesystem-guard-collision", "HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_COLLISION", true, false) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-guard-sync-failure",
              "HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_GUARD_SYNC_FAILURE",
              false,
              false) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-sync-failure", "HSTREAM_UI_TEST_ARCHIVE_SAME_FILESYSTEM_SYNC_FAILURE", false, false) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-post-rename-replacement",
              "HSTREAM_UI_TEST_ARCHIVE_RESOLVED_LOG_REPLACEMENT_AFTER_RENAME",
              false,
              true) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-nfs-destination-replacement",
              "HSTREAM_UI_TEST_RENAME_PATH_DESTINATION_REPLACEMENT",
              false,
              true,
              true) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-nfs-unlink-failure",
              "HSTREAM_UI_TEST_RENAME_PATH_SOURCE_UNLINK_FAILURE",
              false,
              false,
              true) &&
          guarded_same_filesystem_fallback(
              "same-filesystem-nfs-rollback-failure",
              "HSTREAM_UI_TEST_RENAME_PATH_SOURCE_UNLINK_FAILURE",
              false,
              false,
              true,
              true),
      "Same-filesystem log publication must retain the guarded provisional log across guard, sync, and rename races");
#endif

  bool cross_filesystem_log_persisted = true;
#ifdef Q_OS_UNIX
  QTemporaryDir cross_filesystem_root("/dev/shm/hstream-ui-cross-filesystem-XXXXXX");
  struct stat output_root_stat{};
  struct stat cross_root_stat{};
  const QByteArray encoded_output_root = QFile::encodeName(output_root.path());
  const QByteArray encoded_cross_root = QFile::encodeName(cross_filesystem_root.path());
  const bool distinct_cross_filesystem = cross_filesystem_root.isValid() &&
      ::stat(encoded_output_root.constData(), &output_root_stat) == 0 &&
      ::stat(encoded_cross_root.constData(), &cross_root_stat) == 0 &&
      output_root_stat.st_dev != cross_root_stat.st_dev;
  if (distinct_cross_filesystem) {
    const QString cross_filesystem_source = QDir(cross_filesystem_root.path()).filePath("cross-filesystem-output.mkv");
    const QString cross_filesystem_log = cross_filesystem_source + ".log";
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", cross_filesystem_source.toLocal8Bit());
    activate(start);
    for (int i = 0; i < 200 && window->pipelineStateText() != "RUNNING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    QFile cross_log_file(cross_filesystem_log);
    const bool cross_log_opened = cross_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString cross_log_text = cross_log_opened ? QString::fromUtf8(cross_log_file.readAll()) : QString();
    cross_filesystem_log_persisted = expect(
        cross_log_opened &&
            cross_log_text.contains(QString("archive backend resolved output: %1").arg(cross_filesystem_source)) &&
            cross_log_text.contains("pipeline finished"),
        "A backend-resolved archive on another filesystem must receive a secure copied-and-continued UI log");

    const QString cross_filesystem_sync_failure_source =
        QDir(cross_filesystem_root.path()).filePath("cross-filesystem-sync-failure.mkv");
    const QString cross_filesystem_sync_failure_log = cross_filesystem_sync_failure_source + ".log";
    QDir provisional_log_dir(QFileInfo(planned_path).absolutePath());
    const QStringList provisional_logs_before =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", cross_filesystem_sync_failure_source.toLocal8Bit());
    qputenv("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_SYNC_FAILURE", "1");
    activate(start);
    for (int i = 0; i < 200 && window->pipelineStateText() != "RUNNING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const QStringList provisional_logs_after =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    QString fallback_log_text;
    for (const QString& provisional_log : provisional_logs_after) {
      if (provisional_logs_before.contains(provisional_log))
        continue;
      QFile fallback_log(provisional_log_dir.filePath(provisional_log));
      if (fallback_log.open(QIODevice::ReadOnly | QIODevice::Text))
        fallback_log_text = QString::fromUtf8(fallback_log.readAll());
    }
    cross_filesystem_log_persisted &= expect(
        !QFileInfo::exists(cross_filesystem_sync_failure_log) &&
            fallback_log_text.contains(
                QString("archive backend resolved output: %1").arg(cross_filesystem_sync_failure_source)) &&
            fallback_log_text.contains("pipeline finished"),
        "A cross-filesystem destination-sync failure must retain and continue the durable provisional UI log");

    const QString cross_filesystem_copy_replacement_source =
        QDir(cross_filesystem_root.path()).filePath("cross-filesystem-copy-replacement.mkv");
    const QString cross_filesystem_copy_replacement_log = cross_filesystem_copy_replacement_source + ".log";
    const QStringList replacement_provisional_logs_before =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", cross_filesystem_copy_replacement_source.toLocal8Bit());
    qputenv("HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_COPY_REPLACEMENT", "1");
    activate(start);
    for (int i = 0; i < 200 && window->pipelineStateText() != "RUNNING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const QStringList replacement_provisional_logs_after =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    QString replacement_fallback_log_text;
    for (const QString& provisional_log : replacement_provisional_logs_after) {
      if (replacement_provisional_logs_before.contains(provisional_log))
        continue;
      QFile fallback_log(provisional_log_dir.filePath(provisional_log));
      if (fallback_log.open(QIODevice::ReadOnly | QIODevice::Text))
        replacement_fallback_log_text = QString::fromUtf8(fallback_log.readAll());
    }
    QFile foreign_cross_filesystem_log(cross_filesystem_copy_replacement_log);
    const bool foreign_cross_filesystem_log_opened = foreign_cross_filesystem_log.open(QIODevice::ReadOnly);
    const QByteArray foreign_cross_filesystem_log_text =
        foreign_cross_filesystem_log_opened ? foreign_cross_filesystem_log.readAll() : QByteArray();
    cross_filesystem_log_persisted &= expect(
        foreign_cross_filesystem_log_text == "injected foreign cross-filesystem log" &&
            replacement_fallback_log_text.contains(
                QString("archive backend resolved output: %1").arg(cross_filesystem_copy_replacement_source)) &&
            replacement_fallback_log_text.contains("pipeline finished"),
        "Cross-filesystem copy error cleanup must preserve a replacement pathname and continue the pinned provisional log");

    const QString cross_filesystem_reopen_failure_source =
        QDir(cross_filesystem_root.path()).filePath("cross-filesystem-reopen-failure.mkv");
    const QString cross_filesystem_reopen_failure_log = cross_filesystem_reopen_failure_source + ".log";
    const QStringList reopen_provisional_logs_before =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", cross_filesystem_reopen_failure_source.toLocal8Bit());
    qputenv(
        "HSTREAM_UI_TEST_ARCHIVE_CROSS_FILESYSTEM_REOPEN_FAILURE", cross_filesystem_reopen_failure_log.toLocal8Bit());
    activate(start);
    for (int i = 0; i < 200 && window->pipelineStateText() != "RUNNING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const QStringList reopen_provisional_logs_after =
        provisional_log_dir.entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
    QString reopen_fallback_log_text;
    for (const QString& provisional_log : reopen_provisional_logs_after) {
      if (reopen_provisional_logs_before.contains(provisional_log))
        continue;
      QFile fallback_log(provisional_log_dir.filePath(provisional_log));
      if (fallback_log.open(QIODevice::ReadOnly | QIODevice::Text))
        reopen_fallback_log_text = QString::fromUtf8(fallback_log.readAll());
    }
    cross_filesystem_log_persisted &= expect(
        !QFileInfo::exists(cross_filesystem_reopen_failure_log) &&
            reopen_fallback_log_text.contains(
                QString("archive backend resolved output: %1").arg(cross_filesystem_reopen_failure_source)) &&
            reopen_fallback_log_text.contains("pipeline finished"),
        "A cross-filesystem copied-log reopen failure must fall back to the original pinned identity and keep logging");
  }
#endif

  const QString completed_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("completed-source.mkv");
  const QString completed_job_log = completed_source + ".log";
  const QString concurrent_completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio.mp4").arg(window->gameIdText()));
  const QString completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio-2.mp4").arg(window->gameIdText()));
  const QString replaced_completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio-4.mp4").arg(window->gameIdText()));
  const QString dangling_completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio-1.mp4").arg(window->gameIdText()));
  const QString missing_completed_target = QDir(window->gameDirectoryText()).filePath("missing-completed-target");
  const QString existing_telemetry_tracking = QDir(window->gameDirectoryText()).filePath("tracking-3.csv");
  const QString telemetry_working = QDir(output_root.path()).filePath("telemetry-working");
  const std::array<QString, 6> telemetry_stems = {
      "tracking", "detections", "camera", "camera_fast", "hstream_frame_index", "hstream_config_events"};
  const QString telemetry_manifest = QDir(telemetry_working).filePath("hstream_telemetry-7.json");
  bool telemetry_fixture_created = QDir().mkpath(telemetry_working);
  for (const QString& stem : telemetry_stems) {
    QFile artifact(QDir(telemetry_working).filePath(stem + "-7.csv"));
    telemetry_fixture_created &= artifact.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
        artifact.write((stem + " async contents\n").toUtf8()) > 0;
  }
  QFile telemetry_manifest_file(telemetry_manifest);
  telemetry_fixture_created &=
      telemetry_manifest_file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && telemetry_manifest_file.write(R"json({
  "publication_state": "committed",
  "completed": true,
  "hm_compatibility": {
    "tracking_csv": {"file": "tracking-7.csv"},
    "detections_csv": {"file": "detections-7.csv"},
    "camera_csv": {"file": "camera-7.csv"},
    "camera_fast_csv": {"file": "camera_fast-7.csv"}
  },
  "sidecars": {
    "frame_index": "hstream_frame_index-7.csv",
    "config_events": "hstream_config_events-7.csv"
  }
})json") > 0;
  telemetry_manifest_file.close();
  if (!expect(telemetry_fixture_created, "asynchronous telemetry publication fixture must be created"))
    return false;
  const QString ffmpeg_arguments = QDir(output_root.path()).filePath("ffmpeg-arguments.txt");
  QFile::remove(completed_source);
  QFile::remove(completed_job_log);
  QFile::remove(concurrent_completed_target);
  QFile::remove(completed_target);
  QFile::remove(replaced_completed_target);
  QFile::remove(dangling_completed_target);
  QFile::remove(missing_completed_target);
  QFile::remove(existing_telemetry_tracking);
  const bool dangling_completed_target_created = QFile::link(missing_completed_target, dangling_completed_target);
  QFile existing_telemetry_tracking_file(existing_telemetry_tracking);
  const bool existing_telemetry_tracking_created =
      existing_telemetry_tracking_file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
      existing_telemetry_tracking_file.write("existing HM generation\n") > 0;
  existing_telemetry_tracking_file.close();
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", completed_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_WRITE", "1");
  qputenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS", "0");
  qputenv("HSTREAM_UI_TEST_FFMPEG_ARGS", ffmpeg_arguments.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_SYNC_DELAY", "0.25");
  qputenv("HSTREAM_UI_TEST_FFMPEG_SOURCE_REPLACEMENT", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_TARGET_REPLACEMENT_DURING_SYNC", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_OWNER_LOCK_REPLACEMENT", "1");
  qputenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST", telemetry_manifest.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS", "250");
  drivegpt_csv->setChecked(true);
  activate(start);
  QDialog* finalize_dialog = nullptr;
  QProgressBar* finalize_progress = nullptr;
  QLabel* finalize_headline = nullptr;
  for (int i = 0; i < 300 && window->outputStateText("archive-file") != "FINALIZING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString finalizer_owner_lock_path = completed_source + ".hstream-owner-lock";
  bool finalizer_owner_lock_held = false;
#ifdef Q_OS_UNIX
  const QByteArray encoded_owner_lock_path = QFile::encodeName(finalizer_owner_lock_path);
  const int owner_lock_probe = ::open(encoded_owner_lock_path.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (owner_lock_probe >= 0) {
    finalizer_owner_lock_held =
        ::flock(owner_lock_probe, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    ::close(owner_lock_probe);
  }
#endif
  QFile concurrent_completed_archive(concurrent_completed_target);
  const bool concurrent_completed_archive_created =
      concurrent_completed_archive.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
      concurrent_completed_archive.write("another run published this target") > 0;
  concurrent_completed_archive.close();
  finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  finalize_progress = window->findChild<QProgressBar*>("archiveFinalizeProgress");
  finalize_headline = window->findChild<QLabel*>("archiveFinalizeHeadline");
  for (int i = 0; i < 100 && finalize_progress && finalize_progress->value() <= 0; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool finalization_visible = expect(
      finalize_dialog && finalize_progress && finalize_dialog->isVisible() &&
          finalize_dialog->windowModality() == Qt::WindowModal && finalize_progress->value() > 0,
      "A successful pipeline archive must show app-modal lossless finalization progress");
  for (int i = 0; i < 300 && finalize_headline && finalize_headline->text() != "Saving completed video safely…"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  bool ui_timer_fired_during_sync = false;
  QTimer::singleShot(0, window, [&ui_timer_fired_during_sync]() { ui_timer_fired_during_sync = true; });
  QApplication::processEvents();
  const bool durability_sync_responsive = expect(
      finalize_headline && finalize_headline->text() == "Saving completed video safely…" && finalize_progress &&
          finalize_progress->maximum() == 0 && ui_timer_fired_during_sync,
      "Durability sync must keep an indeterminate finalization popup active without blocking the Qt event loop");
  for (int i = 0; i < 300 && finalize_headline && finalize_headline->text() != "Copying DriveGPT CSVs…"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  bool ui_timer_fired_during_telemetry_copy = false;
  QTimer::singleShot(
      0, window, [&ui_timer_fired_during_telemetry_copy]() { ui_timer_fired_during_telemetry_copy = true; });
  QApplication::processEvents();
  const bool telemetry_copy_responsive = expect(
      finalize_headline && finalize_headline->text() == "Copying DriveGPT CSVs…" && finalize_progress &&
          finalize_progress->maximum() == 0 && ui_timer_fired_during_telemetry_copy &&
          window->outputStateText("archive-file") == "FINALIZING",
      "DriveGPT CSV publication must remain asynchronous and keep the Qt event loop responsive");
  for (int i = 0; i < 300 && window->outputStateText("archive-file") != "SAVED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_SOURCE_REPLACEMENT");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_TARGET_REPLACEMENT_DURING_SYNC");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_OWNER_LOCK_REPLACEMENT");
  qunsetenv("HSTREAM_UI_TEST_SYNC_DELAY");
  qunsetenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST");
  qunsetenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS");
  drivegpt_csv->setChecked(false);
  QFile argument_file(ffmpeg_arguments);
  const bool opened_arguments = argument_file.open(QIODevice::ReadOnly);
  const QString argument_text = opened_arguments ? QString::fromUtf8(argument_file.readAll()) : QString();
  QFile completed_log_file(completed_job_log);
  const bool completed_log_opened = completed_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString completed_log_text = completed_log_opened ? QString::fromUtf8(completed_log_file.readAll()) : QString();
  QFile completed_video_file(replaced_completed_target);
  const bool completed_video_opened = completed_video_file.open(QIODevice::ReadOnly);
  const QByteArray completed_video_text = completed_video_opened ? completed_video_file.readAll() : QByteArray();
  QFile foreign_source_file(completed_source);
  const bool foreign_source_opened = foreign_source_file.open(QIODevice::ReadOnly);
  const QByteArray foreign_source_text = foreign_source_opened ? foreign_source_file.readAll() : QByteArray();
  QFile replaced_target_file(completed_target);
  const bool replaced_target_opened = replaced_target_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_target_text = replaced_target_opened ? replaced_target_file.readAll() : QByteArray();
  QFile replaced_owner_lock_file(finalizer_owner_lock_path);
  const bool replaced_owner_lock_opened = replaced_owner_lock_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_owner_lock_text =
      replaced_owner_lock_opened ? replaced_owner_lock_file.readAll() : QByteArray();
  const bool completed_log_persisted = expect(
      completed_log_opened && !QFileInfo::exists(completed_job_log + ".hstream-pin") &&
          completed_log_text.contains(QString("finalizing archive without re-encoding: %1").arg(completed_source)) &&
          completed_log_text.contains(QString("completed archive published: %1").arg(replaced_completed_target)),
      "A completed job log must remain beside the work artifacts and include asynchronous MP4 finalization output");
  const bool archive_deployed = expect(
      finalizer_owner_lock_held && concurrent_completed_archive_created && dangling_completed_target_created &&
          existing_telemetry_tracking_created && QFileInfo(dangling_completed_target).isSymLink() &&
          window->outputStateText("archive-file") == "SAVED" && completed_video_text == "completed lossless archive" &&
          !QFileInfo::exists(replaced_completed_target + ".hstream-pin") &&
          replaced_target_text == "injected foreign completed target" &&
          foreign_source_text == "injected foreign source before ffmpeg input" &&
          replaced_owner_lock_text == "injected foreign owner lock" &&
          QFileInfo(concurrent_completed_target).size() > 0 && argument_text.contains("-n\n") &&
          !argument_text.contains("-y\n") &&
          argument_text.contains(QRegularExpression(R"(-i\n/proc/self/fd/[0-9]+\n)")) &&
          !argument_text.contains("/proc/self/fd/197") && !argument_text.contains("/proc/self/fd/198") &&
          argument_text.contains(
              QString("/.%1-tracking_output-with-audio.hstream-finalize-").arg(window->gameIdText())) &&
          argument_text.contains("-c\ncopy") && argument_text.contains("-movflags\n+faststart") &&
          argument_text.contains("-tag:v\nhvc1") &&
          window->logText().contains(QString("completed archive published: %1").arg(replaced_completed_target)),
      "Finalization must remux the pinned source FD, skip dangling names, republish a target replaced during sync, "
      "and leave replacement source, target, and ownership-lock paths untouched");
  bool telemetry_deployed = true;
  for (const QString& stem : telemetry_stems) {
    QFile published_file(QDir(window->gameDirectoryText()).filePath(stem + "-4.csv"));
    telemetry_deployed &=
        published_file.open(QIODevice::ReadOnly) && published_file.readAll() == (stem + " async contents\n").toUtf8();
  }
  QFile preserved_tracking(existing_telemetry_tracking);
  telemetry_deployed &=
      preserved_tracking.open(QIODevice::ReadOnly) && preserved_tracking.readAll() == "existing HM generation\n";
  telemetry_deployed = expect(
      telemetry_deployed,
      "archive suffix selection must skip existing HM CSVs and copy all six DriveGPT CSVs with the video suffix");

  for (int i = 0; i < 100 && finalize_dialog && finalize_dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  bool ui_cleanup_restart_setup = true;
  const QString interrupted_ui_target = QDir(window->gameDirectoryText()).filePath("interrupted-ui-target.mp4");
  const QString interrupted_ui_target_fallback = interrupted_ui_target + ".hstream-cleanup-pin";
  const QString interrupted_ui_target_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-11111111-2222-4333-8444-555555555555");
  const QString interrupted_ui_guard_target =
      QDir(window->gameDirectoryText()).filePath("interrupted-ui-guard-target.mp4");
  const QString interrupted_ui_guard = interrupted_ui_guard_target + ".hstream-pin";
  const QString interrupted_ui_guard_fallback = interrupted_ui_guard + ".hstream-cleanup-pin";
  const QString interrupted_ui_guard_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-66666666-7777-4888-8999-aaaaaaaaaaaa");
  const QString committed_ui_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-00000000-0000-4000-8000-000000000000");
  const QString live_ui_target = QDir(window->gameDirectoryText()).filePath("live-ui-cleanup-target.mp4");
  const QString live_ui_fallback = live_ui_target + ".hstream-cleanup-pin";
  const QString live_ui_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-00000000-0000-4000-8000-000000000001");
  const QString unrelated_ui_cleanup_file = QDir(window->gameDirectoryText()).filePath("notes.hstream-cleanup-pin");
  const QString unrelated_ui_cleanup_directory =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-dddddddd-eeee-4fff-8aaa-bbbbbbbbbbbb");
  const QString unrelated_ui_cleanup_sibling_owner = unrelated_ui_cleanup_directory + ".hstream-owner";
  const QString reconciliation_race_target =
      QDir(window->gameDirectoryText()).filePath("interrupted-ui-reconciliation-race.mp4");
  const QString reconciliation_race_fallback = reconciliation_race_target + ".hstream-cleanup-pin";
  const QString reconciliation_race_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-01234567-89ab-4cde-8fab-0123456789ab");
#ifdef Q_OS_UNIX
  int live_ui_cleanup_fd = -1;
  const auto write_cleanup_test_file = [](const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
  };
  const auto create_hard_link = [](const QString& source, const QString& destination) {
    const QByteArray encoded_source = QFile::encodeName(source);
    const QByteArray encoded_destination = QFile::encodeName(destination);
    return ::link(encoded_source.constData(), encoded_destination.constData()) == 0;
  };
  const auto write_cleanup_owner = [&](const QString& cleanup_path, const QString& target_path) {
    const QByteArray target_name = QFile::encodeName(QFileInfo(target_path).fileName());
    return write_cleanup_test_file(
        QDir(cleanup_path).filePath("owner"), QByteArray("hstream-cleanup-v2\n") + target_name.toBase64());
  };
  const auto write_cleanup_commit = [&](const QString& cleanup_path, const QString& identity_path) {
    struct stat identity_stat{};
    if (::lstat(QFile::encodeName(identity_path).constData(), &identity_stat) != 0)
      return false;
    return write_cleanup_test_file(
        QDir(cleanup_path).filePath("committed"),
        QByteArray("hstream-cleanup-committed-v1\n") +
            QByteArray::number(static_cast<qulonglong>(identity_stat.st_dev)) + "\n" +
            QByteArray::number(static_cast<qulonglong>(identity_stat.st_ino)) + "\n");
  };
  ui_cleanup_restart_setup = write_cleanup_test_file(interrupted_ui_target, "trusted interrupted UI target") &&
      create_hard_link(interrupted_ui_target, interrupted_ui_target_fallback) &&
      QDir().mkpath(interrupted_ui_target_cleanup) &&
      write_cleanup_owner(interrupted_ui_target_cleanup, interrupted_ui_target) &&
      QFile::rename(interrupted_ui_target, QDir(interrupted_ui_target_cleanup).filePath("entry")) &&
      write_cleanup_test_file(interrupted_ui_guard_target, "trusted interrupted UI target guard") &&
      create_hard_link(interrupted_ui_guard_target, interrupted_ui_guard) &&
      create_hard_link(interrupted_ui_guard_target, interrupted_ui_guard_fallback) &&
      QDir().mkpath(interrupted_ui_guard_cleanup) &&
      write_cleanup_owner(interrupted_ui_guard_cleanup, interrupted_ui_guard) &&
      QFile::rename(interrupted_ui_guard, QDir(interrupted_ui_guard_cleanup).filePath("entry")) &&
      QDir().mkpath(committed_ui_cleanup) &&
      write_cleanup_owner(committed_ui_cleanup, QDir(window->gameDirectoryText()).filePath("committed-deleted.mp4")) &&
      write_cleanup_test_file(QDir(committed_ui_cleanup).filePath("guard"), "committed UI cleanup inode") &&
      create_hard_link(QDir(committed_ui_cleanup).filePath("guard"), QDir(committed_ui_cleanup).filePath("fallback")) &&
      write_cleanup_commit(committed_ui_cleanup, QDir(committed_ui_cleanup).filePath("guard")) &&
      write_cleanup_test_file(live_ui_target, "trusted live UI cleanup target") &&
      create_hard_link(live_ui_target, live_ui_fallback) && QDir().mkpath(live_ui_cleanup) &&
      write_cleanup_owner(live_ui_cleanup, live_ui_target) &&
      QFile::rename(live_ui_target, QDir(live_ui_cleanup).filePath("entry")) &&
      write_cleanup_test_file(unrelated_ui_cleanup_file, "unrelated UI cleanup-looking notes") &&
      QDir().mkpath(unrelated_ui_cleanup_directory) &&
      write_cleanup_test_file(QDir(unrelated_ui_cleanup_directory).filePath("guard"),
                              "unrelated exact cleanup guard") &&
      create_hard_link(QDir(unrelated_ui_cleanup_directory).filePath("guard"),
                       QDir(unrelated_ui_cleanup_directory).filePath("fallback")) &&
      write_cleanup_test_file(unrelated_ui_cleanup_sibling_owner, "hstream-cleanup-v2\ndW5yZWxhdGVkLm1rdg==") &&
      write_cleanup_test_file(reconciliation_race_target, "trusted UI reconciliation-race target") &&
      create_hard_link(reconciliation_race_target, reconciliation_race_fallback) &&
      QDir().mkpath(reconciliation_race_cleanup) &&
      write_cleanup_owner(reconciliation_race_cleanup, reconciliation_race_target) &&
      QFile::rename(reconciliation_race_target, QDir(reconciliation_race_cleanup).filePath("entry"));
  if (ui_cleanup_restart_setup) {
    live_ui_cleanup_fd =
        ::open(QFile::encodeName(live_ui_cleanup).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ui_cleanup_restart_setup = live_ui_cleanup_fd >= 0 && ::flock(live_ui_cleanup_fd, LOCK_EX | LOCK_NB) == 0;
  }
#endif

  const QString reconciliation_trigger_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("reconciliation-trigger.mkv");
  QFile::remove(reconciliation_trigger_source);
  QFile::remove(reconciliation_trigger_source + ".log");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", reconciliation_trigger_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION", reconciliation_race_target.toLocal8Bit());
  activate(start);
  for (int i = 0; i < 400 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_REPLACE_PUBLIC_DURING_CLEANUP_RECONCILIATION");
  finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  auto* reconciliation_failure_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  QString reconciliation_rescue_path;
  QByteArray reconciliation_rescue_content;
  const QString reconciliation_guard_prefix = ".hstream-reconcile-";
  const QDir reconciliation_game_dir(window->gameDirectoryText());
  for (const QString& name : reconciliation_game_dir.entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
    if (!name.startsWith(reconciliation_guard_prefix) || name.endsWith(".hstream-cleanup-pin"))
      continue;
    reconciliation_rescue_path = reconciliation_game_dir.filePath(name);
    QFile rescue_file(reconciliation_rescue_path);
    if (rescue_file.open(QIODevice::ReadOnly))
      reconciliation_rescue_content = rescue_file.readAll();
  }
  QFile reconciliation_foreign_file(reconciliation_race_target);
  const bool reconciliation_foreign_opened = reconciliation_foreign_file.open(QIODevice::ReadOnly);
  const QByteArray reconciliation_foreign_content =
      reconciliation_foreign_opened ? reconciliation_foreign_file.readAll() : QByteArray();
  QFile unrelated_ui_cleanup_file_handle(unrelated_ui_cleanup_file);
  const bool unrelated_ui_cleanup_file_opened = unrelated_ui_cleanup_file_handle.open(QIODevice::ReadOnly);
  const QByteArray unrelated_ui_cleanup_content =
      unrelated_ui_cleanup_file_opened ? unrelated_ui_cleanup_file_handle.readAll() : QByteArray();
  const bool ui_cleanup_reconciliation_race_safe = expect(
      window->outputStateText("archive-file") == "ERROR" &&
          reconciliation_rescue_content == "trusted UI reconciliation-race target" &&
          reconciliation_foreign_content == "foreign public cleanup identity" &&
          QFileInfo::exists(reconciliation_race_cleanup) &&
          !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("entry")) &&
          !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("guard")) &&
          !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("fallback")) &&
          !QFileInfo::exists(committed_ui_cleanup) &&
          !QFileInfo::exists(QDir(committed_ui_cleanup).filePath("owner")) &&
          QFileInfo::exists(QDir(live_ui_cleanup).filePath("entry")) && QFileInfo::exists(live_ui_fallback) &&
          unrelated_ui_cleanup_content == "unrelated UI cleanup-looking notes" &&
          QFileInfo::exists(unrelated_ui_cleanup_directory) &&
          QFileInfo::exists(QDir(unrelated_ui_cleanup_directory).filePath("guard")) &&
          QFileInfo::exists(QDir(unrelated_ui_cleanup_directory).filePath("fallback")) &&
          QFileInfo::exists(unrelated_ui_cleanup_sibling_owner) &&
          !QFileInfo::exists(QDir(window->gameDirectoryText()).filePath("notes")),
      "UI cleanup reconciliation must keep its dedicated guard through private-link retirement and leave unrelated entries untouched");
  if (reconciliation_failure_ok)
    activate(reconciliation_failure_ok);
#ifdef Q_OS_UNIX
  if (live_ui_cleanup_fd >= 0)
    ::close(live_ui_cleanup_fd);
#endif
  reconciliation_foreign_file.close();
  QFile::remove(reconciliation_race_target);
  qputenv(
      "HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE",
      (reconciliation_rescue_path + ".hstream-cleanup-pin").toLocal8Bit());
  QString reconciliation_guard_retirement_error;
  const bool reconciliation_guard_retirement_interrupted = hm::ui_internal::reconcile_cleanup_directory_for_test(
      window->gameDirectoryText(), &reconciliation_guard_retirement_error);
  qunsetenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_QUARANTINE");
  const bool reconciliation_outer_owner_only = QFileInfo::exists(reconciliation_race_cleanup) &&
      QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("owner")) &&
      !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("entry")) &&
      !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("guard")) &&
      !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("fallback"));
  const bool reconciliation_guard_public_fallback =
      QFileInfo::exists(reconciliation_rescue_path + ".hstream-cleanup-pin");
  QString reconciliation_stable_pass_error;
  const bool reconciliation_stable_pass = hm::ui_internal::reconcile_cleanup_directory_for_test(
      window->gameDirectoryText(), &reconciliation_stable_pass_error);
  QFile reconciliation_restored_target(reconciliation_race_target);
  const bool reconciliation_rescue_restored = !reconciliation_guard_retirement_interrupted &&
      reconciliation_outer_owner_only && !reconciliation_guard_public_fallback && reconciliation_stable_pass &&
      reconciliation_restored_target.open(QIODevice::ReadOnly) &&
      reconciliation_restored_target.readAll() == "trusted UI reconciliation-race target" &&
      !QFileInfo::exists(reconciliation_race_cleanup) && !QFileInfo::exists(reconciliation_rescue_path) &&
      !QFileInfo::exists(reconciliation_rescue_path + ".hstream-cleanup-pin");
  QFile::remove(reconciliation_trigger_source);
  QFile::remove(reconciliation_trigger_source + ".log");
  archive->setChecked(false);
  archive->setChecked(true);

  const QString target_cleanup_race_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("target-cleanup-race.mkv");
  const QString target_cleanup_race_recovery = QDir(QFileInfo(target_cleanup_race_source).absolutePath())
                                                   .filePath("target-cleanup-race-finalization-failed.mkv");
  QFile::remove(target_cleanup_race_source);
  QFile::remove(target_cleanup_race_source + ".log");
  QFile::remove(target_cleanup_race_recovery);
  const QStringList finalized_before_target_cleanup_race =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", target_cleanup_race_source.toLocal8Bit());
  qputenv(
      "HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE", (target_cleanup_race_source + ".hstream-pin").toLocal8Bit());
  activate(start);
  for (int i = 0; i < 400 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
  finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  finalize_headline = window->findChild<QLabel*>("archiveFinalizeHeadline");
  auto* target_cleanup_race_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
  auto* target_cleanup_race_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  QFile target_cleanup_race_recovery_file(target_cleanup_race_recovery);
  const bool target_cleanup_race_recovery_opened = target_cleanup_race_recovery_file.open(QIODevice::ReadOnly);
  const QByteArray target_cleanup_race_recovery_text =
      target_cleanup_race_recovery_opened ? target_cleanup_race_recovery_file.readAll() : QByteArray();
  const QStringList finalized_after_target_cleanup_race =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  bool foreign_target_retained =
      finalized_after_target_cleanup_race.size() == finalized_before_target_cleanup_race.size() + 1;
  for (const QString& target_name : finalized_after_target_cleanup_race) {
    if (finalized_before_target_cleanup_race.contains(target_name))
      continue;
    QFile target_file(QDir(window->gameDirectoryText()).filePath(target_name));
    foreign_target_retained = foreign_target_retained && target_file.open(QIODevice::ReadOnly) &&
        target_file.readAll() == "injected foreign publication after quarantine";
  }
  const bool target_cleanup_race_recovered = expect(
      window->outputStateText("archive-file") == "ERROR" && finalize_dialog && finalize_dialog->isVisible() &&
          finalize_headline && finalize_headline->text() == "Video finalization failed" && target_cleanup_race_detail &&
          target_cleanup_race_detail->text().contains(target_cleanup_race_recovery) &&
          target_cleanup_race_recovery_text == "completed lossless archive" && foreign_target_retained,
      "A target replaced during source-guard retirement must fail into trusted MKV recovery and leave the foreign MP4 untouched");
  QFile interrupted_ui_target_file(interrupted_ui_target);
  const bool interrupted_ui_target_opened = interrupted_ui_target_file.open(QIODevice::ReadOnly);
  const QByteArray interrupted_ui_target_content =
      interrupted_ui_target_opened ? interrupted_ui_target_file.readAll() : QByteArray();
  QFile live_ui_target_file(live_ui_target);
  const bool live_ui_target_opened = live_ui_target_file.open(QIODevice::ReadOnly);
  const QByteArray live_ui_target_content = live_ui_target_opened ? live_ui_target_file.readAll() : QByteArray();
  bool ui_cleanup_restart_reconciled = true;
  ui_cleanup_restart_reconciled &= expect(ui_cleanup_restart_setup, "UI cleanup restart fixture must be created");
  ui_cleanup_restart_reconciled &=
      expect(reconciliation_rescue_restored, "The reconciliation race rescue must be restored for later tests");
  ui_cleanup_restart_reconciled &= expect(
      interrupted_ui_target_content == "trusted interrupted UI target",
      "A subsequent archive start must restore an interrupted UI target");
  ui_cleanup_restart_reconciled &= expect(
      !QFileInfo::exists(interrupted_ui_guard),
      "A subsequent archive start must finish retiring an interrupted UI target guard");
  ui_cleanup_restart_reconciled &= expect(
      !QFileInfo::exists(interrupted_ui_target_fallback) && !QFileInfo::exists(interrupted_ui_guard_fallback) &&
          !QFileInfo::exists(interrupted_ui_target_cleanup) && !QFileInfo::exists(interrupted_ui_guard_cleanup) &&
          !QFileInfo::exists(reconciliation_race_cleanup) &&
          !QFileInfo::exists(QDir(interrupted_ui_target_cleanup).filePath("owner")) &&
          !QFileInfo::exists(QDir(interrupted_ui_guard_cleanup).filePath("owner")) &&
          !QFileInfo::exists(QDir(reconciliation_race_cleanup).filePath("owner")) &&
          !QFileInfo::exists(reconciliation_rescue_path),
      "A subsequent archive start must retire interrupted UI cleanup artifacts");
  ui_cleanup_restart_reconciled &= expect(
      live_ui_target_content == "trusted live UI cleanup target" && !QFileInfo::exists(live_ui_cleanup) &&
          !QFileInfo::exists(QDir(live_ui_cleanup).filePath("owner")) && !QFileInfo::exists(live_ui_fallback),
      "UI cleanup reconciliation must skip a live locked transaction and resume it after ownership is released");
  if (target_cleanup_race_ok)
    activate(target_cleanup_race_ok);
  for (int i = 0; i < 100 && finalize_dialog && finalize_dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  const QString guard_sync_failure_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("guard-sync-failure.mkv");
  QFile::remove(guard_sync_failure_source);
  QFile::remove(guard_sync_failure_source + ".log");
  const QStringList finalized_before_guard_sync_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", guard_sync_failure_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_CLEANUP_PARENT_SYNC_FAILURE", "mp4-guard");
  activate(start);
  for (int i = 0; i < 400 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_CLEANUP_PARENT_SYNC_FAILURE");
  finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  auto* guard_sync_failure_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
  auto* guard_sync_failure_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  const QStringList finalized_after_guard_sync_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  QString guard_sync_failure_target;
  for (const QString& target_name : finalized_after_guard_sync_failure) {
    if (!finalized_before_guard_sync_failure.contains(target_name)) {
      guard_sync_failure_target = QDir(window->gameDirectoryText()).filePath(target_name);
      break;
    }
  }
  QFile guard_sync_failure_target_file(guard_sync_failure_target);
  const bool guard_sync_failure_target_opened = guard_sync_failure_target_file.open(QIODevice::ReadOnly);
  const QByteArray guard_sync_failure_target_text =
      guard_sync_failure_target_opened ? guard_sync_failure_target_file.readAll() : QByteArray();
  const bool cleanup_directory_sync_failure_safe = expect(
      window->outputStateText("archive-file") == "ERROR" && finalize_dialog && finalize_dialog->isVisible() &&
          guard_sync_failure_detail &&
          guard_sync_failure_detail->text().contains("identity guard could not be retired safely") &&
          guard_sync_failure_target_text == "completed lossless archive" &&
          QFileInfo::exists(guard_sync_failure_target + ".hstream-pin"),
      "A cleanup-directory sync failure must retain the trusted MP4 and report its restored identity guard");
  if (guard_sync_failure_ok)
    activate(guard_sync_failure_ok);
  const QString guard_sync_failure_manual = guard_sync_failure_target + ".manually-retained";
  QFile::remove(guard_sync_failure_manual);
  const bool guard_sync_failure_moved =
      !guard_sync_failure_target.isEmpty() && QFile::rename(guard_sync_failure_target, guard_sync_failure_manual);
  archive->setChecked(false);
  archive->setChecked(true);

  const QString source_sync_failure =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("source-sync-failure.mkv");
  const QString source_sync_failure_recovery =
      QDir(QFileInfo(source_sync_failure).absolutePath()).filePath("source-sync-failure-finalization-failed.mkv");
  QFile::remove(source_sync_failure);
  QFile::remove(source_sync_failure + ".log");
  QFile::remove(source_sync_failure_recovery);
  const QStringList finalized_before_source_sync_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", source_sync_failure.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_SOURCE_PARENT_SYNC_FAILURE", "1");
  activate(start);
  for (int i = 0; i < 400 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  finalize_headline = window->findChild<QLabel*>("archiveFinalizeHeadline");
  auto* source_sync_failure_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
  auto* source_sync_failure_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  QFile source_sync_failure_recovery_file(source_sync_failure_recovery);
  const bool source_sync_failure_recovery_opened = source_sync_failure_recovery_file.open(QIODevice::ReadOnly);
  const QByteArray source_sync_failure_recovery_text =
      source_sync_failure_recovery_opened ? source_sync_failure_recovery_file.readAll() : QByteArray();
  const QStringList finalized_after_source_sync_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  const bool source_cleanup_sync_failure_recovered = expect(
      window->outputStateText("archive-file") == "ERROR" && finalize_dialog && finalize_dialog->isVisible() &&
          finalize_headline && finalize_headline->text() == "Video finalization failed" && source_sync_failure_detail &&
          source_sync_failure_detail->text().contains(source_sync_failure_recovery) &&
          source_sync_failure_recovery_text == "completed lossless archive" &&
          finalized_after_source_sync_failure == finalized_before_source_sync_failure,
      "A source-parent sync failure must withdraw the MP4 and durably recover the pinned MKV instead of reporting success");
  if (source_sync_failure_ok)
    activate(source_sync_failure_ok);
  for (int i = 0; i < 100 && finalize_dialog && finalize_dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString failed_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText()))
          .filePath(
              "tracking_output-with-audio.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-"
              "aabbccddeeff.mkv");
  const QString dangling_log_video =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("tracking_output-with-audio-finalization-failed.mkv");
  const QString dangling_log_path = dangling_log_video + ".log";
  const QString injected_collision_video =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("tracking_output-with-audio-finalization-failed-1.mkv");
  const QString injected_collision_log = injected_collision_video + ".log";
  const QString replaced_log_video =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("tracking_output-with-audio-finalization-failed-2.mkv");
  const QString replaced_log_path = replaced_log_video + ".log";
  const QString failed_recovery =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("tracking_output-with-audio-finalization-failed-3.mkv");
  const QString failed_source_log = failed_source + ".log";
  const QString failed_recovery_log = failed_recovery + ".log";
  const QString failed_recovery_rescue = failed_recovery + ".hstream-rescue";
  const QString failed_recovery_log_rescue = failed_recovery_log + ".hstream-rescue";
  const QStringList finalized_before_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  QFile::remove(failed_source);
  QFile::remove(dangling_log_video);
  QFile::remove(dangling_log_path);
  QFile::remove(injected_collision_video);
  QFile::remove(injected_collision_log);
  QFile::remove(replaced_log_video);
  QFile::remove(replaced_log_path);
  QFile::remove(failed_recovery);
  QFile::remove(failed_source_log);
  QFile::remove(failed_recovery_log);
  QFile::remove(failed_recovery_rescue);
  QFile::remove(failed_recovery_log_rescue);
  const QString dangling_resolved_log_target =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("missing-resolved-log-target");
  QFile::remove(dangling_resolved_log_target);
  const bool dangling_resolved_log_created = QFile::link(dangling_resolved_log_target, failed_source_log);
  const bool dangling_log_created =
      QFile::link(QDir(QFileInfo(failed_source).absolutePath()).filePath("missing-log-target"), dangling_log_path);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", failed_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_FFMPEG_FAIL", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_VIDEO_COLLISION", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_LOG_REOPEN_FAIL", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_SOURCE_REPLACEMENT", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_OPEN_LOG_REPLACEMENT", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_FORCE_LOG_CLOSE_AND_REPLACE", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_REPLACEMENT_DURING_SYNC", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_GUARD_REPLACEMENT_DURING_SYNC", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT_DURING_SYNC", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_GUARD_REPLACEMENT_DURING_SYNC", "1");
  qputenv("HSTREAM_UI_TEST_SYNC_DELAY", "0.1");
  activate(start);
  for (int i = 0; i < 300 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  auto* finalize_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
  auto* finalize_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  const QStringList finalized_after_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  QFile failed_log_file(failed_recovery_log_rescue);
  const bool failed_log_opened = failed_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString failed_log_text = failed_log_opened ? QString::fromUtf8(failed_log_file.readAll()) : QString();
  QFile replaced_log_file(replaced_log_path);
  const bool replaced_log_opened = replaced_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString replaced_log_text = replaced_log_opened ? QString::fromUtf8(replaced_log_file.readAll()) : QString();
  QFile replaced_source_file(failed_source);
  const bool replaced_source_opened = replaced_source_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_source_text = replaced_source_opened ? replaced_source_file.readAll() : QByteArray();
  QFile replaced_recovery_file(failed_recovery);
  const bool replaced_recovery_opened = replaced_recovery_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_recovery_text = replaced_recovery_opened ? replaced_recovery_file.readAll() : QByteArray();
  QFile trusted_recovery_guard_file(failed_recovery + ".hstream-pin");
  const bool trusted_recovery_guard_opened = trusted_recovery_guard_file.open(QIODevice::ReadOnly);
  const QByteArray trusted_recovery_guard_text =
      trusted_recovery_guard_opened ? trusted_recovery_guard_file.readAll() : QByteArray();
  QFile replaced_recovery_log_file(failed_recovery_log);
  const bool replaced_recovery_log_opened = replaced_recovery_log_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_recovery_log_text =
      replaced_recovery_log_opened ? replaced_recovery_log_file.readAll() : QByteArray();
  QFile replaced_recovery_log_guard_file(failed_recovery_log + ".hstream-pin");
  const bool replaced_recovery_log_guard_opened = replaced_recovery_log_guard_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_recovery_log_guard_text =
      replaced_recovery_log_guard_opened ? replaced_recovery_log_guard_file.readAll() : QByteArray();
  QFile trusted_recovery_file(failed_recovery_rescue);
  const bool trusted_recovery_opened = trusted_recovery_file.open(QIODevice::ReadOnly);
  const QByteArray trusted_recovery_text = trusted_recovery_opened ? trusted_recovery_file.readAll() : QByteArray();
  QString replaced_open_log_text;
  const QStringList provisional_logs =
      QDir(QFileInfo(failed_source).absolutePath())
          .entryList({"tracking_output-with-audio.hstream-run-ui-*.mkv.log"}, QDir::Files, QDir::Name);
  for (const QString& provisional_log : provisional_logs) {
    QFile provisional_log_file(QDir(QFileInfo(failed_source).absolutePath()).filePath(provisional_log));
    if (provisional_log_file.open(QIODevice::ReadOnly)) {
      const QString contents = QString::fromUtf8(provisional_log_file.readAll());
      if (contents == "injected foreign open log pathname")
        replaced_open_log_text = contents;
    }
  }
  const bool failed_archive_retained = expect(
      dangling_resolved_log_created && QFileInfo(failed_source_log).isSymLink() &&
          !QFileInfo::exists(dangling_resolved_log_target) && dangling_log_created &&
          QFileInfo(dangling_log_path).isSymLink() && QFileInfo(injected_collision_video).size() > 0 &&
          !QFileInfo::exists(injected_collision_log) && !QFileInfo::exists(replaced_log_video) &&
          replaced_log_text == "injected recovery log replacement" &&
          window->outputStateText("archive-file") == "ERROR" && finalize_dialog && finalize_dialog->isVisible() &&
          finalize_headline && finalize_headline->text() == "Video finalization failed" &&
          finalize_headline->property("finalizationState").toString() == "failed" && finalize_detail &&
          finalize_detail->text().contains(failed_recovery_rescue) && finalize_ok && finalize_ok->isVisible() &&
          finalize_ok->toolTip().contains("Close the finalization result") &&
          finalize_ok->statusTip() == finalize_ok->toolTip() && replaced_source_opened &&
          replaced_source_text == "injected foreign archive source" &&
          replaced_open_log_text == "injected foreign open log pathname" &&
          replaced_recovery_text == "injected foreign recovery during sync" && trusted_recovery_guard_opened &&
          trusted_recovery_guard_text == "injected foreign recovery guard during sync" &&
          replaced_recovery_log_text == "injected foreign recovery log during sync" &&
          replaced_recovery_log_guard_text == "injected foreign recovery log guard during sync" &&
          trusted_recovery_opened && trusted_recovery_text == "completed lossless archive" &&
          !QFileInfo::exists(failed_source_log) && failed_log_opened &&
          failed_log_text.contains("archive finalization failed") && !failed_recovery.contains(".hstream-run-") &&
          finalized_after_failure == finalized_before_failure,
      "A failed remux must durably rescue the trusted recovery pair if both visible names and guards are replaced");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_VIDEO_COLLISION");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_LOG_REOPEN_FAIL");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_SOURCE_REPLACEMENT");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_OPEN_LOG_REPLACEMENT");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_FORCE_LOG_CLOSE_AND_REPLACE");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_REPLACEMENT_DURING_SYNC");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_GUARD_REPLACEMENT_DURING_SYNC");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_REPLACEMENT_DURING_SYNC");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_LOG_GUARD_REPLACEMENT_DURING_SYNC");
  qunsetenv("HSTREAM_UI_TEST_SYNC_DELAY");
  if (finalize_ok)
    activate(finalize_ok);

  const QString no_log_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText()))
          .filePath("no-log.hstream-run-v3-99999998-77777777-00112233-4455-6677-8899-aabbccddeeff.mkv");
  const QString no_log_replaced_marker =
      QDir(QFileInfo(no_log_source).absolutePath()).filePath("no-log-finalization-failed.mkv.log");
  const QString no_log_recovery =
      QDir(QFileInfo(no_log_source).absolutePath()).filePath("no-log-finalization-failed-1.mkv");
  QFile::remove(no_log_source);
  QFile::remove(no_log_source + ".log");
  QFile::remove(no_log_replaced_marker);
  QFile::remove(no_log_recovery);
  QFile::remove(no_log_recovery + ".log");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", no_log_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_DROP_LOG_BEFORE_RECOVERY", "1");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_MARKER_REPLACEMENT", "1");
  activate(start);
  for (int i = 0; i < 300 && (!finalize_detail || !finalize_detail->text().contains(no_log_recovery)); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QFile replaced_marker_file(no_log_replaced_marker);
  const bool replaced_marker_opened = replaced_marker_file.open(QIODevice::ReadOnly);
  const QByteArray replaced_marker_text = replaced_marker_opened ? replaced_marker_file.readAll() : QByteArray();
  const bool no_log_recovery_reserved = expect(
      replaced_marker_text == "injected recovery marker replacement" && QFileInfo(no_log_recovery).size() > 0 &&
          !QFileInfo::exists(no_log_recovery + ".log") && !QFileInfo::exists(no_log_source) && finalize_detail &&
          finalize_detail->text().contains(no_log_recovery),
      "Recovery without a UI log must atomically reserve the sidecar name, roll back a replaced reservation, and retry a clean basename");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_DROP_LOG_BEFORE_RECOVERY");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_MARKER_REPLACEMENT");
  if (finalize_ok) {
    activate(finalize_ok);
    QApplication::processEvents();
    QTest::qWait(20);
  }

  const QString cleanup_race_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText()))
          .filePath("cleanup-race.hstream-run-v3-99999997-66666666-00112233-4455-6677-8899-aabbccddeeff.mkv");
  const QString cleanup_race_replaced =
      QDir(QFileInfo(cleanup_race_source).absolutePath()).filePath("cleanup-race-finalization-failed.mkv");
  const QString cleanup_race_recovery =
      QDir(QFileInfo(cleanup_race_source).absolutePath()).filePath("cleanup-race-finalization-failed-1.mkv");
  QFile::remove(cleanup_race_source);
  QFile::remove(cleanup_race_source + ".log");
  QFile::remove(cleanup_race_replaced);
  QFile::remove(cleanup_race_replaced + ".log");
  QFile::remove(cleanup_race_recovery);
  QFile::remove(cleanup_race_recovery + ".log");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", cleanup_race_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE", cleanup_race_source.toLocal8Bit());
  activate(start);
  for (int i = 0; i < 300 &&
       (window->outputStateText("archive-file") != "ERROR" || !finalize_detail ||
        !finalize_detail->text().contains(cleanup_race_recovery));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QFile cleanup_race_replaced_file(cleanup_race_replaced);
  const bool cleanup_race_replaced_opened = cleanup_race_replaced_file.open(QIODevice::ReadOnly);
  const QByteArray cleanup_race_replaced_text =
      cleanup_race_replaced_opened ? cleanup_race_replaced_file.readAll() : QByteArray();
  QFile cleanup_race_log_file(cleanup_race_recovery + ".log");
  const bool cleanup_race_log_opened = cleanup_race_log_file.open(QIODevice::ReadOnly);
  const QString cleanup_race_log_text =
      cleanup_race_log_opened ? QString::fromUtf8(cleanup_race_log_file.readAll()) : QString();
  const bool post_quarantine_recovery_safe = expect(
      cleanup_race_replaced_text == "injected foreign publication after quarantine" &&
          !QFileInfo::exists(cleanup_race_replaced + ".log") && QFileInfo(cleanup_race_recovery).size() > 0 &&
          cleanup_race_log_text.contains("archive finalization failed") && !QFileInfo::exists(cleanup_race_source) &&
          finalize_detail && finalize_detail->text().contains(cleanup_race_recovery),
      "UI protected cleanup must restore its pinned source and retry the pair if publication is replaced after quarantine unlink");
  qunsetenv("HSTREAM_UI_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
  if (finalize_ok)
    activate(finalize_ok);

  const QString publication_sync_failure_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("recovery-publication-sync-failure.mkv");
  const QString publication_sync_failure_log = publication_sync_failure_source + ".log";
  const QString publication_sync_failure_recovery =
      QDir(QFileInfo(publication_sync_failure_source).absolutePath())
          .filePath("recovery-publication-sync-failure-finalization-failed.mkv");
  const QString publication_sync_failure_manual = publication_sync_failure_source + ".manually-retained";
  QFile::remove(publication_sync_failure_source);
  QFile::remove(publication_sync_failure_log);
  QFile::remove(publication_sync_failure_source + ".hstream-pin");
  QFile::remove(publication_sync_failure_log + ".hstream-pin");
  QFile::remove(publication_sync_failure_recovery);
  QFile::remove(publication_sync_failure_recovery + ".log");
  QFile::remove(publication_sync_failure_manual);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", publication_sync_failure_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_PUBLICATION_SYNC_FAILURE", "1");
  activate(start);
  for (int i = 0; i < 300 &&
       (window->outputStateText("archive-file") != "ERROR" || !finalize_detail ||
        !finalize_detail->text().contains("could not make the recovery pair durable"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QFile publication_sync_failure_log_file(publication_sync_failure_log);
  const bool publication_sync_failure_log_opened =
      publication_sync_failure_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString publication_sync_failure_log_text =
      publication_sync_failure_log_opened ? QString::fromUtf8(publication_sync_failure_log_file.readAll()) : QString();
  const bool recovery_publication_sync_failure_safe = expect(
      QFileInfo(publication_sync_failure_source).size() > 0 && publication_sync_failure_log_opened &&
          publication_sync_failure_log_text.contains("archive finalization failed") &&
          QFileInfo::exists(publication_sync_failure_source + ".hstream-pin") &&
          !QFileInfo::exists(publication_sync_failure_recovery) &&
          !QFileInfo::exists(publication_sync_failure_recovery + ".log") && finalize_detail &&
          finalize_detail->text().contains(publication_sync_failure_source) &&
          finalize_detail->text().contains("could not make the recovery pair durable"),
      "A recovery-publication sync failure must retain the original video and log before source cleanup");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVERY_PUBLICATION_SYNC_FAILURE");
  if (finalize_ok)
    activate(finalize_ok);
  const bool publication_sync_failure_moved =
      QFile::rename(publication_sync_failure_source, publication_sync_failure_manual);
  archive->setChecked(false);
  archive->setChecked(true);

  const QString blocked_directory =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("blocked-recovery");
  const QString blocked_source = QDir(blocked_directory).filePath("blocked-source.mkv");
  const QString manually_recovered = QDir(blocked_directory).filePath("blocked-source-manually-recovered.mkv");
  QDir().mkpath(blocked_directory);
  QFile::remove(blocked_source);
  QFile::remove(manually_recovered);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", blocked_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY", "1");
  activate(start);
  for (int i = 0; i < 300 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool unsafe_retry_blocked = expect(
      QFileInfo::exists(blocked_source) && start && !start->isEnabled() && finalize_detail &&
          finalize_detail->text().contains("Do not start another archive run"),
      "If recovery rename fails, archive Play must stay disabled while the sole retained MKV occupies its work path");
  QFile::setPermissions(blocked_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  const bool manually_moved = QFile::rename(blocked_source, manually_recovered);
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY");
  if (finalize_ok)
    activate(finalize_ok);
  archive->setChecked(false);
  archive->setChecked(true);
  const bool retry_unblocked_after_recovery = expect(
      manually_moved && start->isEnabled(),
      "Moving the retained MKV to safety must re-enable archive Play on the next route refresh");
  bool ui_cleanup_owner_scoped = true;
#ifdef Q_OS_UNIX
  const QString scoped_ui_target = QDir(window->gameDirectoryText()).filePath("scoped-ui-cleanup-target.mp4");
  const QString scoped_ui_unrelated = QDir(window->gameDirectoryText()).filePath("scoped-ui-unrelated-hardlink.mp4");
  const QString scoped_ui_cleanup =
      QDir(window->gameDirectoryText()).filePath(".hstream-cleanup-v2-bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
  const QString scoped_ui_entry = QDir(scoped_ui_cleanup).filePath("entry");
  const QString scoped_ui_owner = QDir(scoped_ui_cleanup).filePath("owner");
  QFile::remove(scoped_ui_target);
  QFile::remove(scoped_ui_target + ".hstream-cleanup-pin");
  QFile::remove(scoped_ui_unrelated);
  QDir().mkpath(scoped_ui_cleanup);
  const bool scoped_ui_setup = write_cleanup_test_file(scoped_ui_entry, "trusted scoped UI cleanup inode") &&
      create_hard_link(scoped_ui_entry, scoped_ui_unrelated) &&
      write_cleanup_owner(scoped_ui_cleanup, scoped_ui_target);
  activate(start);
  for (int i = 0; i < 400 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  QFile scoped_ui_unrelated_file(scoped_ui_unrelated);
  const bool scoped_ui_unrelated_opened = scoped_ui_unrelated_file.open(QIODevice::ReadOnly);
  const QByteArray scoped_ui_unrelated_content =
      scoped_ui_unrelated_opened ? scoped_ui_unrelated_file.readAll() : QByteArray();
  ui_cleanup_owner_scoped = expect(
      scoped_ui_setup && window->outputStateText("archive-file") == "ERROR" && !QFileInfo::exists(scoped_ui_target) &&
          !QFileInfo::exists(scoped_ui_target + ".hstream-cleanup-pin") && QFileInfo::exists(scoped_ui_entry) &&
          QFileInfo::exists(scoped_ui_owner) && scoped_ui_unrelated_content == "trusted scoped UI cleanup inode",
      "UI cleanup reconciliation must retain private evidence instead of claiming a hardlink outside its owner target");
  auto* scoped_ui_ok = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  if (scoped_ui_ok)
    activate(scoped_ui_ok);
  scoped_ui_unrelated_file.close();
  QFile::remove(scoped_ui_unrelated);
  QFile::remove(scoped_ui_entry);
  QFile::remove(scoped_ui_owner);
  QDir().rmdir(scoped_ui_cleanup);
#endif
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_FAIL");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_WRITE");
  qunsetenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS");
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_ARGS");
  archive->setChecked(false);
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH");
  if (original_output_root.isEmpty()) {
    qunsetenv("HM_OUTPUT_WORK_DIR");
  } else {
    qputenv("HM_OUTPUT_WORK_DIR", original_output_root);
  }
  return relative_override_resolved && calibration_archive_routed && dual_archive_routed && path_refreshes_with_game &&
      path_visible_before_start && path_prepared && nonlocal_seek_blocked && interrupted_archive_preserved &&
      missing_new_output_reported && job_log_persisted && incomplete_exit_log_guarded && same_filesystem_log_rollback &&
      cross_filesystem_log_persisted && finalization_visible && completed_log_persisted && archive_deployed &&
      durability_sync_responsive && telemetry_copy_responsive && telemetry_deployed && target_cleanup_race_recovered &&
      ui_cleanup_restart_reconciled && ui_cleanup_reconciliation_race_safe && source_cleanup_sync_failure_recovered &&
      cleanup_directory_sync_failure_safe && guard_sync_failure_moved && failed_archive_retained &&
      no_log_recovery_reserved && post_quarantine_recovery_safe && recovery_publication_sync_failure_safe &&
      publication_sync_failure_moved && unsafe_retry_blocked && retry_unblocked_after_recovery &&
      ui_cleanup_owner_scoped;
}

bool test_dual_archive_finalization(HStreamWindow* window) {
  auto* archive = require_child<QCheckBox>(window, "outputToggle_archive-file");
  auto* stitched_archive = require_child<QCheckBox>(window, "outputToggle_archive-stitched");
  auto* drivegpt_csv = require_child<QCheckBox>(window, "drivegptCsvCheck");
  auto* archive_path = require_child<QLabel>(window, "archiveOutputPath");
  auto* stitched_archive_path = require_child<QLabel>(window, "stitchedArchiveOutputPath");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  if (!archive || !stitched_archive || !drivegpt_csv || !archive_path || !stitched_archive_path || !start || !mode)
    return false;

  QTemporaryDir output_root;
  if (!output_root.isValid())
    return false;
  const QByteArray original_output_root = qgetenv("HM_OUTPUT_WORK_DIR");
  const QString program_source = QDir(output_root.path()).filePath("dual-program.mkv");
  const QString stitched_source = QDir(output_root.path()).filePath("dual-stitched.mkv");
  const QString combined_job_log = program_source + ".log";
  const QString telemetry_working = QDir(output_root.path()).filePath("dual-telemetry-working");
  const std::array<QString, 6> telemetry_stems = {
      "tracking", "detections", "camera", "camera_fast", "hstream_frame_index", "hstream_config_events"};
  const QString telemetry_manifest = QDir(telemetry_working).filePath("hstream_telemetry-11.json");
  bool telemetry_fixture_created = QDir().mkpath(telemetry_working);
  for (const QString& stem : telemetry_stems) {
    QFile artifact(QDir(telemetry_working).filePath(stem + "-11.csv"));
    telemetry_fixture_created &= artifact.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
        artifact.write((stem + " dual archive contents\n").toUtf8()) > 0;
  }
  QFile telemetry_manifest_file(telemetry_manifest);
  telemetry_fixture_created &=
      telemetry_manifest_file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && telemetry_manifest_file.write(R"json({
  "publication_state": "committed",
  "completed": true,
  "hm_compatibility": {
    "tracking_csv": {"file": "tracking-11.csv"},
    "detections_csv": {"file": "detections-11.csv"},
    "camera_csv": {"file": "camera-11.csv"},
    "camera_fast_csv": {"file": "camera_fast-11.csv"}
  },
  "sidecars": {
    "frame_index": "hstream_frame_index-11.csv",
    "config_events": "hstream_config_events-11.csv"
  }
})json") > 0;
  telemetry_manifest_file.close();
  if (!expect(telemetry_fixture_created, "dual archive telemetry fixture must be created"))
    return false;
  qputenv("HM_OUTPUT_WORK_DIR", output_root.path().toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", program_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH", stitched_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_WRITE", "1");
  qputenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS", "0");
  qputenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST", telemetry_manifest.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS", "100");
  mode->setCurrentIndex(mode->findData("program"));
  archive->setChecked(true);
  stitched_archive->setChecked(true);
  drivegpt_csv->setChecked(true);
  activate(start);
  for (int i = 0; i < 600 &&
       (window->outputStateText("archive-file") != "SAVED" || window->outputStateText("archive-stitched") != "SAVED");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }

  const QString program_completed = archive_path->text().section("Completed archive: ", 1).trimmed();
  const QString stitched_completed = stitched_archive_path->text().section("Completed archive: ", 1).trimmed();
  QFile program_file(program_completed);
  QFile stitched_file(stitched_completed);
  QFile combined_log_file(combined_job_log);
  const bool program_opened = program_file.open(QIODevice::ReadOnly);
  const bool stitched_opened = stitched_file.open(QIODevice::ReadOnly);
  const bool combined_log_opened = combined_log_file.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString combined_log_text = combined_log_opened ? QString::fromUtf8(combined_log_file.readAll()) : QString();
  const QString program_finalize_log = QString("finalizing archive without re-encoding: %1").arg(program_source);
  const QString stitched_finalize_log = QString("finalizing archive without re-encoding: %1").arg(stitched_source);
  const int program_finalize_index = window->logText().lastIndexOf(program_finalize_log);
  const int telemetry_finalize_index = window->logText().lastIndexOf("DriveGPT CSVs copied to the game directory");
  const int stitched_finalize_index = window->logText().lastIndexOf(stitched_finalize_log);
  const QString completed_base = QFileInfo(program_completed).completeBaseName();
  const QString unsuffixed_base = QString("%1-tracking_output-with-audio").arg(window->gameIdText());
  const QString telemetry_suffix = completed_base.startsWith(unsuffixed_base)
      ? completed_base.mid(unsuffixed_base.size())
      : QString("invalid");
  bool dual_telemetry_deployed = telemetry_suffix.isEmpty() ||
      QRegularExpression(R"(^-[1-9][0-9]*$)").match(telemetry_suffix).hasMatch();
  for (const QString& stem : telemetry_stems) {
    QFile published(QDir(window->gameDirectoryText()).filePath(stem + telemetry_suffix + ".csv"));
    dual_telemetry_deployed &= published.open(QIODevice::ReadOnly) &&
        published.readAll() == (stem + " dual archive contents\n").toUtf8();
  }
  const bool ok = expect(
      window->outputStateText("archive-file") == "SAVED" && window->outputStateText("archive-stitched") == "SAVED" &&
          program_opened && stitched_opened && program_file.readAll() == "completed lossless archive" &&
          stitched_file.readAll() == "completed stitched archive" &&
          QFileInfo(program_completed).completeBaseName().contains("-tracking_output-with-audio") &&
          QFileInfo(stitched_completed).completeBaseName().contains("-stitched_output-with-audio") &&
          !QFileInfo::exists(program_source) && !QFileInfo::exists(stitched_source) && program_finalize_index >= 0 &&
          telemetry_finalize_index > program_finalize_index && stitched_finalize_index > telemetry_finalize_index &&
          dual_telemetry_deployed && combined_log_opened &&
          combined_log_text.contains(program_finalize_log) && combined_log_text.contains(stitched_finalize_log) &&
          combined_log_text.contains("DriveGPT CSVs copied to the game directory") &&
          combined_log_text.contains(QString("completed archive published: %1").arg(program_completed)) &&
          combined_log_text.contains(QString("completed archive published: %1").arg(stitched_completed)),
      "A successful Program run with both archives must publish Program telemetry, finalize both work files "
      "sequentially with distinct names, and retain one complete run log");

  auto* finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  for (int i = 0; i < 100 && finalize_dialog && finalize_dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  program_file.close();
  stitched_file.close();
  combined_log_file.close();
  QFile::remove(program_completed);
  QFile::remove(stitched_completed);
  QFile::remove(combined_job_log);
  for (const QString& stem : telemetry_stems)
    QFile::remove(QDir(window->gameDirectoryText()).filePath(stem + telemetry_suffix + ".csv"));
  qunsetenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST");
  qunsetenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS");
  drivegpt_csv->setChecked(false);

  const bool telemetry_source_removed =
      QFile::remove(QDir(telemetry_working).filePath("camera_fast-11.csv"));
  const auto run_telemetry_publication_failure = [&](const QString& label, bool with_stitched_archive) {
    const QString failed_telemetry_program_source = QDir(output_root.path()).filePath(label + "-program.mkv");
    const QString failed_telemetry_stitched_source = QDir(output_root.path()).filePath(label + "-stitched.mkv");
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", failed_telemetry_program_source.toLocal8Bit());
    if (with_stitched_archive) {
      qputenv(
          "HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH", failed_telemetry_stitched_source.toLocal8Bit());
    } else {
      qunsetenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH");
    }
    qputenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST", telemetry_manifest.toLocal8Bit());
    qputenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS", "100");
    stitched_archive->setChecked(with_stitched_archive);
    drivegpt_csv->setChecked(true);
    activate(start);
    for (int i = 0; i < 600 &&
         (window->outputStateText("archive-file") != "SAVED" ||
          (with_stitched_archive && window->outputStateText("archive-stitched") != "SAVED"));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_TELEMETRY_MANIFEST");
    qunsetenv("HSTREAM_UI_TEST_TELEMETRY_PUBLICATION_DELAY_MS");
    drivegpt_csv->setChecked(false);
    const QString failed_telemetry_program_completed =
        archive_path->text().section("Completed archive: ", 1).trimmed();
    const QString failed_telemetry_stitched_completed = with_stitched_archive
        ? stitched_archive_path->text().section("Completed archive: ", 1).trimmed()
        : QString();
    auto* failed_telemetry_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
    auto* failed_telemetry_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
    auto* failed_telemetry_ok_button = window->findChild<QPushButton*>("archiveFinalizeOkButton");
    const QString telemetry_warning = "DriveGPT CSV publication failed:";
    const int telemetry_warning_index = window->logText().lastIndexOf("WARNING: completed DriveGPT CSVs");
    const int stitched_after_warning_index =
        window->logText().lastIndexOf(QString("finalizing archive without re-encoding: %1")
                                         .arg(failed_telemetry_stitched_source));
    const bool result = expect(
        window->outputStateText("archive-file") == "SAVED" &&
            (!with_stitched_archive || window->outputStateText("archive-stitched") == "SAVED") &&
            failed_telemetry_dialog && failed_telemetry_dialog->isVisible() && failed_telemetry_ok_button &&
            failed_telemetry_ok_button->isVisible() && failed_telemetry_detail &&
            failed_telemetry_detail->text().contains(telemetry_warning) &&
            failed_telemetry_detail->text().contains(telemetry_working) && telemetry_warning_index >= 0 &&
            (!with_stitched_archive || stitched_after_warning_index > telemetry_warning_index),
        QString("A %1 telemetry publication failure must preserve its working-storage warning for acknowledgement "
                "without blocking saved video outputs")
            .arg(with_stitched_archive ? "dual-archive" : "Program-only")
            .toStdString());
    QFile::remove(failed_telemetry_program_completed);
    QFile::remove(failed_telemetry_stitched_completed);
    QFile::remove(failed_telemetry_program_source + ".log");
    if (failed_telemetry_ok_button && failed_telemetry_ok_button->isVisible())
      activate(failed_telemetry_ok_button);
    return result;
  };
  const bool program_telemetry_failure_visible =
      telemetry_source_removed && run_telemetry_publication_failure("program-telemetry-failure", false);
  const bool dual_telemetry_failure_visible =
      run_telemetry_publication_failure("dual-telemetry-failure", true);

  const auto run_route_failure = [&](const QString& label, const QByteArray& fail_route, bool fail_program) {
    const QString failed_program_source = QDir(output_root.path()).filePath(label + "-program.mkv");
    const QString failed_stitched_source = QDir(output_root.path()).filePath(label + "-stitched.mkv");
    qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", failed_program_source.toLocal8Bit());
    qputenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH", failed_stitched_source.toLocal8Bit());
    qputenv("HSTREAM_UI_TEST_FFMPEG_FAIL_ROUTE", fail_route);
    activate(start);
    const QString failed_output = fail_program ? QString("archive-file") : QString("archive-stitched");
    const QString saved_output = fail_program ? QString("archive-stitched") : QString("archive-file");
    for (int i = 0; i < 600 &&
         (window->outputStateText(failed_output) != "ERROR" || window->outputStateText(saved_output) != "SAVED");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_FFMPEG_FAIL_ROUTE");
    const QString failed_source = fail_program ? failed_program_source : failed_stitched_source;
    const QString saved_source = fail_program ? failed_stitched_source : failed_program_source;
    const QString failed_recovery =
        QFileInfo(failed_source)
            .dir()
            .filePath(QFileInfo(failed_source).completeBaseName() + "-finalization-failed.mkv");
    const QString failed_log = failed_recovery + ".log";
    QLabel* failed_path_label = fail_program ? archive_path : stitched_archive_path;
    auto* finalize_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
    auto* ok_button = window->findChild<QPushButton*>("archiveFinalizeOkButton");
    auto* finalize_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
    QFile run_log(failed_log);
    const bool run_log_opened = run_log.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString run_log_text = run_log_opened ? QString::fromUtf8(run_log.readAll()) : QString();
    const bool route_result = expect(
        window->outputStateText(failed_output) == "ERROR" && window->outputStateText(saved_output) == "SAVED" &&
            QFileInfo::exists(failed_recovery) && !QFileInfo::exists(failed_source) &&
            !QFileInfo::exists(saved_source) && run_log_opened &&
            failed_path_label->text() == QString("Recovery archive: %1").arg(failed_recovery) &&
            finalize_dialog && finalize_dialog->isVisible() && ok_button && ok_button->isVisible() &&
            finalize_detail && finalize_detail->text().contains(failed_recovery) &&
            run_log_text.contains(QString("finalizing archive without re-encoding: %1").arg(failed_program_source)) &&
            run_log_text.contains(QString("finalizing archive without re-encoding: %1").arg(failed_stitched_source)) &&
            run_log_text.contains("archive finalization failed") &&
            run_log_text.contains("completed archive published"),
        QString(
            "A %1-route failure must retain its recovery pair while the other archive still finalizes and the "
            "combined log records both outcomes")
            .arg(fail_program ? "first" : "second")
            .toStdString());
    run_log.close();
    QFile::remove(failed_recovery + ".hstream-pin");
    QFile::remove(failed_log + ".hstream-pin");
    QFile::remove(failed_recovery);
    QFile::remove(failed_log);
    if (ok_button && ok_button->isVisible())
      activate(ok_button);
    return route_result;
  };

  const bool first_failure_safe = run_route_failure("dual-first-failure", "tracking_output", true);
  const bool second_failure_safe = run_route_failure("dual-second-failure", "stitched_output", false);

  const QString both_failed_program_source = QDir(output_root.path()).filePath("dual-both-fail-program.mkv");
  const QString both_failed_stitched_source = QDir(output_root.path()).filePath("dual-both-fail-stitched.mkv");
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", both_failed_program_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH", both_failed_stitched_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_FFMPEG_FAIL", "1");
  activate(start);
  for (int i = 0; i < 600 &&
       (window->outputStateText("archive-file") != "ERROR" ||
        window->outputStateText("archive-stitched") != "ERROR");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_FAIL");
  const QString both_failed_program_recovery =
      QDir(output_root.path()).filePath("dual-both-fail-program-finalization-failed.mkv");
  const QString both_failed_stitched_recovery =
      QDir(output_root.path()).filePath("dual-both-fail-stitched-finalization-failed.mkv");
  QFile both_failed_program_log(both_failed_program_recovery + ".log");
  QFile both_failed_stitched_log(both_failed_stitched_recovery + ".log");
  const bool both_failed_program_log_opened =
      both_failed_program_log.open(QIODevice::ReadOnly | QIODevice::Text);
  const bool both_failed_stitched_log_opened =
      both_failed_stitched_log.open(QIODevice::ReadOnly | QIODevice::Text);
  const QString both_failed_program_log_text =
      both_failed_program_log_opened ? QString::fromUtf8(both_failed_program_log.readAll()) : QString();
  const QString both_failed_stitched_log_text =
      both_failed_stitched_log_opened ? QString::fromUtf8(both_failed_stitched_log.readAll()) : QString();
  auto* both_failed_detail = window->findChild<QLabel*>("archiveFinalizeDetail");
  auto* both_failed_ok_button = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  auto* both_failed_dialog = window->findChild<QDialog*>("archiveFinalizeDialog");
  const bool both_failures_safe = expect(
      window->outputStateText("archive-file") == "ERROR" &&
          window->outputStateText("archive-stitched") == "ERROR" &&
          QFileInfo::exists(both_failed_program_recovery) && QFileInfo::exists(both_failed_stitched_recovery) &&
          both_failed_program_log_opened && both_failed_stitched_log_opened &&
          archive_path->text() == QString("Recovery archive: %1").arg(both_failed_program_recovery) &&
          stitched_archive_path->text() == QString("Recovery archive: %1").arg(both_failed_stitched_recovery) &&
          both_failed_dialog && both_failed_dialog->isVisible() && both_failed_ok_button &&
          both_failed_ok_button->isVisible() && both_failed_detail &&
          both_failed_detail->text().contains(both_failed_program_recovery) &&
          both_failed_detail->text().contains(both_failed_stitched_recovery) &&
          both_failed_program_log_text.contains(
              QString("finalizing archive without re-encoding: %1").arg(both_failed_program_source)) &&
          both_failed_program_log_text.contains(
              QString("finalizing archive without re-encoding: %1").arg(both_failed_stitched_source)) &&
          both_failed_stitched_log_text.contains(
              QString("finalizing archive without re-encoding: %1").arg(both_failed_program_source)) &&
          both_failed_stitched_log_text.contains(
              QString("finalizing archive without re-encoding: %1").arg(both_failed_stitched_source)),
      "When both archive routes fail, each retained recovery video must keep a durable link to the combined job log");
  both_failed_program_log.close();
  both_failed_stitched_log.close();
  for (const QString& recovery : {both_failed_program_recovery, both_failed_stitched_recovery}) {
    QFile::remove(recovery + ".hstream-pin");
    QFile::remove(recovery + ".log.hstream-pin");
    QFile::remove(recovery);
    QFile::remove(recovery + ".log");
  }
  if (both_failed_ok_button && both_failed_ok_button->isVisible())
    activate(both_failed_ok_button);

  const QString blocked_directory = QDir(output_root.path()).filePath("dual-blocked");
  const QString blocked_program_source = QDir(blocked_directory).filePath("program.mkv");
  const QString blocked_program_manual = QDir(blocked_directory).filePath("program-manually-retained.mkv");
  const QString resumed_stitched_source = QDir(output_root.path()).filePath("dual-blocked-stitched.mkv");
  QDir().mkpath(blocked_directory);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", blocked_program_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH", resumed_stitched_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_FFMPEG_FAIL_ROUTE", "tracking_output");
  qputenv("HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY", "1");
  activate(start);
  for (int i = 0; i < 600 && window->outputStateText("archive-file") != "ERROR"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString blocked_owner_lock = blocked_program_source + ".hstream-owner-lock";
  bool blocked_owner_lock_held = false;
#ifdef Q_OS_UNIX
  const int blocked_lock_probe =
      ::open(QFile::encodeName(blocked_owner_lock).constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (blocked_lock_probe >= 0) {
    blocked_owner_lock_held =
        ::flock(blocked_lock_probe, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    ::close(blocked_lock_probe);
  }
#endif
  const QString stitched_finalize_after_block =
      QString("finalizing archive without re-encoding: %1").arg(resumed_stitched_source);
  const bool queue_stayed_blocked = window->outputStateText("archive-file") == "ERROR" &&
      window->outputStateText("archive-stitched") == "FINALIZING" && QFileInfo::exists(blocked_program_source) &&
      !window->logText().contains(stitched_finalize_after_block) && blocked_owner_lock_held;
  window->close();
  QApplication::processEvents();
  const bool blocked_close_deferred = window->isVisible() &&
      window->logText().contains("window close deferred while a retained archive blocks pending finalization");
  QFile::setPermissions(blocked_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  const bool blocked_source_moved = QFile::rename(blocked_program_source, blocked_program_manual);
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_FAIL_ROUTE");
  qunsetenv("HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY");
  HStreamWindowTestAccess::refreshRunControls(window);
  for (int i = 0; i < 600 && window->outputStateText("archive-stitched") != "SAVED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString resumed_stitched_completed = stitched_archive_path->text().section("Completed archive: ", 1).trimmed();
  const bool blocked_recovery_resumed = expect(
      queue_stayed_blocked && blocked_close_deferred && blocked_source_moved &&
          window->outputStateText("archive-file") == "ERROR" &&
          window->outputStateText("archive-stitched") == "SAVED" &&
          window->logText().contains(stitched_finalize_after_block) && QFileInfo::exists(blocked_program_manual) &&
          !QFileInfo::exists(resumed_stitched_source),
      QString(
          "A blocked first finalization must keep its ownership lock and halt the second archive until the retained "
          "file is moved to safety, then resume the queue (queue_blocked=%1 close_deferred=%2 moved=%3 program=%4 "
          "stitched=%5 manual=%6 source=%7 log=%8)")
          .arg(queue_stayed_blocked)
          .arg(blocked_close_deferred)
          .arg(blocked_source_moved)
          .arg(window->outputStateText("archive-file"), window->outputStateText("archive-stitched"))
          .arg(QFileInfo::exists(blocked_program_manual))
          .arg(QFileInfo::exists(resumed_stitched_source))
          .arg(window->logText().contains(stitched_finalize_after_block))
          .toStdString());
  QFile::remove(resumed_stitched_completed);
  QFile::remove(blocked_program_source + ".hstream-pin");
  QFile::remove(blocked_program_source + ".log.hstream-pin");
  QFile::remove(blocked_program_source + ".log");
  QFile::remove(blocked_program_manual);
  QFile::remove(blocked_owner_lock);
  auto* blocked_ok_button = window->findChild<QPushButton*>("archiveFinalizeOkButton");
  if (blocked_ok_button && blocked_ok_button->isVisible())
    activate(blocked_ok_button);
  archive->setChecked(false);
  stitched_archive->setChecked(false);
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH");
  qunsetenv("HSTREAM_UI_TEST_STITCHED_ARCHIVE_RESOLVED_PATH");
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_WRITE");
  qunsetenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS");
  if (original_output_root.isEmpty())
    qunsetenv("HM_OUTPUT_WORK_DIR");
  else
    qputenv("HM_OUTPUT_WORK_DIR", original_output_root);
  return ok && program_telemetry_failure_visible && dual_telemetry_failure_visible && first_failure_safe &&
      second_failure_safe && both_failures_safe && blocked_recovery_resumed;
}

bool test_projection_parameter_persistence(HStreamWindow* window) {
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  auto* control_point_matcher = require_child<QComboBox>(window, "controlPointMatcherCombo");
  auto* mapping_backend = require_child<QComboBox>(window, "mappingBackendCombo");
  auto* projection = require_child<QComboBox>(window, "stitchProjectionCombo");
  auto* run_autooptimizer = require_child<QCheckBox>(window, "runAutooptimizerCheck");
  auto* compression = require_child<QDoubleSpinBox>(window, "generalPaniniCompressionSpin");
  auto* top_squeeze = require_child<QDoubleSpinBox>(window, "generalPaniniTopSqueezeSpin");
  auto* bottom_squeeze = require_child<QDoubleSpinBox>(window, "generalPaniniBottomSqueezeSpin");
  auto* auto_fov = require_child<QCheckBox>(window, "projectionAutoFovCheck");
  auto* horizontal_fov = require_child<QDoubleSpinBox>(window, "projectionHorizontalFovSpin");
  auto* auto_canvas = require_child<QCheckBox>(window, "projectionAutoCanvasCheck");
  auto* auto_crop = require_child<QCheckBox>(window, "projectionAutoCropCheck");
  if (!game_id || !create || !reset || !save || !control_point_matcher || !mapping_backend || !projection ||
      !run_autooptimizer || !compression || !top_squeeze || !bottom_squeeze || !auto_fov || !horizontal_fov ||
      !auto_canvas || !auto_crop) {
    return false;
  }
  const QString original_game_id = game_id->text();
  game_id->setText("ui-projection-parameters-game");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("general-panini"));
  compression->setValue(120.0);
  top_squeeze->setValue(15.0);
  bottom_squeeze->setValue(-20.0);
  QApplication::processEvents();
  if (!expect(save->isEnabled(), "Editing General Panini parameters must dirty the game preset"))
    return false;
  activate(save);
  const fs::path config_path = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const YAML::Node config = YAML::LoadFile(config_path.string());
  const YAML::Node parameters = config["stitching"]["projection_parameters"]["general-panini"];
  const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  const bool saved = expect(
      parameters.IsSequence() && parameters.size() == 3 && parameters[0].as<double>() == 120.0 &&
          parameters[1].as<double>() == 15.0 && parameters[2].as<double>() == -20.0 &&
          calibration["status"].as<std::string>() == "pending" &&
          calibration["stale_from"].as<std::string>() == "canvas" && !save->isEnabled(),
      "Saving General Panini parameters must persist Hugin order and invalidate calibration from the canvas stage");

  YAML::Node generated_override = YAML::Clone(config);
  generated_override["stitching"]["projection"] = "triplane";
  generated_override["stitching"]["projection_parameters"]["triplane"] = YAML::Load("[75]");
  YAML::Node generated_choices = generated_override["hstream_ui"]["generated_stitching_backend_choices"];
  generated_choices["control_point_matcher"] =
      generated_override["stitching"]["control_point_matcher"].as<std::string>();
  generated_choices["mapping_backend"] = "nona";
  generated_choices["projection"] = "triplane";
  generated_choices["run_autooptimizer"] = true;
  generated_choices["projection_parameters"] = YAML::Load("[75]");
  generated_choices["previous_control_point_matcher"] =
      generated_override["stitching"]["control_point_matcher"].as<std::string>();
  generated_choices["previous_mapping_backend"] = "nona";
  generated_choices["previous_projection"] = "general-panini";
  generated_choices["previous_run_autooptimizer"] = true;
  generated_choices["previous_projection_parameters"] = YAML::Load("[120, 15, -20]");
  std::ofstream(config_path) << YAML::Dump(generated_override) << '\n';
  activate(create);
  const bool generated_parameters_restored = expect(
      projection->currentData().toString() == "general-panini" && compression->value() == 120.0 &&
          top_squeeze->value() == 15.0 && bottom_squeeze->value() == -20.0,
      "UI load must restore projection parameters displaced by generated backend choices");
  projection->setCurrentIndex(projection->findData("triplane"));
  QApplication::processEvents();
  const bool generated_projection_parameters_discarded = expect(
      compression->value() == 60.0,
      "UI load must discard generated parameters for a projection that was not previously selected");

  generated_choices["previous_generated_projection_parameters"] = YAML::Load("[80]");
  std::ofstream(config_path) << YAML::Dump(generated_override) << '\n';
  activate(create);
  projection->setCurrentIndex(projection->findData("triplane"));
  QApplication::processEvents();
  const bool displaced_inactive_parameters_restored = expect(
      compression->value() == 80.0,
      "UI load must restore an inactive custom parameter vector displaced by a generated projection");

  generated_override["stitching"]["projection_parameters"]["general-panini"] = YAML::Load("[125, 10, -10]");
  std::ofstream(config_path) << YAML::Dump(generated_override) << '\n';
  activate(create);
  const bool edited_inactive_parameters_are_preserved = expect(
      projection->currentData().toString() == "general-panini" && compression->value() == 125.0 &&
          top_squeeze->value() == 10.0 && bottom_squeeze->value() == -10.0,
      "UI load must preserve current inactive projection parameters over stale restoration metadata");

  generated_override["stitching"]["projection_parameters"]["triplane"] = YAML::Load("[80]");
  std::ofstream(config_path) << YAML::Dump(generated_override) << '\n';
  activate(create);
  const bool edited_generated_parameters_are_user_intent = expect(
      projection->currentData().toString() == "triplane" && compression->value() == 80.0,
      "UI load must stop trusting generated-choice provenance after projection parameters are edited");

  YAML::Node generated_backend_alias = YAML::Clone(config);
  generated_backend_alias["stitching"]["control_point_matcher"] = "dedode";
  generated_backend_alias["stitching"]["mapping_backend"] = "MAGSAC++";
  generated_backend_alias["stitching"]["projection"] = "rectilinear";
  generated_backend_alias["stitching"]["run_autooptimizer"] = false;
  YAML::Node generated_backend_alias_choices =
      generated_backend_alias["hstream_ui"]["generated_stitching_backend_choices"];
  generated_backend_alias_choices["control_point_matcher"] = "dedode-lightglue";
  generated_backend_alias_choices["mapping_backend"] = "opencv-magsac";
  generated_backend_alias_choices["projection"] = "rectilinear";
  generated_backend_alias_choices["run_autooptimizer"] = false;
  generated_backend_alias_choices.remove("projection_parameters");
  generated_backend_alias_choices["previous_control_point_matcher"] = "superpoint";
  generated_backend_alias_choices["previous_mapping_backend"] = "nona";
  generated_backend_alias_choices["previous_projection"] = "general-panini";
  generated_backend_alias_choices["previous_run_autooptimizer"] = true;
  generated_backend_alias_choices["previous_projection_parameters"] = YAML::Load("[120, 15, -20]");
  generated_backend_alias_choices.remove("previous_generated_projection_parameters");
  std::ofstream(config_path) << YAML::Dump(generated_backend_alias) << '\n';
  activate(create);
  const bool generated_backend_aliases_restore_previous = expect(
      control_point_matcher->currentData().toString() == "superpoint-lightglue" &&
          mapping_backend->currentData().toString() == "nona" &&
          projection->currentData().toString() == "general-panini" && compression->value() == 120.0 &&
          top_squeeze->value() == 15.0 && bottom_squeeze->value() == -20.0,
      "UI load must compare generated matcher and mapping backend provenance with shared parser semantics");

  YAML::Node generated_framing_override = YAML::Clone(config);
  YAML::Node generated_framing = generated_framing_override["stitching"]["projection_framing"];
  generated_framing["auto_fov"] = true;
  generated_framing["horizontal_fov"] = 240.0;
  generated_framing["auto_canvas"] = false;
  generated_framing["auto_crop"] = true;
  YAML::Node generated_framing_choices =
      generated_framing_override["hstream_ui"]["generated_stitching_backend_choices"];
  generated_framing_choices["control_point_matcher"] =
      generated_framing_override["stitching"]["control_point_matcher"].as<std::string>();
  generated_framing_choices["mapping_backend"] = "nona";
  generated_framing_choices["projection"] = "general-panini";
  generated_framing_choices["run_autooptimizer"] = true;
  generated_framing_choices["projection_parameters"] = YAML::Load("[120, 15, -20]");
  generated_framing_choices["projection_framing"] = YAML::Clone(generated_framing);
  generated_framing_choices["previous_control_point_matcher"] =
      generated_framing_override["stitching"]["control_point_matcher"].as<std::string>();
  generated_framing_choices["previous_mapping_backend"] = "nona";
  generated_framing_choices["previous_projection"] = "general-panini";
  generated_framing_choices["previous_run_autooptimizer"] = true;
  generated_framing_choices["previous_projection_parameters"] = YAML::Load("[120, 15, -20]");
  generated_framing_choices["previous_projection_framing"] = YAML::Load("{horizontal_fov: 185}");
  std::ofstream(config_path) << YAML::Dump(generated_framing_override) << '\n';
  activate(create);
  const bool partial_previous_framing_inherits_defaults = expect(
      !auto_fov->isChecked() && horizontal_fov->value() == 185.0 && auto_canvas->isChecked() && !auto_crop->isChecked(),
      "UI load must merge partial previous projection framing over effective inherited defaults");

  generated_framing_choices.remove("previous_projection_framing");
  std::ofstream(config_path) << YAML::Dump(generated_framing_override) << '\n';
  activate(create);
  const bool absent_previous_framing_restores_defaults = expect(
      !auto_fov->isChecked() && horizontal_fov->value() == 180.0 && auto_canvas->isChecked() && !auto_crop->isChecked(),
      "UI load must restore inherited projection framing when generated choices displaced no private map");

  auto malformed_previous_choice_rejects_marker = [&](const char* key, const char* value, const char* message) {
    YAML::Node malformed = YAML::Clone(generated_backend_alias);
    malformed["hstream_ui"]["generated_stitching_backend_choices"][key] = value;
    std::ofstream(config_path) << YAML::Dump(malformed) << '\n';
    activate(create);
    return expect(
        control_point_matcher->currentData().toString() == "dedode-lightglue" &&
            mapping_backend->currentData().toString() == "opencv-magsac" &&
            projection->currentData().toString() == "rectilinear",
        message);
  };
  const bool invalid_previous_matcher_rejects_marker = malformed_previous_choice_rejects_marker(
      "previous_control_point_matcher",
      "invalid-matcher",
      "UI load must reject the entire generated marker when its previous matcher is malformed");
  const bool invalid_previous_backend_rejects_marker = malformed_previous_choice_rejects_marker(
      "previous_mapping_backend",
      "invalid-backend",
      "UI load must reject the entire generated marker when its previous mapping backend is malformed");
  YAML::Node invalid_previous_projection = YAML::Clone(generated_backend_alias);
  YAML::Node invalid_previous_projection_choices =
      invalid_previous_projection["hstream_ui"]["generated_stitching_backend_choices"];
  invalid_previous_projection_choices["previous_projection"] = "invalid-projection";
  invalid_previous_projection_choices.remove("previous_projection_parameters");
  std::ofstream(config_path) << YAML::Dump(invalid_previous_projection) << '\n';
  activate(create);
  const bool invalid_previous_projection_rejects_marker = expect(
      control_point_matcher->currentData().toString() == "dedode-lightglue" &&
          mapping_backend->currentData().toString() == "opencv-magsac" &&
          projection->currentData().toString() == "rectilinear",
      "UI load must reject the entire generated marker when its previous projection scalar is malformed");
  auto worker_tuple_fixture = [&]() {
    YAML::Node fixture = YAML::Clone(generated_backend_alias);
    fixture["stitching"]["control_point_matcher"] = "superpoint-lightglue";
    fixture["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] = "superpoint";
    return fixture;
  };
  auto show_non_worker_tuple = [&]() {
    mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
    projection->setCurrentIndex(projection->findData("general-panini"));
    QApplication::processEvents();
  };
  auto worker_tuple_is_visible = [&](const char* message) {
    return expect(
        control_point_matcher->currentData().toString() == "superpoint-lightglue" &&
            mapping_backend->currentData().toString() == "opencv-magsac" &&
            projection->currentData().toString() == "rectilinear" && !run_autooptimizer->isChecked(),
        message);
  };
  YAML::Node incompatible_previous_tuple = worker_tuple_fixture();
  YAML::Node incompatible_previous_choices =
      incompatible_previous_tuple["hstream_ui"]["generated_stitching_backend_choices"];
  incompatible_previous_choices["previous_mapping_backend"] = "opencv-magsac";
  incompatible_previous_choices["previous_projection"] = "general-panini";
  incompatible_previous_choices["previous_run_autooptimizer"] = false;
  std::ofstream(config_path) << YAML::Dump(incompatible_previous_tuple) << '\n';
  show_non_worker_tuple();
  activate(create);
  const bool incompatible_previous_tuple_rejects_marker = worker_tuple_is_visible(
      "UI load must reject incompatible previous backend/projection provenance and load the current worker tuple");
  YAML::Node malformed_previous_autooptimizer = worker_tuple_fixture();
  malformed_previous_autooptimizer["hstream_ui"]["generated_stitching_backend_choices"]["previous_run_autooptimizer"] =
      "not-a-boolean";
  std::ofstream(config_path) << YAML::Dump(malformed_previous_autooptimizer) << '\n';
  show_non_worker_tuple();
  activate(create);
  const bool malformed_previous_autooptimizer_rejects_marker = worker_tuple_is_visible(
      "UI load must reject a malformed previous autooptimizer without aborting the current worker-tuple load");
  YAML::Node selectable_previous_matcher = worker_tuple_fixture();
  selectable_previous_matcher["hstream_ui"]["generated_stitching_backend_choices"]["previous_control_point_matcher"] =
      "dedode-lightglue";
  std::ofstream(config_path) << YAML::Dump(selectable_previous_matcher) << '\n';
  const int dedode_index = control_point_matcher->findData("dedode-lightglue");
  control_point_matcher->setCurrentIndex(control_point_matcher->findData("superpoint-lightglue"));
  const bool alternate_matcher_state_selected = expect(
      dedode_index >= 0 && control_point_matcher->currentData().toString() == "superpoint-lightglue",
      "UI test setup must select an alternate matcher before loading generated provenance");
  activate(create);
  const bool selectable_previous_matcher_is_restored = expect(
      control_point_matcher->currentData().toString() == "dedode-lightglue" &&
          mapping_backend->currentData().toString() == "nona" &&
          projection->currentData().toString() == "general-panini",
      "UI load must restore a generated-choice previous matcher that is now selectable");

  game_id->setText("ui-opencv-framing-roundtrip-game");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("opencv-magsac"));
  projection->setCurrentIndex(projection->findData("rectilinear"));
  QApplication::processEvents();
  const bool opencv_framing_starts_clean = expect(
      horizontal_fov->value() == 180.0 && !save->isEnabled(),
      "A fresh OpenCV preset must keep the inherited 180-degree framing as clean state");
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  QApplication::processEvents();
  const bool nona_rectilinear_clamps_fov = expect(
      horizontal_fov->value() == 179.0,
      "NONA Rectilinear must constrain the fixed horizontal FOV to Hugin's 179-degree limit");
  mapping_backend->setCurrentIndex(mapping_backend->findData("opencv-magsac"));
  QApplication::processEvents();
  const bool inactive_opencv_framing_is_clean = expect(
      !save->isEnabled(),
      "OpenCV to NONA to OpenCV must not dirty the preset because OpenCV does not use projection framing");

  game_id->setText("ui-projection-fov-cache-game-a");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("stereographic"));
  auto_fov->setChecked(false);
  horizontal_fov->setValue(250.0);
  projection->setCurrentIndex(projection->findData("rectilinear"));
  QApplication::processEvents();
  game_id->setText("ui-projection-fov-cache-game-b");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("stereographic"));
  QApplication::processEvents();
  const bool projection_fov_does_not_leak_between_games = expect(
      horizontal_fov->value() == 180.0, "Inactive per-projection FOV values must not leak from one game into another");

  horizontal_fov->setValue(250.0);
  projection->setCurrentIndex(projection->findData("rectilinear"));
  activate(reset);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("stereographic"));
  QApplication::processEvents();
  const bool projection_fov_reset_clears_cache =
      expect(horizontal_fov->value() == 180.0, "Reset Controls must clear inactive per-projection FOV values");

  game_id->setText("ui-projection-parameter-cache-game-a");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("general-panini"));
  compression->setValue(135.0);
  top_squeeze->setValue(25.0);
  bottom_squeeze->setValue(-30.0);
  QApplication::processEvents();
  game_id->setText("ui-projection-parameter-cache-game-b");
  activate(create);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("general-panini"));
  QApplication::processEvents();
  const bool projection_parameters_do_not_leak_between_games = expect(
      compression->value() == 100.0 && top_squeeze->value() == 0.0 && bottom_squeeze->value() == 0.0,
      "Visible projection parameters must not leak from one game into a newly created game");

  compression->setValue(140.0);
  top_squeeze->setValue(35.0);
  bottom_squeeze->setValue(-40.0);
  QApplication::processEvents();
  activate(reset);
  mapping_backend->setCurrentIndex(mapping_backend->findData("nona"));
  projection->setCurrentIndex(projection->findData("general-panini"));
  QApplication::processEvents();
  const bool projection_parameter_reset_clears_cache = expect(
      compression->value() == 100.0 && top_squeeze->value() == 0.0 && bottom_squeeze->value() == 0.0,
      "Reset Controls must discard the visible projection parameter cache");

  game_id->setText(original_game_id);
  activate(create);
  return saved && generated_parameters_restored && generated_projection_parameters_discarded &&
      displaced_inactive_parameters_restored && edited_inactive_parameters_are_preserved &&
      edited_generated_parameters_are_user_intent && generated_backend_aliases_restore_previous &&
      partial_previous_framing_inherits_defaults && absent_previous_framing_restores_defaults &&
      invalid_previous_matcher_rejects_marker && invalid_previous_backend_rejects_marker &&
      invalid_previous_projection_rejects_marker && incompatible_previous_tuple_rejects_marker &&
      malformed_previous_autooptimizer_rejects_marker && alternate_matcher_state_selected &&
      selectable_previous_matcher_is_restored && opencv_framing_starts_clean && nona_rectilinear_clamps_fov &&
      inactive_opencv_framing_is_clean && projection_fov_does_not_leak_between_games &&
      projection_fov_reset_clears_cache && projection_parameters_do_not_leak_between_games &&
      projection_parameter_reset_clears_cache;
}

bool test_camera_controls(HStreamWindow* window) {
  if (!expect(window->cameraTabCount() == 7, "Native-effective controls should be grouped by associated stage")) {
    return false;
  }

  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* fixed_edge_link = require_child<QSlider>(window, "cameraSlider_Link_Fixed_Edge_Rotation_Left_Right");
  auto* fixed_edge_left = require_child<QSlider>(window, "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10");
  auto* fixed_edge_right = require_child<QSlider>(window, "cameraSlider_Right_Fixed_Edge_Rotation_Angle_x10");
  auto* stop_delay = require_child<QSlider>(window, "cameraSlider_Stop_Direction_Change_Delay_Frames");
  auto* zoom_in_aggressiveness = require_child<QSlider>(window, "cameraSlider_Zoom_In_Aggressiveness");
  auto* apply_to_fast = require_child<QSlider>(window, "cameraSlider_Apply_To_Fast_Box");
  auto* max_accel_x = require_child<QSlider>(window, "cameraSlider_Max_Accel_X_x10");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* max_speed_y = require_child<QSlider>(window, "cameraSlider_Max_Speed_Y_x10");
  auto* bring_up_shadows = require_child<QSlider>(window, "cameraSlider_Bring_Up_Shadows");
  auto* exposure = require_child<QSlider>(window, "cameraSlider_Exposure_x100");
  auto* lift_shadow_black_point = require_child<QCheckBox>(window, "cameraCheck_Lift_Shadow_Black_Point");
  auto* use_10_bit_grading = require_child<QCheckBox>(window, "cameraCheck_Use_10_Bit_Grading");
  auto* stitched_bring_up_shadows = require_child<QSlider>(window, "stitchedCameraSlider_Bring_Up_Shadows");
  auto* stitched_exposure = require_child<QSlider>(window, "stitchedCameraSlider_Exposure_x100");
  auto* stitched_lift_shadow_black_point =
      require_child<QCheckBox>(window, "stitchedCameraCheck_Lift_Shadow_Black_Point");
  auto* stitched_use_10_bit_grading = require_child<QCheckBox>(window, "stitchedCameraCheck_Use_10_Bit_Grading");
  auto* stitched_precision_status = require_child<QLabel>(window, "stitchedColorPrecisionStatus");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* clean_stitching = require_child<QPushButton>(window, "cleanStitchingButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* restart = require_child<QPushButton>(window, "restartStageButton");
  auto* pipeline_process = window->findChild<QProcess*>();
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* stitch_frame_time = require_child<QTimeEdit>(window, "stitchFrameTimeEdit");
  auto* stitch_max_output_width = require_child<QSpinBox>(window, "stitchMaxOutputWidthSpin");
  auto* mapping_backend = require_child<QComboBox>(window, "mappingBackendCombo");
  auto* projection = require_child<QComboBox>(window, "stitchProjectionCombo");
  if (!rotate || !fixed_edge_link || !fixed_edge_left || !fixed_edge_right || !stop_delay || !zoom_in_aggressiveness ||
      !apply_to_fast || !max_accel_x || !max_speed_x || !max_speed_y || !bring_up_shadows || !exposure ||
      !lift_shadow_black_point || !use_10_bit_grading || !stitched_bring_up_shadows || !stitched_exposure ||
      !stitched_lift_shadow_black_point || !stitched_use_10_bit_grading || !stitched_precision_status || !reset ||
      !clean_stitching || !save || !create || !game_id || !start || !stop || !restart || !pipeline_process || !mode ||
      !stitch_frame_time || !stitch_max_output_width || !mapping_backend || !projection) {
    return false;
  }

  const QStringList documented_controls = {
      "runModeCombo",
      "controlPointsSpin",
      "stitchFrameTimeEdit",
      "stitchMaxOutputWidthSpin",
      "runAutooptimizerCheck",
      "renderVideoCheck",
      "startPipelineButton",
      "pausePipelineButton",
      "restartStageButton",
      "savePresetButton",
      "resetCameraButton",
      "cleanStitchingButton",
      "stopPipelineButton",
      "createGameButton",
      "refreshGamesButton",
      "browseVideoButton",
      "addVideoButton",
      "removeVideoButton",
      "videoRole_auto",
      "videoRole_left",
      "videoRole_center",
      "videoRole_right",
      "outputToggle_youtube-primary",
      "outputToggle_rtsp-local",
      "outputToggle_archive-file",
      "outputToggle_archive-stitched",
      "outputToggle_spare-rtmp",
      "redirectYoutubeButton",
      "addRtspButton",
      "clearLogButton",
      "programFocusButton",
      "stitchedFocusButton",
      "camera1FocusButton",
      "camera2FocusButton",
      "camera3FocusButton",
      "programControlsToggle",
      "stitchedControlsToggle",
      "projectionAutoFovCheck",
      "projectionHorizontalFovSpin",
      "projectionAutoCanvasCheck",
      "projectionAutoCropCheck",
      "cameraSlider_Stitch_Rotate_Degrees",
      "playbackSeekSlider",
      "playbackSeekBack10Button",
      "playbackSeekForward10Button",
      "cameraSlider_Zoom_In_Aggressiveness",
      "cameraSlider_Stop_Direction_Change_Delay_Frames",
      "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10",
      "cameraSlider_Bring_Up_Shadows",
      "cameraSlider_Exposure_x100",
      "cameraCheck_Lift_Shadow_Black_Point",
      "cameraCheck_Use_10_Bit_Grading",
      "stitchedCameraSlider_Bring_Up_Shadows",
      "stitchedCameraSlider_Exposure_x100",
      "stitchedCameraCheck_Lift_Shadow_Black_Point",
      "stitchedCameraCheck_Use_10_Bit_Grading",
  };
  for (const QString& object_name : documented_controls) {
    QWidget* control = window->findChild<QWidget*>(object_name);
    if (!expect(
            control && control->toolTip().trimmed().size() >= 20 && control->statusTip() == control->toolTip(),
            QString("Interactive control should provide detailed hover help: %1").arg(object_name).toStdString())) {
      return false;
    }
  }

  mode->setCurrentIndex(mode->findData("program"));
  bring_up_shadows->setValue(17);
  if (!expect(
          stitched_bring_up_shadows->value() == 17,
          "Program Color slider changes should update the Stitched Color & Precision mirror")) {
    return false;
  }
  stitched_bring_up_shadows->setValue(23);
  stitched_exposure->setValue(30);
  stitched_lift_shadow_black_point->setChecked(true);
  if (!expect(
          bring_up_shadows->value() == 23 && exposure->value() == 30 && lift_shadow_black_point->isChecked(),
          "Stitched Color & Precision edits should update the canonical Program Color controls")) {
    return false;
  }
  lift_shadow_black_point->setChecked(false);
  exposure->setValue(60);
  if (!expect(
          !stitched_lift_shadow_black_point->isChecked() && stitched_exposure->value() == 60 &&
              stitched_precision_status->text().contains("existing 8-bit Program grading"),
          "Canonical checkbox/slider changes and Program-mode precision status should update the Stitched tab")) {
    return false;
  }

  mode->setCurrentIndex(mode->findData("stitch-calibration"));
  stitched_use_10_bit_grading->setCheckState(Qt::PartiallyChecked);
  if (!expect(
          stitched_precision_status->text().contains("automatic source-depth detection") &&
              stitched_bring_up_shadows->isEnabled() && stitched_exposure->isEnabled() &&
              stitched_lift_shadow_black_point->isEnabled(),
          "Idle automatic calibration should explain its 10-bit-only color behavior while allowing preset edits")) {
    return false;
  }
  stitched_use_10_bit_grading->setChecked(true);
  if (!expect(
          use_10_bit_grading->isChecked() && stitched_precision_status->text().contains("forced 10-bit / FP16"),
          "The Stitched force override should update the canonical setting and next-run status")) {
    return false;
  }
  use_10_bit_grading->setChecked(false);
  if (!expect(
          stitched_precision_status->text().contains("forced standard 8-bit") &&
              !stitched_bring_up_shadows->isEnabled() && !stitched_exposure->isEnabled() &&
              !stitched_lift_shadow_black_point->isEnabled(),
          "The forced-off state should remain distinct from Auto and disable unsupported calibration grading")) {
    return false;
  }
  use_10_bit_grading->setCheckState(Qt::PartiallyChecked);
  HStreamWindowTestAccess::setCalibrationPrecisionRunActive(window, true);
  if (!expect(
          stitched_precision_status->text().contains("Detecting source precision") &&
              !stitched_bring_up_shadows->isEnabled() && !stitched_exposure->isEnabled() &&
              !stitched_lift_shadow_black_point->isEnabled() && stitched_use_10_bit_grading->isEnabled(),
          "Calibration tone controls should wait for the effective source-precision decision while Force stays editable")) {
    return false;
  }
  const bool high_bit_reported = HStreamWindowTestAccess::reportHighBitDepth(
      window, "HSTREAM_HIGH_BIT_DEPTH mode=auto enabled=1 sources=2 unknown=0 minimum-source-bit-depth=10");
  if (!expect(
          high_bit_reported && stitched_precision_status->text().contains("Auto detected: 10-bit / FP16 active") &&
              stitched_precision_status->text().contains("minimum 10-bit") && stitched_bring_up_shadows->isEnabled() &&
              stitched_exposure->isEnabled() && stitched_lift_shadow_black_point->isEnabled(),
          "An automatic 10-bit decision should unlock calibration color controls and report the effective path")) {
    return false;
  }
  const bool low_bit_reported = HStreamWindowTestAccess::reportHighBitDepth(
      window, "HSTREAM_HIGH_BIT_DEPTH mode=auto enabled=0 sources=2 unknown=0 minimum-source-bit-depth=8");
  if (!expect(
          low_bit_reported && stitched_precision_status->text().contains("minimum source depth is 8-bit") &&
              !stitched_bring_up_shadows->isEnabled() && !stitched_exposure->isEnabled() &&
              !stitched_lift_shadow_black_point->isEnabled() && stitched_use_10_bit_grading->isEnabled(),
          "An 8-bit calibration decision should disable only the unsupported tone controls")) {
    return false;
  }
  const bool unknown_reported = HStreamWindowTestAccess::reportHighBitDepth(
      window, "HSTREAM_HIGH_BIT_DEPTH mode=auto enabled=0 sources=2 unknown=1 minimum-source-bit-depth=10");
  if (!expect(
          unknown_reported && stitched_precision_status->text().contains("1 of 2 source bit depths are unknown"),
          "Unknown source precision should explain why calibration grading is unavailable")) {
    return false;
  }
  HStreamWindowTestAccess::setCalibrationPrecisionRunActive(window, false);
  mode->setCurrentIndex(mode->findData("program"));

  game_id->setText("ui-camera-control-game");
  activate(create);
  if (!expect(
          reset->text() == "Reset Controls" &&
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 10 &&
              window->cameraControlValue("Cancel_Stop_On_Opposite_Direction") == 1 &&
              window->cameraControlValue("Stop_Cancel_Hysteresis_Frames") == 2 &&
              window->cameraControlValue("Stop_Delay_Cooldown_Frames") == 2 &&
              window->cameraControlValue("Time_To_Dest_Speed_Limit_Frames") == 20 &&
              window->cameraControlValue("Zoom_In_Aggressiveness") == 25 &&
              window->cameraControlValue("Overshoot_Stop_Delay_Frames") == 6 &&
              window->cameraControlValue("Post_Nonstop_Stop_Delay_Frames") == 6 &&
              window->cameraControlValue("Overshoot_Speed_Ratio_x100") == 70 &&
              window->cameraControlValue("Stitch_Rotate_Degrees") == 90 &&
              window->cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") == 1 &&
              window->cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10") == 100 &&
              window->cameraControlValue("Right_Fixed_Edge_Rotation_Angle_x10") == 100 &&
              window->cameraControlValue("Lift_Shadow_Black_Point") == 0 && !lift_shadow_black_point->isChecked() &&
              window->cameraControlValue("Exposure_x100") == 0 &&
              window->cameraControlValue("Use_10_Bit_Grading") == 0 &&
              use_10_bit_grading->checkState() == Qt::PartiallyChecked && stitched_bring_up_shadows->value() == 0 &&
              stitched_exposure->value() == 0 && !stitched_lift_shadow_black_point->isChecked() &&
              stitched_use_10_bit_grading->checkState() == Qt::PartiallyChecked,
          "Camera control defaults should be transformed directly from the bundled baseline")) {
    return false;
  }
  bring_up_shadows->setValue(35);
  lift_shadow_black_point->setChecked(true);
  exposure->setValue(60);
  use_10_bit_grading->setChecked(true);
  const QStringList high_bit_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  if (!expect(
          high_bit_arguments.contains("--options=pipeline.hmstitcher.properties.high-bit-depth=1") &&
              high_bit_arguments.contains("--options=hstream_ui.camera_controls.Bring_Up_Shadows=35") &&
              high_bit_arguments.contains("--options=hstream_ui.camera_controls.Lift_Shadow_Black_Point=1") &&
              high_bit_arguments.contains("--options=hstream_ui.camera_controls.Exposure_x100=60"),
          "A forced high-bit launch should let the CLI route canonical tone controls to the FP16 stitcher")) {
    return false;
  }
  use_10_bit_grading->setChecked(false);
  const QStringList forced_8_bit_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  if (!expect(
          forced_8_bit_arguments.contains("--options=pipeline.hmstitcher.properties.high-bit-depth=0") &&
              forced_8_bit_arguments.contains("--options=hstream_ui.camera_controls.Bring_Up_Shadows=35") &&
              forced_8_bit_arguments.contains("--options=hstream_ui.camera_controls.Lift_Shadow_Black_Point=1") &&
              forced_8_bit_arguments.contains("--options=hstream_ui.camera_controls.Exposure_x100=60"),
          "A forced standard launch should preserve the explicit off state and canonical tone controls")) {
    return false;
  }
  use_10_bit_grading->setCheckState(Qt::PartiallyChecked);
  const QStringList automatic_arguments = HStreamWindowTestAccess::pipelineArguments(window);
  if (!expect(
          automatic_arguments.contains("--options=pipeline.hmstitcher.properties.high-bit-depth=auto"),
          "The default launch should defer source-depth and tone-stage selection to the CLI")) {
    return false;
  }
  const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const fs::path game_dir = config.parent_path();
  {
    YAML::Node clean_fixture(YAML::NodeType::Map);
    clean_fixture["stitching"]["post_stitch_rotate_degrees"] = 18.0;
    clean_fixture["stitching"]["mapping_backend"] = "opencv-magsac";
    clean_fixture["stitching"]["control_point_matcher"] = "superpoint-lightglue";
    clean_fixture["stitching"]["max_output_width"] = 4096;
    clean_fixture["stitching"]["frame_offsets"]["left"] = "12";
    clean_fixture["stitching"]["control_points"][0][0] = 1.0;
    clean_fixture["game"]["stitching"]["frame_offsets"]["right"] = "34";
    clean_fixture["game"]["stitching"]["control_points"][0][0] = 2.0;
    clean_fixture["hstream_ui"]["stitching_calibration"]["control_points"] = 1700;
    clean_fixture["hstream_ui"]["stitching_calibration"]["frame_count"] = 3;
    clean_fixture["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    clean_fixture["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "complete";
    clean_fixture["hstream_ui"]["stitching_calibration"]["stale_from"] = "canvas";
    clean_fixture["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
    clean_fixture["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "clean-fixture-generation";
    clean_fixture["hstream_ui"]["stitching_calibration"]["backend_generation"] = "clean-fixture-generation";
    clean_fixture["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = "opencv-magsac";
    clean_fixture["rink"]["scoreboard"]["perspective_polygon"].push_back(1);
    clean_fixture["rink"]["ice_contours_mask_count"] = 1;
    clean_fixture["rink"]["camera"]["fixed_edge_rotation_angle"] = 7.5;
    clean_fixture["pipeline"]["hmstitcher"]["properties"]["shadow-lift"] = 35;
    std::ofstream out(config);
    out << clean_fixture << "\n";
  }
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto", "mapping_0000.tif", "panorama.tif",
                           "seam_file.png", "left.png", "right.png", "rink_mask_0.png"}) {
    std::ofstream artifact(game_dir / name);
    artifact << "manual clean fixture\n";
  }
  qputenv("HSTREAM_UI_TEST_SIMULATE_CLEAN_ARTIFACTS", "1");
  activate(clean_stitching);
  qunsetenv("HSTREAM_UI_TEST_SIMULATE_CLEAN_ARTIFACTS");
  const YAML::Node cleaned_stitching = YAML::LoadFile(config.string());
  if (!expect(
          !fs::exists(game_dir / "hm_project.pto") && !fs::exists(game_dir / "panorama.tif") &&
              !fs::exists(game_dir / "seam_file.png") && !fs::exists(game_dir / "left.png") &&
              !fs::exists(game_dir / "right.png") && !fs::exists(game_dir / "rink_mask_0.png"),
          "Clean Stitching should remove full stitching calibration artifacts") ||
      !expect(
          !lookup_yaml_path(cleaned_stitching, {"stitching", "frame_offsets"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"game", "stitching", "frame_offsets"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"stitching", "control_points"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"game", "stitching", "control_points"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"hstream_ui", "stitching_calibration", "status"}, nullptr) &&
              !lookup_yaml_path(
                  cleaned_stitching, {"hstream_ui", "stitching_calibration", "rink_mask_status"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"hstream_ui", "stitching_calibration", "stale_from"}, nullptr) &&
              !lookup_yaml_path(
                  cleaned_stitching, {"hstream_ui", "stitching_calibration", "artifacts_invalidated"}, nullptr) &&
              !lookup_yaml_path(
                  cleaned_stitching, {"hstream_ui", "stitching_calibration", "invalidation_id"}, nullptr) &&
              !lookup_yaml_path(
                  cleaned_stitching, {"hstream_ui", "stitching_calibration", "backend_generation"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"rink", "scoreboard", "perspective_polygon"}, nullptr) &&
              !lookup_yaml_path(cleaned_stitching, {"rink", "ice_contours_mask_count"}, nullptr),
          "Clean Stitching should remove calibration-derived config.yaml state") ||
      !expect(
          cleaned_stitching["stitching"]["post_stitch_rotate_degrees"].as<double>() == 18.0 &&
              cleaned_stitching["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
              cleaned_stitching["stitching"]["control_point_matcher"].as<std::string>() == "superpoint-lightglue" &&
              cleaned_stitching["stitching"]["max_output_width"].as<int>() == 4096 &&
              cleaned_stitching["hstream_ui"]["stitching_calibration"]["control_points"].as<int>() == 1700 &&
              cleaned_stitching["hstream_ui"]["stitching_calibration"]["frame_count"].as<int>() == 3 &&
              cleaned_stitching["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"]
                      .as<std::string>() == "opencv-magsac" &&
              cleaned_stitching["rink"]["camera"]["fixed_edge_rotation_angle"].as<double>() == 7.5 &&
              cleaned_stitching["pipeline"]["hmstitcher"]["properties"]["shadow-lift"].as<int>() == 35,
          "Clean Stitching should preserve user-authored stitching and non-calibration config")) {
    return false;
  }
  {
    YAML::Node capped(YAML::NodeType::Map);
    capped["stitching"]["max_output_width"] = 4096;
    capped["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = 2048;
    capped["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 2048;
    capped["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    capped["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "complete";
    std::ofstream out(config);
    out << capped << "\n";
  }
  activate(create);
  if (!expect(
          stitch_max_output_width->value() == 4096 && !save->isEnabled(),
          "A saved max stitched width should load as the clean preset state")) {
    return false;
  }
  bring_up_shadows->setValue(36);
  if (!expect(save->isEnabled(), "Changing an unrelated stitching control should enable Save Preset")) {
    return false;
  }
  auto active_calibration_lock = hm::stitching::try_lock_canvas_constraint_artifacts(config.parent_path());
  if (!expect(
          active_calibration_lock.ok() && *active_calibration_lock,
          "Unchanged-width preset save must have a contended calibration fixture")) {
    return false;
  }
  activate(save);
  active_calibration_lock->reset();
  const YAML::Node after_conflicting_width_save = YAML::LoadFile(config.string());
  const YAML::Node after_conflicting_width_calibration =
      after_conflicting_width_save["hstream_ui"]["stitching_calibration"];
  if (!expect(
          after_conflicting_width_save["stitching"]["max_output_width"].as<int>() == 4096 &&
              !lookup_yaml_path(
                  after_conflicting_width_save,
                  {"pipeline", "hmstitcher", "properties", "max-output-width"},
                  nullptr) &&
              after_conflicting_width_calibration["status"].as<std::string>() == "complete" &&
              !after_conflicting_width_calibration["stale_from"].IsDefined(),
          "Unchanged-width preset save must ignore calibration lock contention and normalize aliases")) {
    return false;
  }
  bring_up_shadows->setValue(35);
  stitch_max_output_width->setValue(0);
  if (!expect(save->isEnabled(), "Changing max stitched width back to Auto should enable Save Preset")) {
    return false;
  }
  qputenv("HSTREAM_UI_TEST_CANVAS_CHECK", "compatible");
  activate(save);
  qunsetenv("HSTREAM_UI_TEST_CANVAS_CHECK");
  const YAML::Node after_max_width_auto = YAML::LoadFile(config.string());
  const YAML::Node after_max_width_auto_calibration = after_max_width_auto["hstream_ui"]["stitching_calibration"];
  if (!expect(
          !lookup_yaml_path(after_max_width_auto, {"stitching", "max_output_width"}, nullptr) &&
              !lookup_yaml_path(
                  after_max_width_auto, {"pipeline", "hmstitcher", "properties", "max-output-width"}, nullptr) &&
              !lookup_yaml_path(
                  after_max_width_auto,
                  {"pipeline", "hmstitcher", "private-properties", "stitch_max_output_width"},
                  nullptr) &&
              after_max_width_auto_calibration["status"].as<std::string>() == "complete" &&
              !after_max_width_auto_calibration["stale_from"].IsDefined() &&
              after_max_width_auto_calibration["rink_mask_status"].as<std::string>() == "complete" &&
              !after_max_width_auto_calibration["artifacts_invalidated"].IsDefined(),
          "Saving a nonbinding 4096 -> Auto change must preserve compatible canvas artifacts")) {
    return false;
  }
  {
    YAML::Node native_only(YAML::NodeType::Map);
    native_only["pipeline"]["hmstitcher"]["properties"]["max_output_width"] = 2048;
    native_only["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 2048;
    native_only["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    native_only["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "complete";
    std::ofstream out(config);
    out << native_only << "\n";
  }
  activate(create);
  if (!expect(
          stitch_max_output_width->value() == 2048 && !save->isEnabled(),
          "A legacy native-only max stitched width should load as the clean preset state")) {
    return false;
  }
  stitch_max_output_width->setValue(0);
  qputenv("HSTREAM_UI_TEST_CANVAS_CHECK", "compatible");
  activate(save);
  qunsetenv("HSTREAM_UI_TEST_CANVAS_CHECK");
  const YAML::Node after_native_width_auto = YAML::LoadFile(config.string());
  const YAML::Node after_native_width_auto_calibration = after_native_width_auto["hstream_ui"]["stitching_calibration"];
  if (!expect(
          !lookup_yaml_path(after_native_width_auto, {"stitching", "max_output_width"}, nullptr) &&
              !lookup_yaml_path(
                  after_native_width_auto, {"pipeline", "hmstitcher", "properties", "max_output_width"}, nullptr) &&
              !lookup_yaml_path(
                  after_native_width_auto,
                  {"pipeline", "hmstitcher", "private-properties", "stitch_max_output_width"},
                  nullptr) &&
              after_native_width_auto_calibration["status"].as<std::string>() == "complete" &&
              !after_native_width_auto_calibration["stale_from"].IsDefined(),
          "Saving a compatible legacy native-only cap to Auto must migrate aliases without invalidating maps")) {
    return false;
  }
  {
    YAML::Node private_only(YAML::NodeType::Map);
    private_only["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 1536;
    private_only["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    private_only["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "complete";
    std::ofstream out(config);
    out << private_only << "\n";
  }
  activate(create);
  if (!expect(
          stitch_max_output_width->value() == 1536 && !save->isEnabled(),
          "A private-only native max stitched width should load as the clean preset state")) {
    return false;
  }
  bring_up_shadows->setValue(36);
  activate(save);
  const YAML::Node after_private_only_width_save = YAML::LoadFile(config.string());
  const YAML::Node after_private_only_width_calibration =
      after_private_only_width_save["hstream_ui"]["stitching_calibration"];
  if (!expect(
          after_private_only_width_save["stitching"]["max_output_width"].as<int>() == 1536 &&
              !lookup_yaml_path(
                  after_private_only_width_save,
                  {"pipeline", "hmstitcher", "private-properties", "stitch_max_output_width"},
                  nullptr) &&
              after_private_only_width_calibration["status"].as<std::string>() == "complete" &&
              after_private_only_width_calibration["rink_mask_status"].as<std::string>() == "complete",
          "Saving an unedited private-only native max stitched width must preserve the cap and calibration while migrating aliases")) {
    return false;
  }
  {
    YAML::Node existing(YAML::NodeType::Map);
    existing["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    std::ofstream out(config);
    out << existing << "\n";
  }
  activate(create);
  if (!expect(
          fixed_edge_link->value() == 1 && fixed_edge_left->value() == 220 && fixed_edge_right->value() == 220,
          "Camera controls should load the scalar rink.camera.fixed_edge_rotation_angle value") ||
      !expect(!save->isEnabled(), "Save Preset should be disabled after loading the saved control snapshot")) {
    return false;
  }

  {
    YAML::Node null_rotation(YAML::NodeType::Map);
    null_rotation["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Node(YAML::NodeType::Null);
    std::ofstream out(config);
    out << null_rotation << "\n";
  }
  activate(create);
  if (!expect(
          fixed_edge_link->value() == 1 && fixed_edge_left->value() == 0 && fixed_edge_right->value() == 0 &&
              !save->isEnabled(),
          "An explicit null fixed-edge rotation should load as a clean neutral UI value")) {
    return false;
  }
  rotate->setValue(89);
  activate(save);
  YAML::Node saved_with_null_rotation = YAML::LoadFile(config.string());
  if (!expect(
          saved_with_null_rotation["rink"]["camera"]["fixed_edge_rotation_angle"].IsNull(),
          "Saving an unrelated control must retain an explicit null fixed-edge rotation")) {
    return false;
  }

  {
    YAML::Node direct_overrides(YAML::NodeType::Map);
    direct_overrides["rink"]["camera"]["stop_on_dir_change_delay"] = 13;
    direct_overrides["rink"]["camera"]["cancel_stop_on_opposite_dir"] = false;
    direct_overrides["rink"]["camera"]["stop_cancel_hysteresis_frames"] = 4;
    direct_overrides["rink"]["camera"]["stop_delay_cooldown_frames"] = 5;
    direct_overrides["rink"]["camera"]["time_to_dest_speed_limit_frames"] = 30;
    direct_overrides["rink"]["camera"]["zoom_in_aggressiveness"] = 80;
    direct_overrides["rink"]["camera"]["breakaway_detection"]["overshoot_stop_delay_count"] = 8;
    direct_overrides["rink"]["camera"]["breakaway_detection"]["post_nonstop_stop_delay_count"] = 9;
    direct_overrides["rink"]["camera"]["breakaway_detection"]["overshoot_scale_speed_ratio"] = 0.83;
    direct_overrides["stitching"]["post_stitch_rotate_degrees"] = 18;
    direct_overrides["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 35;
    direct_overrides["pipeline"]["hmplaycropper"]["properties"]["shadow-lift-black-point"] = true;
    direct_overrides["pipeline"]["hmplaycropper"]["properties"]["exposure"] = 0.6;
    std::ofstream out(config);
    out << direct_overrides << "\n";
  }
  activate(create);
  if (!expect(
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 13 &&
              window->cameraControlValue("Cancel_Stop_On_Opposite_Direction") == 0 &&
              window->cameraControlValue("Stop_Cancel_Hysteresis_Frames") == 4 &&
              window->cameraControlValue("Stop_Delay_Cooldown_Frames") == 5 &&
              window->cameraControlValue("Time_To_Dest_Speed_Limit_Frames") == 30 &&
              window->cameraControlValue("Zoom_In_Aggressiveness") == 80 &&
              window->cameraControlValue("Overshoot_Stop_Delay_Frames") == 8 &&
              window->cameraControlValue("Post_Nonstop_Stop_Delay_Frames") == 9 &&
              window->cameraControlValue("Overshoot_Speed_Ratio_x100") == 83 &&
              window->cameraControlValue("Stitch_Rotate_Degrees") == 72 &&
              window->cameraControlValue("Bring_Up_Shadows") == 35 && lift_shadow_black_point->isChecked() &&
              window->cameraControlValue("Exposure_x100") == 60 && stitched_bring_up_shadows->value() == 35 &&
              stitched_lift_shadow_black_point->isChecked() && stitched_exposure->value() == 60,
          "Direct per-game baseline-key overrides should initialize every corresponding camera control")) {
    return false;
  }
  activate(reset);
  if (!expect(
          stop_delay->value() == 10 && zoom_in_aggressiveness->value() == 25 && rotate->value() == 90 &&
              bring_up_shadows->value() == 0 && exposure->value() == 0 && !lift_shadow_black_point->isChecked() &&
              stitched_bring_up_shadows->value() == 0 && stitched_exposure->value() == 0 &&
              !stitched_lift_shadow_black_point->isChecked() && save->isEnabled(),
          "Reset should stage removal of direct per-game canonical overrides")) {
    return false;
  }
  activate(save);
  const YAML::Node after_direct_reset = YAML::LoadFile(config.string());
  if (!expect(
          !lookup_yaml_path(after_direct_reset, {"rink", "camera", "stop_on_dir_change_delay"}, nullptr) &&
              !lookup_yaml_path(after_direct_reset, {"rink", "camera", "zoom_in_aggressiveness"}, nullptr) &&
              !lookup_yaml_path(after_direct_reset, {"stitching", "post_stitch_rotate_degrees"}, nullptr) &&
              !lookup_yaml_path(
                  after_direct_reset, {"pipeline", "hmplaycropper", "properties", "shadow-lift"}, nullptr) &&
              !lookup_yaml_path(
                  after_direct_reset,
                  {"pipeline", "hmplaycropper", "properties", "shadow-lift-black-point"},
                  nullptr) &&
              !lookup_yaml_path(after_direct_reset, {"pipeline", "hmplaycropper", "properties", "exposure"}, nullptr),
          "Reset plus Save should remove direct canonical values instead of letting them resurface on reload")) {
    return false;
  }
  activate(create);
  if (!expect(
          stop_delay->value() == 10 && zoom_in_aggressiveness->value() == 25 && rotate->value() == 90 &&
              bring_up_shadows->value() == 0 && exposure->value() == 0 && !lift_shadow_black_point->isChecked() &&
              stitched_bring_up_shadows->value() == 0 && stitched_exposure->value() == 0 &&
              !stitched_lift_shadow_black_point->isChecked() && !save->isEnabled(),
          "Reload after Reset plus Save should remain on bundled defaults")) {
    return false;
  }

  {
    YAML::Node shadow_precedence(YAML::NodeType::Map);
    shadow_precedence["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 35;
    shadow_precedence["pipeline"]["hmplaycropper"]["properties"]["shadow-lift-black-point"] = false;
    shadow_precedence["pipeline"]["hmplaycropper"]["properties"]["exposure"] = 0.6;
    shadow_precedence["hstream_ui"]["camera_controls"]["Bring_Up_Shadows"] = 45;
    shadow_precedence["hstream_ui"]["camera_controls"]["Lift_Shadow_Black_Point"] = 1;
    shadow_precedence["hstream_ui"]["camera_controls"]["Exposure_x100"] = 30;
    std::ofstream out(config);
    out << shadow_precedence << "\n";
  }
  activate(create);
  if (!expect(
          bring_up_shadows->value() == 45 && bring_up_shadows->minimum() == 0 && bring_up_shadows->maximum() == 100 &&
              lift_shadow_black_point->isChecked() && exposure->value() == 30 && exposure->minimum() == 0 &&
              exposure->maximum() == 130 && !save->isEnabled(),
          "Saved UI color controls should take precedence over canonical runtime values")) {
    return false;
  }

  {
    YAML::Node automatic_color(YAML::NodeType::Map);
    automatic_color["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = "auto";
    automatic_color["pipeline"]["hmstitcher"]["properties"]["shadow-lift"] = 25;
    automatic_color["pipeline"]["hmstitcher"]["properties"]["shadow-lift-black-point"] = true;
    automatic_color["pipeline"]["hmstitcher"]["properties"]["exposure"] = 0.3;
    std::ofstream out(config);
    out << automatic_color << "\n";
  }
  activate(create);
  if (!expect(
          use_10_bit_grading->checkState() == Qt::PartiallyChecked &&
              stitched_use_10_bit_grading->checkState() == Qt::PartiallyChecked && bring_up_shadows->value() == 25 &&
              stitched_bring_up_shadows->value() == 25 && lift_shadow_black_point->isChecked() &&
              stitched_lift_shadow_black_point->isChecked() && exposure->value() == 30 &&
              stitched_exposure->value() == 30 &&
              HStreamWindowTestAccess::pipelineArguments(window).contains(
                  "--options=hstream_ui.camera_controls.Bring_Up_Shadows=25") &&
              HStreamWindowTestAccess::pipelineArguments(window).contains(
                  "--options=hstream_ui.camera_controls.Lift_Shadow_Black_Point=1") &&
              HStreamWindowTestAccess::pipelineArguments(window).contains(
                  "--options=hstream_ui.camera_controls.Exposure_x100=30") &&
              !save->isEnabled(),
          "Automatic precision should load and launch native stitcher tones without becoming a forced override")) {
    return false;
  }
  bring_up_shadows->setValue(26);
  activate(save);
  const YAML::Node saved_automatic_color = YAML::LoadFile(config.string());
  if (!expect(
          !lookup_yaml_path(saved_automatic_color, {"pipeline", "hmstitcher", "properties", "high-bit-depth"}, nullptr),
          "Saving automatic precision should omit a native force override")) {
    return false;
  }

  {
    YAML::Node forced_standard_color(YAML::NodeType::Map);
    forced_standard_color["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = false;
    forced_standard_color["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 20;
    forced_standard_color["hstream_ui"]["camera_controls"]["Use_10_Bit_Grading"] = 1;
    std::ofstream out(config);
    out << forced_standard_color << "\n";
  }
  activate(create);
  if (!expect(
          use_10_bit_grading->checkState() == Qt::Unchecked &&
              stitched_use_10_bit_grading->checkState() == Qt::Unchecked && bring_up_shadows->value() == 20 &&
              HStreamWindowTestAccess::pipelineArguments(window).contains(
                  "--options=pipeline.hmstitcher.properties.high-bit-depth=0") &&
              !save->isEnabled(),
          "Native forced-standard precision should override a conflicting legacy UI value and launch explicitly off")) {
    return false;
  }
  bring_up_shadows->setValue(21);
  activate(save);
  const YAML::Node saved_forced_standard_color = YAML::LoadFile(config.string());
  YAML::Node saved_forced_standard_mode;
  if (!expect(
          lookup_yaml_path(
              saved_forced_standard_color,
              {"pipeline", "hmstitcher", "properties", "high-bit-depth"},
              &saved_forced_standard_mode) &&
              !saved_forced_standard_mode.as<bool>(),
          "Saving forced standard precision should persist an explicit false override")) {
    return false;
  }

  {
    YAML::Node high_bit_color(YAML::NodeType::Map);
    high_bit_color["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = true;
    high_bit_color["pipeline"]["hmstitcher"]["properties"]["shadow-lift"] = 35;
    high_bit_color["pipeline"]["hmstitcher"]["properties"]["shadow-lift-black-point"] = true;
    high_bit_color["pipeline"]["hmstitcher"]["properties"]["exposure"] = 0.6;
    high_bit_color["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 99;
    std::ofstream out(config);
    out << high_bit_color << "\n";
  }
  activate(create);
  if (!expect(
          use_10_bit_grading->isChecked() && bring_up_shadows->value() == 35 && lift_shadow_black_point->isChecked() &&
              exposure->value() == 60 && stitched_use_10_bit_grading->isChecked() &&
              stitched_bring_up_shadows->value() == 35 && stitched_lift_shadow_black_point->isChecked() &&
              stitched_exposure->value() == 60 && !save->isEnabled(),
          "High-bit presets should load grading controls from the FP16 stitcher instead of stale playcropper values")) {
    return false;
  }
  bring_up_shadows->setValue(45);
  activate(save);
  const YAML::Node saved_high_bit_color = YAML::LoadFile(config.string());
  YAML::Node saved_high_bit_enabled;
  YAML::Node saved_high_bit_shadow;
  if (!expect(
          lookup_yaml_path(
              saved_high_bit_color,
              {"pipeline", "hmstitcher", "properties", "high-bit-depth"},
              &saved_high_bit_enabled) &&
              saved_high_bit_enabled.as<bool>() &&
              lookup_yaml_path(
                  saved_high_bit_color,
                  {"pipeline", "hmstitcher", "properties", "shadow-lift"},
                  &saved_high_bit_shadow) &&
              saved_high_bit_shadow.as<int>() == 45 &&
              !lookup_yaml_path(
                  saved_high_bit_color, {"pipeline", "hmplaycropper", "properties", "shadow-lift"}, nullptr),
          "Saving high-bit grading should persist one tone owner and remove stale playcropper grading")) {
    return false;
  }

  for (const int numeric_value : {1, 0}) {
    YAML::Node numeric_black_point(YAML::NodeType::Map);
    numeric_black_point["pipeline"]["hmplaycropper"]["properties"]["shadow-lift-black-point"] = numeric_value;
    std::ofstream out(config);
    out << numeric_black_point << "\n";
    out.close();
    activate(create);
    if (!expect(
            lift_shadow_black_point->isChecked() == (numeric_value != 0) && !save->isEnabled(),
            "Canonical black-point lift should load strict numeric 1 and 0 forms")) {
      return false;
    }
  }

  {
    YAML::Node invalid_black_point(YAML::NodeType::Map);
    invalid_black_point["pipeline"]["hmplaycropper"]["properties"]["shadow-lift-black-point"] = 2;
    std::ofstream out(config);
    out << invalid_black_point << "\n";
  }
  activate(create);
  if (!expect(
          !lift_shadow_black_point->isChecked() && save->isEnabled(),
          "Canonical black-point lift should reject non-boolean numerics without overriding the default")) {
    return false;
  }

  {
    YAML::Node invalid_shadow_lift(YAML::NodeType::Map);
    invalid_shadow_lift["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 101;
    std::ofstream out(config);
    out << invalid_shadow_lift << "\n";
  }
  activate(create);
  if (!expect(
          bring_up_shadows->value() == 0 && bring_up_shadows->minimum() == 0 && bring_up_shadows->maximum() == 100 &&
              save->isEnabled(),
          "Out-of-range canonical shadow lift should fail closed without widening the slider")) {
    return false;
  }

  {
    YAML::Node fractional_shadow_lift(YAML::NodeType::Map);
    fractional_shadow_lift["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 35.5;
    std::ofstream out(config);
    out << fractional_shadow_lift << "\n";
  }
  activate(create);
  if (!expect(
          bring_up_shadows->value() == 0 && bring_up_shadows->minimum() == 0 && bring_up_shadows->maximum() == 100 &&
              save->isEnabled(),
          "Fractional canonical shadow lift should fail closed as incompatible with the integer control")) {
    return false;
  }

  {
    YAML::Node malformed(YAML::NodeType::Map);
    malformed["hstream_ui"]["camera_controls"]["Stop_Direction_Change_Delay_Frames"] = 11;
    malformed["hstream_ui"]["camera_controls"]["Max_Speed_X_x10"] = "not-an-integer";
    std::ofstream out(config);
    out << malformed << "\n";
  }
  activate(create);
  if (!expect(
          stop_delay->value() == 10,
          "Malformed saved controls should not partially apply values parsed before the error") ||
      !expect(save->isEnabled(), "Malformed saved controls should leave the current controls unsaved")) {
    return false;
  }
  {
    YAML::Node existing(YAML::NodeType::Map);
    existing["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    std::ofstream out(config);
    out << existing << "\n";
  }
  activate(create);
  if (!expect(
          fixed_edge_link->value() == 1 && fixed_edge_left->value() == 220 && fixed_edge_right->value() == 220,
          "Camera controls should recover after a malformed saved config is corrected") ||
      !expect(!save->isEnabled(), "Loading the corrected config should restore the clean preset snapshot")) {
    return false;
  }

  {
    YAML::Node fractional(YAML::NodeType::Map);
    fractional["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    fractional["stitching"]["stitch_frame_time"] = "00:10:07.500";
    std::ofstream out(config);
    out << fractional << "\n";
  }
  activate(create);
  if (!expect(
          stitch_frame_time->time() == QTime(0, 10, 7, 500),
          "Camera controls should load a fractional stitching.stitch_frame_time value") ||
      !expect(
          stitch_frame_time->displayFormat() == "HH:mm:ss.zzz",
          "A fractional stitch-frame time should display milliseconds") ||
      !expect(!save->isEnabled(), "A fractional stitch-frame time should participate in the clean preset snapshot")) {
    return false;
  }

  {
    YAML::Node nonscalar(YAML::NodeType::Map);
    nonscalar["stitching"]["stitch_frame_time"].push_back("00:00:07");
    std::ofstream out(config);
    out << nonscalar << "\n";
  }
  activate(create);
  if (!expect(
          stitch_frame_time->time() == QTime(0, 0, 0),
          "A non-scalar stitch-frame time should fail closed to the default") ||
      !expect(save->isEnabled(), "A non-scalar stitch-frame time should leave the current controls unsaved")) {
    return false;
  }
  {
    YAML::Node existing(YAML::NodeType::Map);
    existing["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    std::ofstream out(config);
    out << existing << "\n";
  }
  activate(create);

  {
    YAML::Node out_of_range_link(YAML::NodeType::Map);
    out_of_range_link["hstream_ui"]["camera_controls"]["Link_Fixed_Edge_Rotation_Left_Right"] = -1;
    out_of_range_link["hstream_ui"]["camera_controls"]["Left_Fixed_Edge_Rotation_Angle_x10"] = 250;
    out_of_range_link["hstream_ui"]["camera_controls"]["Right_Fixed_Edge_Rotation_Angle_x10"] = 750;
    std::ofstream out(config);
    out << out_of_range_link << "\n";
  }
  activate(create);
  if (!expect(
          fixed_edge_link->value() == 1 && fixed_edge_left->value() == 100 && fixed_edge_right->value() == 100 &&
              save->isEnabled(),
          "Out-of-domain selector values should be rejected visibly without partially applying the preset")) {
    return false;
  }
  {
    YAML::Node expanded_range(YAML::NodeType::Map);
    expanded_range["rink"]["camera"]["stop_on_dir_change_delay"] = 100;
    std::ofstream out(config);
    out << expanded_range << "\n";
  }
  activate(create);
  if (!expect(
          stop_delay->value() == 100 && stop_delay->maximum() >= 100 && !save->isEnabled(),
          "A valid per-game value beyond the initial slider range should be represented exactly")) {
    return false;
  }
  activate(reset);
  activate(save);
  activate(create);
  const YAML::Node after_expanded_reset = YAML::LoadFile(config.string());
  if (!expect(
          stop_delay->value() == 10 && !save->isEnabled() &&
              !lookup_yaml_path(after_expanded_reset, {"rink", "camera", "stop_on_dir_change_delay"}, nullptr),
          "Reset plus Save should normalize an expanded-range canonical override back to the bundled default")) {
    return false;
  }
  {
    YAML::Node existing(YAML::NodeType::Map);
    existing["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    std::ofstream out(config);
    out << existing << "\n";
  }
  activate(create);

  {
    const fs::path rotation_only_rink_mask = fs::path(window->gameDirectoryText().toStdString()) / "rink_mask_0.png";
    const fs::path rotation_only_snapshot = fs::path(window->gameDirectoryText().toStdString()) / "s.png";
    YAML::Node rotation_only = YAML::LoadFile(config.string());
    rotation_only["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    rotation_only["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "complete";
    rotation_only["rink"]["ice_contours_mask_count"] = 1;
    rotation_only["rink"]["stitched_output_generation"] = "stale-generation";
    rotation_only["rink"]["stitched_output_persisted_rotation_degrees"] = 1.0;
    rotation_only["rink"]["stitched_output_pending_generation"] = "pending-generation";
    {
      std::ofstream out(config);
      out << rotation_only << "\n";
    }
    {
      std::ofstream out(rotation_only_rink_mask);
      out << "rotation-only-mask";
    }
    {
      std::ofstream out(rotation_only_snapshot);
      out << "rotation-only-snapshot";
    }
    activate(create);
    rotate->setValue(rotate->value() - 1);
    activate(save);
    const YAML::Node after_rotation_only_save = YAML::LoadFile(config.string());
    const YAML::Node rotation_only_calibration = after_rotation_only_save["hstream_ui"]["stitching_calibration"];
    if (!expect(
            !fs::exists(rotation_only_rink_mask), "A rotation-only preset save should remove the stale rink mask") ||
        !expect(!fs::exists(rotation_only_snapshot), "A rotation-only preset save should remove the stale snapshot") ||
        !expect(
            !after_rotation_only_save["rink"]["stitched_output_generation"].IsDefined(),
            "A rotation-only preset save should clear the stale stitched-output generation") ||
        !expect(
            !after_rotation_only_save["rink"]["stitched_output_persisted_rotation_degrees"].IsDefined(),
            "A rotation-only preset save should clear the stale persisted-rotation marker") ||
        !expect(
            !after_rotation_only_save["rink"]["stitched_output_pending_generation"].IsDefined(),
            "A rotation-only preset save should clear the pending live generation") ||
        !expect(
            rotation_only_calibration["status"].as<std::string>("") == "complete",
            "A rotation-only preset save should preserve completed stitch-map status") ||
        !expect(
            rotation_only_calibration["rink_mask_status"].as<std::string>("") == "pending",
            "A rotation-only preset save should mark the removed rink mask pending")) {
      return false;
    }
  }

  {
    YAML::Node invalid_exposure(YAML::NodeType::Map);
    invalid_exposure["pipeline"]["hmplaycropper"]["properties"]["exposure"] = 1.31;
    std::ofstream out(config);
    out << invalid_exposure << "\n";
  }
  activate(create);
  if (!expect(
          exposure->value() == 0 && exposure->minimum() == 0 && exposure->maximum() == 130 && save->isEnabled(),
          "Out-of-range canonical exposure should fail closed without widening the slider")) {
    return false;
  }
  {
    YAML::Node existing(YAML::NodeType::Map);
    existing["rink"]["camera"]["fixed_edge_rotation_angle"] = 22.0;
    std::ofstream out(config);
    out << existing << "\n";
  }
  activate(create);

  const QString loaded_game_id = game_id->text();
  stop_delay->setValue(1);
  if (!expect(save->isEnabled(), "Changing a loaded preset should enable Save Preset")) {
    return false;
  }
  game_id->clear();
  if (!expect(!save->isEnabled(), "Save Preset should disable when there is no game to receive the changes")) {
    return false;
  }
  game_id->setText(loaded_game_id);
  if (!expect(save->isEnabled(), "Restoring the destination game should expose the still-unsaved change")) {
    return false;
  }
  stop_delay->setValue(10);
  if (!expect(!save->isEnabled(), "Reverting a control to its loaded value should disable Save Preset")) {
    return false;
  }

  rotate->setValue(72);
  fixed_edge_left->setValue(250);
  fixed_edge_link->setValue(0);
  fixed_edge_right->setValue(750);
  stop_delay->setValue(14);
  max_speed_x->setValue(450);
  bring_up_shadows->setValue(35);
  exposure->setValue(60);
  lift_shadow_black_point->setChecked(true);
  stitch_frame_time->setTime(QTime(0, 0, 7));
  if (!expect(
          window->cameraControlValue("Stitch_Rotate_Degrees") == 72,
          "Stitch rotation slider should update controller state") ||
      !expect(
          window->cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") == 0 &&
              window->cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10") == 250 &&
              window->cameraControlValue("Right_Fixed_Edge_Rotation_Angle_x10") == 750,
          "Fixed-edge rotation should support independently configured left and right angles") ||
      !expect(
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 14,
          "Tracker braking slider should update controller state") ||
      !expect(window->cameraControlValue("Max_Speed_X_x10") == 450, "Speed slider should update controller state") ||
      !expect(
          window->cameraControlValue("Bring_Up_Shadows") == 35 && bring_up_shadows->minimum() == 0 &&
              bring_up_shadows->maximum() == 100,
          "Bring up shadows should expose a bounded percentage control") ||
      !expect(
          window->cameraControlValue("Lift_Shadow_Black_Point") == 1 && lift_shadow_black_point->isChecked(),
          "Black-point lift should expose a boolean checkbox") ||
      !expect(
          window->cameraControlValue("Exposure_x100") == 60 && exposure->minimum() == 0 && exposure->maximum() == 130,
          "Exposure should expose the measured 0.00 through 1.30 range in hundredths") ||
      !expect(
          stitch_frame_time->time() == QTime(0, 0, 7),
          "Stitch-frame control should accept an HH:MM:SS calibration timestamp") ||
      !expect(save->isEnabled(), "Changing a preset-backed control should enable Save Preset")) {
    return false;
  }

  const int stop_delay_before_wheel = stop_delay->value();
  QWheelEvent wheel_event(
      stop_delay->rect().center(),
      stop_delay->mapToGlobal(stop_delay->rect().center()),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(stop_delay, &wheel_event);
  QApplication::processEvents();
  if (!expect(
          stop_delay->value() == stop_delay_before_wheel,
          "Mouse wheel over camera slider should not change live camera control")) {
    return false;
  }

  const fs::path rink_mask = fs::path(window->gameDirectoryText().toStdString()) / "rink_mask_0.png";
  {
    YAML::Node seeded(YAML::NodeType::Map);
    YAML::Node polygon(YAML::NodeType::Sequence);
    polygon.push_back(0);
    polygon.push_back(1);
    seeded["rink"]["scoreboard"]["perspective_polygon"] = polygon;
    seeded["rink"]["ice_contours_mask_count"] = 1;
    seeded["rink"]["ice_contours_mask_centroid"] = "10,20";
    seeded["rink"]["ice_contours_combined_bbox"] = "0,0,100,50";
    std::ofstream out(config);
    out << seeded << "\n";
  }
  {
    std::ofstream out(rink_mask);
    out << "stale-mask";
  }

  const fs::path runtime_dir_collision = fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui";
  {
    std::ofstream out(runtime_dir_collision);
    out << "directory-collision";
  }
  std::ifstream config_before_failed_save_input(config, std::ios::binary);
  const std::string config_before_failed_save(
      (std::istreambuf_iterator<char>(config_before_failed_save_input)), std::istreambuf_iterator<char>());
  activate(save);
  std::ifstream config_after_failed_save_input(config, std::ios::binary);
  const std::string config_after_failed_save(
      (std::istreambuf_iterator<char>(config_after_failed_save_input)), std::istreambuf_iterator<char>());
  if (!expect(save->isEnabled(), "A failed playtracker sidecar write should keep Save Preset enabled") ||
      !expect(
          config_after_failed_save == config_before_failed_save,
          "A failed playtracker sidecar write should not publish a partially updated config.yaml")) {
    return false;
  }
  fs::remove(runtime_dir_collision);

  activate(save);
  if (!expect(!save->isEnabled(), "A successful preset save should disable Save Preset until another change")) {
    return false;
  }

  const YAML::Node committed_before_publish_failure = YAML::LoadFile(config.string());
  YAML::Node committed_sidecar_node;
  if (!expect(
          lookup_yaml_path(
              committed_before_publish_failure,
              {"pipeline", "ds-playtracker", "config-file"},
              &committed_sidecar_node) &&
              committed_sidecar_node.IsScalar(),
          "A saved tracker preset should reference its immutable sidecar")) {
    return false;
  }
  const fs::path committed_sidecar = committed_sidecar_node.as<std::string>();
  std::ifstream committed_config_input(config, std::ios::binary);
  const std::string committed_config(
      (std::istreambuf_iterator<char>(committed_config_input)), std::istreambuf_iterator<char>());
  std::ifstream committed_sidecar_input(committed_sidecar, std::ios::binary);
  const std::string committed_sidecar_contents(
      (std::istreambuf_iterator<char>(committed_sidecar_input)), std::istreambuf_iterator<char>());
  max_speed_x->setValue(451);
  qputenv("HSTREAM_UI_TEST_FAIL_PRESET_RETIREMENT_PUBLISH", "1");
  activate(save);
  qunsetenv("HSTREAM_UI_TEST_FAIL_PRESET_RETIREMENT_PUBLISH");
  const YAML::Node after_failed_retirement = YAML::LoadFile(config.string());
  YAML::Node after_failed_retirement_sidecar;
  const QStringList sidecars_after_failed_retirement =
      QDir(QString::fromStdString(committed_sidecar.parent_path().string()))
          .entryList({"play_tracker_config_*.yaml"}, QDir::Files, QDir::Name);
  if (!expect(save->isEnabled(), "A failed retirement marker should keep Save Preset enabled") ||
      !expect(
          lookup_yaml_path(
              after_failed_retirement,
              {"pipeline", "ds-playtracker", "config-file"},
              &after_failed_retirement_sidecar) &&
              after_failed_retirement_sidecar.IsScalar() &&
              after_failed_retirement_sidecar.as<std::string>() == committed_sidecar.string(),
          "A failed retirement marker should leave the old sidecar active") ||
      !expect(
          sidecars_after_failed_retirement.size() == 1,
          "A failed retirement marker should remove the unpublished replacement generation")) {
    return false;
  }
  qputenv("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_PUBLISH", "1");
  activate(save);
  qunsetenv("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_PUBLISH");
  std::ifstream failed_publish_config_input(config, std::ios::binary);
  const std::string failed_publish_config(
      (std::istreambuf_iterator<char>(failed_publish_config_input)), std::istreambuf_iterator<char>());
  std::ifstream failed_publish_sidecar_input(committed_sidecar, std::ios::binary);
  const std::string failed_publish_sidecar(
      (std::istreambuf_iterator<char>(failed_publish_sidecar_input)), std::istreambuf_iterator<char>());
  const QStringList persistent_sidecars = QDir(QString::fromStdString(committed_sidecar.parent_path().string()))
                                              .entryList({"play_tracker_config_*.yaml"}, QDir::Files, QDir::Name);
  if (!expect(save->isEnabled(), "A failed config commit should keep the changed preset savable") ||
      !expect(failed_publish_config == committed_config, "A failed config commit should preserve config.yaml") ||
      !expect(
          failed_publish_sidecar == committed_sidecar_contents,
          "A failed config commit should preserve the effective playtracker sidecar") ||
      !expect(
          persistent_sidecars.size() == 1,
          "A failed config commit should remove its unpublished playtracker sidecar generation")) {
    return false;
  }

  fs::last_write_time(committed_sidecar, fs::file_time_type::clock::now() - std::chrono::hours(25));
  qputenv("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_POST_COMMIT", "1");
  activate(save);
  qunsetenv("HSTREAM_UI_TEST_FAIL_PRESET_CONFIG_POST_COMMIT");
  const YAML::Node visible_after_post_commit_error = YAML::LoadFile(config.string());
  YAML::Node visible_sidecar_node;
  const bool has_visible_sidecar = lookup_yaml_path(
      visible_after_post_commit_error, {"pipeline", "ds-playtracker", "config-file"}, &visible_sidecar_node);
  const fs::path visible_sidecar = has_visible_sidecar && visible_sidecar_node.IsScalar()
      ? fs::path(visible_sidecar_node.as<std::string>())
      : fs::path();
  if (!expect(save->isEnabled(), "A post-commit durability error should keep Save Preset enabled") ||
      !expect(
          !visible_sidecar.empty() && visible_sidecar != committed_sidecar && fs::exists(visible_sidecar),
          "A visible post-commit config generation should retain its referenced sidecar") ||
      !expect(
          fs::exists(committed_sidecar),
          "A prior config reader should retain access to its immutable sidecar after a later save")) {
    return false;
  }
  max_speed_x->setValue(450);
  if (!expect(
          save->isEnabled(),
          "Reverting a control after a post-commit error should stay dirty against the visible generation")) {
    return false;
  }
  max_speed_x->setValue(451);
  if (!expect(save->isEnabled(), "A visible generation with uncertain durability should keep retry enabled")) {
    return false;
  }
  activate(create);
  if (!expect(
          max_speed_x->value() == 451 && save->isEnabled(),
          "Reloading the same visible generation should preserve its durability retry requirement")) {
    return false;
  }
  const QString durability_retry_game_id = game_id->text();
  game_id->setText("ui-camera-control-other-game");
  activate(create);
  if (!expect(
          !save->isEnabled() && !save->toolTip().contains("Retry saving"),
          "A different game should not inherit another game's durability retry requirement")) {
    return false;
  }
  game_id->setText(durability_retry_game_id);
  activate(create);
  if (!expect(
          max_speed_x->value() == 451 && save->isEnabled() && save->toolTip().contains("Retry saving"),
          "Returning to a game should restore its pending durability retry requirement")) {
    return false;
  }
  activate(save);
  if (!expect(!save->isEnabled(), "A successful durability retry should clear the retry-required state")) {
    return false;
  }
  stitch_max_output_width->setValue(4096);
  game_id->setText("ui-camera-control-empty-game");
  activate(create);
  if (!expect(
          stitch_max_output_width->value() == 0 && !save->isEnabled(),
          "A game without config.yaml must reset max stitched width to the effective default instead of inheriting the "
          "previous game")) {
    return false;
  }
  game_id->setText(durability_retry_game_id);
  activate(create);
  YAML::Node generated_backend_marker_config = YAML::LoadFile(config.string());
  generated_backend_marker_config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  generated_backend_marker_config["stitching"]["mapping_backend"] = "nona";
  generated_backend_marker_config["stitching"].remove("projection");
  generated_backend_marker_config["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] =
      "superpoint-lightglue";
  generated_backend_marker_config["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = "nona";
  YAML::Node generated_choices = generated_backend_marker_config["hstream_ui"]["generated_stitching_backend_choices"];
  generated_choices.remove("projection");
  generated_choices["previous_mapping_backend"] = "opencv-magsac";
  generated_choices.remove("previous_projection");
  std::ofstream(config) << YAML::Dump(generated_backend_marker_config) << '\n';
  activate(create);
  if (!expect(
          mapping_backend->currentData().toString() == "opencv-magsac" &&
              projection->currentData().toString() == "rectilinear" && !save->isEnabled(),
          "Legacy generated OpenCV backend provenance without a projection must migrate to rectilinear")) {
    return false;
  }
  max_speed_x->setValue(450);
  activate(save);
  if (!expect(!save->isEnabled(), "A successful retry should restore the intended saved snapshot") ||
      !expect(
          fs::exists(committed_sidecar),
          "Recent superseded sidecars should remain available to delayed pipeline readers")) {
    return false;
  }
  const fs::path committed_retirement_marker =
      committed_sidecar.parent_path() / (".retired-" + committed_sidecar.filename().string());
  fs::last_write_time(committed_retirement_marker, fs::file_time_type::clock::now() - std::chrono::hours(25));

  stop_delay->setValue(15);
  if (!expect(save->isEnabled(), "A new change after saving should re-enable Save Preset")) {
    return false;
  }
  stop_delay->setValue(14);
  if (!expect(!save->isEnabled(), "Reverting to the saved value should clear the preset dirty state")) {
    return false;
  }
  YAML::Node saved = YAML::LoadFile(config.string());
  const bool removed_generated_backend_marker =
      !lookup_yaml_path(saved, {"hstream_ui", "generated_stitching_backend_choices"}, nullptr);
  const bool removed_rink_mask = !fs::exists(rink_mask);
  const bool removed_scoreboard_polygon =
      !lookup_yaml_path(saved, {"rink", "scoreboard", "perspective_polygon"}, nullptr);
  const bool removed_ice_mask_keys = !lookup_yaml_path(saved, {"rink", "ice_contours_mask_count"}, nullptr) &&
      !lookup_yaml_path(saved, {"rink", "ice_contours_mask_centroid"}, nullptr) &&
      !lookup_yaml_path(saved, {"rink", "ice_contours_combined_bbox"}, nullptr);
  auto saved_int = [&](const char* key, int expected) {
    YAML::Node value;
    if (!lookup_yaml_path(saved, {"hstream_ui", "camera_controls", key}, &value)) {
      return false;
    }
    return value && value.IsScalar() && value.as<int>() == expected;
  };
  const bool saved_controls_ok = saved_int("Stop_Direction_Change_Delay_Frames", 14) &&
      saved_int("Stitch_Rotate_Degrees", 72) && saved_int("Link_Fixed_Edge_Rotation_Left_Right", 0) &&
      saved_int("Left_Fixed_Edge_Rotation_Angle_x10", 250) && saved_int("Right_Fixed_Edge_Rotation_Angle_x10", 750) &&
      saved_int("Bring_Up_Shadows", 35) && saved_int("Lift_Shadow_Black_Point", 1) && saved_int("Exposure_x100", 60);
  YAML::Node saved_rotation;
  const bool has_saved_rotation = lookup_yaml_path(saved, {"stitching", "post_stitch_rotate_degrees"}, &saved_rotation);
  const bool saved_rotation_ok = saved_rotation && saved_rotation.IsScalar() && saved_rotation.as<int>() == 18;
  YAML::Node saved_shadow_lift;
  const bool saved_shadow_lift_ok =
      lookup_yaml_path(saved, {"pipeline", "hmplaycropper", "properties", "shadow-lift"}, &saved_shadow_lift) &&
      saved_shadow_lift.IsScalar() && saved_shadow_lift.as<int>() == 35;
  YAML::Node saved_shadow_black_point;
  const bool saved_shadow_black_point_ok =
      lookup_yaml_path(
          saved, {"pipeline", "hmplaycropper", "properties", "shadow-lift-black-point"}, &saved_shadow_black_point) &&
      saved_shadow_black_point.IsScalar() && saved_shadow_black_point.as<bool>();
  YAML::Node saved_exposure;
  const bool saved_exposure_ok =
      lookup_yaml_path(saved, {"pipeline", "hmplaycropper", "properties", "exposure"}, &saved_exposure) &&
      saved_exposure.IsScalar() && std::abs(saved_exposure.as<double>() - 0.6) < 1e-9;
  YAML::Node saved_stitch_frame_time;
  const bool saved_stitch_frame_time_ok =
      lookup_yaml_path(saved, {"stitching", "stitch_frame_time"}, &saved_stitch_frame_time) &&
      saved_stitch_frame_time.IsScalar() && saved_stitch_frame_time.as<std::string>() == "00:00:07";
  YAML::Node saved_calibration_control_points;
  YAML::Node saved_calibration_status;
  YAML::Node saved_calibration_stale_from;
  YAML::Node saved_calibration_artifacts_invalidated;
  YAML::Node saved_calibration_invalidation_id;
  const bool stitch_frame_time_invalidated_calibration =
      lookup_yaml_path(
          saved, {"hstream_ui", "stitching_calibration", "control_points"}, &saved_calibration_control_points) &&
      saved_calibration_control_points.IsScalar() && saved_calibration_control_points.as<int>() == 1500 &&
      lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "status"}, &saved_calibration_status) &&
      saved_calibration_status.IsScalar() && saved_calibration_status.as<std::string>() == "pending" &&
      lookup_yaml_path(saved, {"hstream_ui", "stitching_calibration", "stale_from"}, &saved_calibration_stale_from) &&
      saved_calibration_stale_from.IsScalar() && saved_calibration_stale_from.as<std::string>() == "input" &&
      lookup_yaml_path(
          saved,
          {"hstream_ui", "stitching_calibration", "artifacts_invalidated"},
          &saved_calibration_artifacts_invalidated) &&
      saved_calibration_artifacts_invalidated.IsScalar() && !saved_calibration_artifacts_invalidated.as<bool>() &&
      lookup_yaml_path(
          saved, {"hstream_ui", "stitching_calibration", "invalidation_id"}, &saved_calibration_invalidation_id) &&
      saved_calibration_invalidation_id.IsScalar() && !saved_calibration_invalidation_id.as<std::string>().empty();
  YAML::Node saved_fixed_edge_rotation;
  const bool has_saved_fixed_edge_rotation =
      lookup_yaml_path(saved, {"rink", "camera", "fixed_edge_rotation_angle"}, &saved_fixed_edge_rotation);
  const bool saved_fixed_edge_rotation_ok = has_saved_fixed_edge_rotation && saved_fixed_edge_rotation.IsSequence() &&
      saved_fixed_edge_rotation.size() == 2 && saved_fixed_edge_rotation[0].as<double>() == 25.0 &&
      saved_fixed_edge_rotation[1].as<double>() == 75.0;
  YAML::Node saved_playtracker_config_path;
  const bool has_saved_playtracker_config_path =
      lookup_yaml_path(saved, {"pipeline", "ds-playtracker", "config-file"}, &saved_playtracker_config_path);
  const fs::path playtracker_config_path =
      has_saved_playtracker_config_path ? fs::path(saved_playtracker_config_path.as<std::string>()) : fs::path();
  YAML::Node playtracker_config = has_saved_playtracker_config_path && fs::exists(playtracker_config_path)
      ? YAML::LoadFile(playtracker_config_path.string())
      : YAML::Node();
  YAML::Node live_boxes;
  const bool has_live_boxes = lookup_yaml_path(playtracker_config, {"play-tracker", "live-boxes"}, &live_boxes);
  YAML::Node follower_max_speed_x;
  YAML::Node follower_max_speed_y;
  YAML::Node follower_max_accel_x;
  YAML::Node follower_max_accel_y;
  YAML::Node fast_max_speed_x;
  const bool saved_follower_max_speed_x = has_live_boxes && live_boxes.IsSequence() && live_boxes.size() > 1 &&
      lookup_yaml_key(live_boxes[1], "max-speed-x", &follower_max_speed_x) && follower_max_speed_x.IsScalar() &&
      follower_max_speed_x.as<double>() == 45.0;
  const bool saved_follower_max_speed_y = has_live_boxes && live_boxes.IsSequence() && live_boxes.size() > 1 &&
      lookup_yaml_key(live_boxes[1], "max-speed-y", &follower_max_speed_y);
  const bool saved_follower_max_accel_x = has_live_boxes && live_boxes.IsSequence() && live_boxes.size() > 1 &&
      lookup_yaml_key(live_boxes[1], "max-accel-x", &follower_max_accel_x);
  const bool saved_follower_max_accel_y = has_live_boxes && live_boxes.IsSequence() && live_boxes.size() > 1 &&
      lookup_yaml_key(live_boxes[1], "max-accel-y", &follower_max_accel_y);
  const bool saved_fast_max_speed_x = has_live_boxes && live_boxes.IsSequence() && live_boxes.size() > 0 &&
      lookup_yaml_key(live_boxes[0], "max-speed-x", &fast_max_speed_x);
  const bool has_default_follower =
      lookup_yaml_path(saved, {"hstream_ui", "camera_controls", "Apply_To_Follower_Box"}, nullptr);
  if (!saved_controls_ok) {
    std::cerr << saved << '\n';
  }
  if (!expect(window->logText().contains("preset saved"), "Save preset button should log persistence") ||
      !expect(saved_controls_ok, "Save preset should persist non-default control values") ||
      !expect(!has_default_follower, "Save preset should omit default control values") ||
      !expect(
          saved_stitch_frame_time_ok, "Save preset should persist a non-default stitching.stitch_frame_time value") ||
      !expect(
          removed_generated_backend_marker,
          "Save preset should make UI-owned stitching matcher/backend choices explicit") ||
      !expect(
          stitch_frame_time_invalidated_calibration,
          "Changing stitch-frame time should reserve a backend-visible input invalidation owner") ||
      !expect(has_saved_rotation && saved_rotation_ok, "Stitch slider should save the runtime rotation config") ||
      !expect(saved_shadow_lift_ok, "Shadow slider should save the playcropper GPU property") ||
      !expect(saved_shadow_black_point_ok, "Black-point checkbox should save the playcropper GPU property") ||
      !expect(saved_exposure_ok, "Exposure slider should save the measured gain property") ||
      !expect(
          saved_fixed_edge_rotation_ok,
          "Unlinked fixed-edge sliders should save rink.camera.fixed_edge_rotation_angle as [left, right]") ||
      !expect(removed_rink_mask, "Saving stitch rotation should remove stale rink mask image") ||
      !expect(removed_scoreboard_polygon, "Saving stitch rotation should invalidate scoreboard perspective") ||
      !expect(removed_ice_mask_keys, "Saving stitch rotation should invalidate cached ice-mask metadata") ||
      !expect(
          has_saved_playtracker_config_path && saved_follower_max_speed_x && !saved_follower_max_speed_y &&
              !saved_follower_max_accel_x && !saved_follower_max_accel_y && !saved_fast_max_speed_x,
          "Speed slider should save only changed follower playtracker runtime config")) {
    if (!has_saved_rotation || !saved_rotation_ok || !saved_follower_max_speed_x || saved_follower_max_speed_y ||
        saved_follower_max_accel_x || saved_follower_max_accel_y || saved_fast_max_speed_x) {
      std::cerr << saved << '\n';
      std::cerr << playtracker_config << '\n';
    }
    return false;
  }

  {
    YAML::Node same_rotation = saved;
    YAML::Node polygon(YAML::NodeType::Sequence);
    polygon.push_back(2);
    polygon.push_back(3);
    same_rotation["rink"]["scoreboard"]["perspective_polygon"] = polygon;
    same_rotation["rink"]["ice_contours_mask_count"] = 1;
    same_rotation["rink"]["ice_contours_mask_centroid"] = "30,40";
    same_rotation["rink"]["ice_contours_combined_bbox"] = "0,0,120,60";
    {
      std::ofstream out(config);
      out << same_rotation << "\n";
    }
    {
      std::ofstream out(rink_mask);
      out << "fresh-mask";
    }
    max_accel_x->setValue(10);
    if (!expect(save->isEnabled(), "A non-rotation control change should make the externally updated preset savable")) {
      return false;
    }
    activate(save);
    YAML::Node after_same_rotation_save = YAML::LoadFile(config.string());
    const bool kept_rink_mask = fs::exists(rink_mask);
    const bool kept_scoreboard_polygon =
        lookup_yaml_path(after_same_rotation_save, {"rink", "scoreboard", "perspective_polygon"}, nullptr);
    const bool kept_ice_mask_keys =
        lookup_yaml_path(after_same_rotation_save, {"rink", "ice_contours_mask_count"}, nullptr) &&
        lookup_yaml_path(after_same_rotation_save, {"rink", "ice_contours_mask_centroid"}, nullptr) &&
        lookup_yaml_path(after_same_rotation_save, {"rink", "ice_contours_combined_bbox"}, nullptr);
    if (!expect(kept_rink_mask, "Saving unchanged stitch rotation should preserve rink mask image") ||
        !expect(kept_scoreboard_polygon, "Saving unchanged stitch rotation should preserve scoreboard perspective") ||
        !expect(kept_ice_mask_keys, "Saving unchanged stitch rotation should preserve cached ice-mask metadata") ||
        !expect(
            !fs::exists(committed_sidecar),
            "A later successful save should garbage-collect superseded sidecars after the reader grace period")) {
      std::cerr << after_same_rotation_save << '\n';
      return false;
    }
    max_accel_x->setValue(0);
    activate(save);
    saved = YAML::LoadFile(config.string());
    if (!expect(!save->isEnabled(), "Restoring and saving a control should leave the preset clean")) {
      return false;
    }
  }

  YAML::Node stitching;
  lookup_yaml_path(saved, {"stitching"}, &stitching);
  YAML::Node stitching_copy = stitching && stitching.IsMap() ? YAML::Clone(stitching) : YAML::Node(YAML::NodeType::Map);
  YAML::Node right;
  lookup_yaml_path(stitching_copy, {"right"}, &right);
  YAML::Node right_copy = right && right.IsMap() ? YAML::Clone(right) : YAML::Node(YAML::NodeType::Map);
  YAML::Node color;
  lookup_yaml_path(right_copy, {"color"}, &color);
  YAML::Node color_copy = color && color.IsMap() ? YAML::Clone(color) : YAML::Node(YAML::NodeType::Map);
  color_copy["gamma"] = 1.75;
  right_copy["color"] = color_copy;
  stitching_copy["right"] = right_copy;
  YAML::Node left;
  lookup_yaml_path(stitching_copy, {"left"}, &left);
  YAML::Node left_copy = left && left.IsMap() ? YAML::Clone(left) : YAML::Node(YAML::NodeType::Map);
  YAML::Node left_color;
  lookup_yaml_path(left_copy, {"color"}, &left_color);
  YAML::Node left_color_copy =
      left_color && left_color.IsMap() ? YAML::Clone(left_color) : YAML::Node(YAML::NodeType::Map);
  left_color_copy["gamma"] = 1.75;
  left_copy["color"] = left_color_copy;
  stitching_copy["left"] = left_copy;
  saved["stitching"] = stitching_copy;
  const fs::path custom_playtracker_config =
      fs::path(window->gameDirectoryText().toStdString()) / "custom_playtracker.yaml";
  {
    YAML::Node custom_tracker(YAML::NodeType::Map);
    YAML::Node live_boxes_custom(YAML::NodeType::Sequence);
    YAML::Node fast_box(YAML::NodeType::Map);
    fast_box["name"] = "current_roi";
    YAML::Node follower_box(YAML::NodeType::Map);
    follower_box["name"] = "current_roi_aspect";
    follower_box["max-speed-y"] = 77.0;
    follower_box["sticky-translation-gaussian-mult"] = 9.5;
    live_boxes_custom.push_back(fast_box);
    live_boxes_custom.push_back(follower_box);
    custom_tracker["play-tracker"]["live-boxes"] = live_boxes_custom;
    std::ofstream out(custom_playtracker_config);
    out << custom_tracker << "\n";
  }
  saved["pipeline"]["ds-playtracker"]["config-file"] = custom_playtracker_config.string();
  {
    std::ofstream out(config);
    out << saved << "\n";
  }

  activate(reset);
  if (!expect(
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 10,
          "Reset should restore the bundled baseline tracker default") ||
      !expect(window->cameraControlValue("Bring_Up_Shadows") == 0, "Reset should disable shadow lift") ||
      !expect(window->cameraControlValue("Exposure_x100") == 0, "Reset should disable exposure") ||
      !expect(
          window->cameraControlValue("Lift_Shadow_Black_Point") == 0 && !lift_shadow_black_point->isChecked(),
          "Reset should disable black-point lift")) {
    return false;
  }

  activate(create);
  if (!expect(
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 14,
          "Create/Load should restore saved native controls") ||
      !expect(window->cameraControlValue("Stitch_Rotate_Degrees") == 72, "Create/Load should restore stitch control") ||
      !expect(window->cameraControlValue("Bring_Up_Shadows") == 35, "Create/Load should restore shadow lift") ||
      !expect(window->cameraControlValue("Exposure_x100") == 60, "Create/Load should restore exposure") ||
      !expect(
          window->cameraControlValue("Lift_Shadow_Black_Point") == 1 && lift_shadow_black_point->isChecked(),
          "Create/Load should restore black-point lift") ||
      !expect(
          window->cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") == 0 &&
              window->cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10") == 250 &&
              window->cameraControlValue("Right_Fixed_Edge_Rotation_Angle_x10") == 750,
          "Create/Load should restore independent fixed-edge rotation angles")) {
    return false;
  }

  YAML::Node relative_runtime_config = YAML::LoadFile(config.string());
  relative_runtime_config["pipeline"]["ds-playtracker"]["config-file"] = ".hstream-ui/play_tracker_config.yaml";
  relative_runtime_config["pipeline"]["hmplaycropper"]["properties"]["shadow-lift"] = 35;
  relative_runtime_config["pipeline"]["hmplaycropper"]["properties"]["shadow-lift-black-point"] = false;
  relative_runtime_config["pipeline"]["hmplaycropper"]["properties"]["exposure"] = 0.6;
  relative_runtime_config["hstream_ui"]["camera_controls"]["Bring_Up_Shadows"] = 45;
  relative_runtime_config["hstream_ui"]["camera_controls"]["Lift_Shadow_Black_Point"] = 1;
  relative_runtime_config["hstream_ui"]["camera_controls"]["Exposure_x100"] = 30;
  relative_runtime_config["hstream_ui"]["playtracker_config_base"] = custom_playtracker_config.string();
  {
    std::ofstream out(config);
    out << relative_runtime_config << "\n";
  }
  activate(create);
  if (!expect(
          bring_up_shadows->value() == 45 && lift_shadow_black_point->isChecked() && exposure->value() == 30 &&
              !save->isEnabled(),
          "Reload should select companion color controls over stale canonical values before launch")) {
    return false;
  }
  mode->setCurrentIndex(mode->findData("program"));
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->pipelineStateText() == "PLAYING", "Fake runner should start for live playtracker control test")) {
    return false;
  }
  if (!expect(
          window->logText().contains("--stitch-frame-time=00:00:07"),
          "A saved non-default stitch-frame time should be passed to hstream-cli")) {
    activate(stop);
    return false;
  }
  if (!expect(
          window->logText().contains("--options=hstream_ui.camera_controls.Bring_Up_Shadows=45"),
          "Program launch should override stale canonical shadow lift with the effective UI value")) {
    activate(stop);
    return false;
  }
  if (!expect(
          window->logText().contains("--options=hstream_ui.camera_controls.Lift_Shadow_Black_Point=1"),
          "Program launch should override stale canonical black-point lift with the effective checkbox value")) {
    activate(stop);
    return false;
  }
  if (!expect(
          window->logText().contains("--options=hstream_ui.camera_controls.Exposure_x100=30"),
          "Program launch should override stale canonical exposure with the effective UI value")) {
    activate(stop);
    return false;
  }
  use_10_bit_grading->setChecked(true);
  if (!expect(
          window->logText().contains("camera control Use_10_Bit_Grading=1 apply=save/restart"),
          "Changing high-bit mode during playback should be deferred to the next run")) {
    activate(stop);
    return false;
  }
  const int black_point_commands_before =
      window->logText().count("stdin:@set-property playcropper0 shadow-lift-black-point=");
  lift_shadow_black_point->setChecked(false);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Lift_Shadow_Black_Point=0 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property playcropper0 shadow-lift-black-point=") ==
              black_point_commands_before + 1,
          "Black-point checkbox should issue one coalesced live GPU property update") ||
      !expect(
          window->logText().contains("stdin:@set-property playcropper0 shadow-lift-black-point=0") &&
              window->logText().contains("camera control Lift_Shadow_Black_Point=0 apply=pending") &&
              window->logText().contains("camera control Lift_Shadow_Black_Point=0 apply=live"),
          "Black-point checkbox should report pending and live application")) {
    activate(stop);
    return false;
  }
  const int exposure_commands_before = window->logText().count("stdin:@set-property playcropper0 exposure=");
  for (int value = 31; value <= 60; ++value) {
    exposure->setValue(value);
  }
  for (int i = 0; i < 50 && !window->logText().contains("camera control Exposure_x100=60 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property playcropper0 exposure=") == exposure_commands_before + 1,
          "Rapid exposure changes should coalesce into one live GPU property update") ||
      !expect(
          window->logText().contains("stdin:@set-property playcropper0 exposure=0.60") &&
              window->logText().contains("camera control Exposure_x100=60 apply=pending") &&
              window->logText().contains("camera control Exposure_x100=60 apply=live") &&
              !window->logText().contains("camera control Exposure_x100=31 apply=pending"),
          "Only the final coalesced exposure should become pending and live")) {
    activate(stop);
    return false;
  }
  const int shadow_commands_before = window->logText().count("stdin:@set-property playcropper0 shadow-lift=");
  for (int value = 36; value <= 45; ++value) {
    bring_up_shadows->setValue(value);
  }
  for (int i = 0; i < 50 && !window->logText().contains("camera control Bring_Up_Shadows=45 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property playcropper0 shadow-lift=") == shadow_commands_before + 1,
          "Rapid shadow-lift changes should coalesce into one live GPU property update") ||
      !expect(
          window->logText().contains("stdin:@set-property playcropper0 shadow-lift=45") &&
              window->logText().contains("camera control Bring_Up_Shadows=45 apply=pending") &&
              window->logText().contains("camera control Bring_Up_Shadows=45 apply=live") &&
              !window->logText().contains("camera control Bring_Up_Shadows=36 apply=pending"),
          "Only the final coalesced shadow lift should become pending and live")) {
    activate(stop);
    return false;
  }
  use_10_bit_grading->setChecked(false);
  const int rotation_scoreboard_commands_before =
      window->logText().count("stdin:@set-property playcropper0 scoreboard-perspective-polygon=");
  const int rotation_commands_before =
      window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=");
  for (int value = 60; value <= 69; ++value) {
    rotate->setValue(value);
  }
  for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=69 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=") ==
              rotation_commands_before + 1,
          "Rapid stitch rotation changes should coalesce into one live pipeline command") ||
      !expect(
          window->logText().contains("camera control Stitch_Rotate_Degrees=69 apply=live") &&
              !window->logText().contains("camera control Stitch_Rotate_Degrees=60 apply=pending") &&
              window->logText().count("stdin:@set-property playcropper0 scoreboard-perspective-polygon=") ==
                  rotation_scoreboard_commands_before,
          "Only the final coalesced stitch rotation should publish frame-bound scoreboard geometry")) {
    activate(stop);
    return false;
  }
  const int dragged_rotation_commands_before =
      window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=");
  rotate->setSliderDown(true);
  rotate->setValue(70);
  QApplication::processEvents();
  QTest::qWait(150);
  rotate->setValue(71);
  QApplication::processEvents();
  QTest::qWait(150);
  if (!expect(
          window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=") ==
              dragged_rotation_commands_before,
          "A held stitch rotation drag should not publish expensive intermediate output epochs")) {
    rotate->setSliderDown(false);
    activate(stop);
    return false;
  }
  rotate->setValue(72);
  rotate->setSliderDown(false);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=72 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=") ==
              dragged_rotation_commands_before + 1,
          "Releasing stitch rotation should publish exactly one final output epoch") ||
      !expect(
          window->logText().contains("camera control Stitch_Rotate_Degrees=72 apply=live") &&
              !window->logText().contains("camera control Stitch_Rotate_Degrees=70 apply=pending") &&
              !window->logText().contains("camera control Stitch_Rotate_Degrees=71 apply=pending"),
          "Only the released stitch rotation value should become pending and live")) {
    activate(stop);
    return false;
  }
  fixed_edge_link->setValue(1);
  fixed_edge_left->setValue(300);
  for (int i = 0; i < 50 &&
       (!window->logText().contains("stdin:@set-property dsplaytracker0 fixed-edge-rotation-angle=30.0") ||
        !window->logText().contains("stdin:@set-property playcropper0 fixed-edge-rotation-angle=30.0"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(fixed_edge_right->value() == 300, "Linked fixed-edge control should update both sides") ||
      !expect(
          window->logText().contains("stdin:@set-property dsplaytracker0 fixed-edge-rotation-angle=30.0") &&
              window->logText().contains("stdin:@set-property playcropper0 fixed-edge-rotation-angle=30.0"),
          "Linked fixed-edge control should update tracker and cropper live")) {
    std::cerr << window->logText().toStdString() << '\n';
    activate(stop);
    return false;
  }
  fixed_edge_link->setValue(0);
  fixed_edge_right->setValue(650);
  for (int i = 0; i < 50 &&
       (!window->logText().contains("stdin:@set-property dsplaytracker0 fixed-edge-rotation-angle-right=65.0") ||
        !window->logText().contains("stdin:@set-property playcropper0 fixed-edge-rotation-angle-right=65.0"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("stdin:@set-property dsplaytracker0 fixed-edge-rotation-angle-right=65.0") &&
              window->logText().contains("stdin:@set-property playcropper0 fixed-edge-rotation-angle-right=65.0"),
          "Unlinked right fixed-edge control should update the right side of both runtime stages")) {
    activate(stop);
    return false;
  }

  pipeline_process->write("@test-reject-runtime-control\n");
  for (int i = 0; i < 50 && !window->logText().contains("test runtime control rejection armed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  fixed_edge_right->setValue(640);
  rotate->setValue(73);
  for (int i = 0; i < 100 && !window->logText().contains("camera control Stitch_Rotate_Degrees=73 apply=failed"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString rejected_batch_log = window->logText();
  const int rejected_tracker_index =
      rejected_batch_log.lastIndexOf("stdin:@set-property dsplaytracker0 fixed-edge-rotation-angle-right=64.0");
  const int rejected_cropper_index =
      rejected_batch_log.lastIndexOf("stdin:@set-property playcropper0 fixed-edge-rotation-angle-right=64.0");
  const int rejected_epoch_index = rejected_batch_log.lastIndexOf("stdin:@set-property hmstitcher0 stitched-output-epoch=");
  if (!expect(
          rejected_tracker_index >= 0 && rejected_cropper_index > rejected_tracker_index &&
              rejected_epoch_index > rejected_cropper_index &&
              rejected_batch_log.contains(
                  "runtime command failed: plugin rejected hmstitcher0.stitched-output-epoch=") &&
              !rejected_batch_log.contains("camera control Stitch_Rotate_Degrees=73 apply=live"),
          "A fallible mixed runtime batch must publish its frame epoch last so a rejected epoch never becomes live")) {
    std::cerr << rejected_batch_log.toStdString() << '\n';
    activate(stop);
    return false;
  }

  HStreamWindowTestAccess::clearLog(window);
  const int pipeline_commands_before_deferred_restart = window->logText().count("pipeline command ");
  activate(restart);
  for (int i = 0; i < 200 &&
       (window->pipelineStateText() != "PLAYING" ||
        window->logText().count("pipeline command ") == pipeline_commands_before_deferred_restart);
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->pipelineStateText() == "PLAYING" &&
              window->logText().count("pipeline command ") > pipeline_commands_before_deferred_restart &&
              window->logText().contains("stage restart continuing after pipeline cleanup"),
          "Restart Stage must resume after asynchronous live-rotation authorization rollback")) {
    activate(stop);
    return false;
  }

  max_speed_x->setValue(460);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Max_Speed_X_x10=460 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  auto newest_live_playtracker_config = [&]() {
    const fs::path dir = fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui";
    fs::path newest;
    std::uint64_t newest_generation = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
      const std::string name = entry.path().filename().string();
      constexpr std::string_view prefix = "play_tracker_runtime_";
      if (name.rfind(prefix, 0) != 0)
        continue;
      const std::uint64_t generation = std::stoull(name.substr(prefix.size()));
      if (newest.empty() || generation > newest_generation) {
        newest = entry.path();
        newest_generation = generation;
      }
    }
    return newest;
  };
  fs::path live_playtracker_config = newest_live_playtracker_config();
  YAML::Node live_playtracker =
      fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
  YAML::Node live_custom_tracker_value;
  YAML::Node live_follower_max_speed_x;
  YAML::Node live_follower_max_speed_y;
  const bool live_preserved_custom_tracker_config =
      lookup_yaml_path(
          live_playtracker,
          {"play-tracker", "live-boxes", "1", "sticky-translation-gaussian-mult"},
          &live_custom_tracker_value) &&
      live_custom_tracker_value.IsScalar() && live_custom_tracker_value.as<double>() == 9.5;
  const bool live_saved_follower_speed =
      lookup_yaml_path(
          live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-x"}, &live_follower_max_speed_x) &&
      live_follower_max_speed_x.IsScalar() && live_follower_max_speed_x.as<double>() == 46.0;
  const bool live_preserved_follower_y_speed =
      lookup_yaml_path(
          live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-y"}, &live_follower_max_speed_y) &&
      live_follower_max_speed_y.IsScalar() && live_follower_max_speed_y.as<double>() == 77.0;
  const bool native_runtime_tuning_ok =
      fs::exists(live_playtracker_config) && DsPlayTrackerLoadRuntimeTuning(live_playtracker_config.string()).ok();
  if (!expect(
          window->logText().contains("stdin:@set-property dsplaytracker0 runtime-tuning-config-file="),
          "Live speed slider should send a state-preserving playtracker update to the running pipeline") ||
      !expect(
          window->logText().contains("camera control Max_Speed_X_x10=460 apply=pending") &&
              window->logText().contains("camera control Max_Speed_X_x10=460 apply=live"),
          "Live speed slider should only report success after the pipeline acknowledges it") ||
      !expect(
          live_preserved_custom_tracker_config,
          "Live playtracker update should preserve the custom base tracker config") ||
      !expect(live_saved_follower_speed, "Live playtracker update should write the new follower speed") ||
      !expect(live_preserved_follower_y_speed, "Live playtracker update should preserve untouched motion limits") ||
      !expect(native_runtime_tuning_ok, "The native playtracker loader should accept the exact UI runtime sidecar")) {
    std::cerr << live_playtracker << '\n';
    activate(stop);
    return false;
  }
  zoom_in_aggressiveness->setValue(75);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Zoom_In_Aggressiveness=75 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const fs::path live_zoom_config_path = newest_live_playtracker_config();
  const YAML::Node live_zoom_config =
      fs::exists(live_zoom_config_path) ? YAML::LoadFile(live_zoom_config_path.string()) : YAML::Node();
  YAML::Node live_zoom_value;
  const bool live_zoom_written = lookup_yaml_path(
      live_zoom_config, {"play-tracker", "hstream-runtime-tuning", "zoom-in-aggressiveness"}, &live_zoom_value);
  if (!expect(
          window->logText().contains("camera control Zoom_In_Aggressiveness=75 apply=pending") &&
              window->logText().contains("camera control Zoom_In_Aggressiveness=75 apply=live") && live_zoom_written &&
              live_zoom_value.IsScalar() && live_zoom_value.as<int>() == 75,
          "Zoom-in aggressiveness should publish an acknowledged sparse live follower tuning update")) {
    activate(stop);
    return false;
  }
  max_speed_y->setValue(480);
  for (int i = 0; i < 50; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
    live_playtracker_config = newest_live_playtracker_config();
    live_playtracker =
        fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
    YAML::Node sequential_x;
    YAML::Node sequential_y;
    if (lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-x"}, &sequential_x) &&
        lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-y"}, &sequential_y) &&
        sequential_x.IsScalar() && sequential_y.IsScalar() && sequential_x.as<double>() == 46.0 &&
        sequential_y.as<double>() == 48.0) {
      break;
    }
  }
  live_playtracker_config = newest_live_playtracker_config();
  live_playtracker =
      fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
  YAML::Node sequential_x;
  YAML::Node sequential_y;
  const bool live_kept_prior_x_speed =
      lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-x"}, &sequential_x) &&
      sequential_x.IsScalar() && sequential_x.as<double>() == 46.0;
  const bool live_saved_y_speed =
      lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-y"}, &sequential_y) &&
      sequential_y.IsScalar() && sequential_y.as<double>() == 48.0;
  if (!expect(live_kept_prior_x_speed, "Second live playtracker update should preserve prior X override") ||
      !expect(live_saved_y_speed, "Second live playtracker update should write Y override")) {
    std::cerr << live_playtracker << '\n';
    return false;
  }
  apply_to_fast->setValue(1);
  max_speed_x->setValue(510);
  max_accel_x->setValue(35);
  for (int i = 0; i < 50; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
    live_playtracker_config = newest_live_playtracker_config();
    live_playtracker =
        fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
    YAML::Node rapid_speed_x;
    YAML::Node rapid_accel_x;
    if (lookup_yaml_path(live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-x"}, &rapid_speed_x) &&
        lookup_yaml_path(live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-accel-x"}, &rapid_accel_x) &&
        rapid_speed_x.as<double>() == 51.0 && rapid_accel_x.as<double>() == 3.5) {
      break;
    }
  }
  YAML::Node rapid_speed_x;
  YAML::Node rapid_accel_x;
  const bool coalesced_rapid_controls =
      lookup_yaml_path(live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-x"}, &rapid_speed_x) &&
      lookup_yaml_path(live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-accel-x"}, &rapid_accel_x) &&
      rapid_speed_x.as<double>() == 51.0 && rapid_accel_x.as<double>() == 3.5;
  for (int i = 0; i < 50 &&
       (!window->logText().contains("camera control Max_Speed_X_x10=510 apply=live") ||
        !window->logText().contains("camera control Max_Accel_X_x10=35 apply=live"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(coalesced_rapid_controls, "Rapid distinct live controls should be coalesced without dropping either") ||
      !expect(
          window->logText().contains("camera control Max_Speed_X_x10=510 apply=live") &&
              window->logText().contains("camera control Max_Accel_X_x10=35 apply=live"),
          "Every control coalesced into one snapshot should be acknowledged")) {
    std::cerr << live_playtracker << '\n';
    activate(stop);
    return false;
  }

  activate(reset);
  for (int i = 0; i < 50; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
    live_playtracker_config = newest_live_playtracker_config();
    live_playtracker =
        fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
    YAML::Node reset_runtime_speed_x;
    YAML::Node reset_runtime_speed_y;
    YAML::Node reset_runtime_accel_x;
    if (lookup_yaml_path(
            live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-x"}, &reset_runtime_speed_x) &&
        lookup_yaml_path(
            live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-y"}, &reset_runtime_speed_y) &&
        lookup_yaml_path(
            live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-accel-x"}, &reset_runtime_accel_x) &&
        reset_runtime_speed_x.as<double>() == 0.0 && reset_runtime_speed_y.as<double>() == 0.0 &&
        reset_runtime_accel_x.as<double>() == 0.0) {
      break;
    }
  }
  YAML::Node reset_runtime_speed_x;
  YAML::Node reset_runtime_speed_y;
  YAML::Node reset_runtime_accel_x;
  const bool reset_coalesced_all_dirty_controls =
      lookup_yaml_path(
          live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-x"}, &reset_runtime_speed_x) &&
      lookup_yaml_path(
          live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-speed-y"}, &reset_runtime_speed_y) &&
      lookup_yaml_path(
          live_playtracker, {"play-tracker", "hstream-runtime-tuning", "max-accel-x"}, &reset_runtime_accel_x) &&
      reset_runtime_speed_x.as<double>() == 0.0 && reset_runtime_speed_y.as<double>() == 0.0 &&
      reset_runtime_accel_x.as<double>() == 0.0 &&
      live_playtracker["play-tracker"]["hstream-apply-to-fast-box"].as<bool>() &&
      live_playtracker["play-tracker"]["hstream-apply-to-follower-box"].as<bool>();
  if (!expect(
          reset_coalesced_all_dirty_controls,
          "Reset Controls during playback should restore every changed control on both previously tunable boxes")) {
    std::cerr << live_playtracker << '\n';
    activate(stop);
    return false;
  }

  max_speed_x->setValue(460);
  max_speed_y->setValue(480);
  max_speed_y->setValue(0);
  for (int i = 0; i < 50; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
    live_playtracker_config = newest_live_playtracker_config();
    live_playtracker =
        fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
    YAML::Node reset_x;
    YAML::Node reset_y;
    if (lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-x"}, &reset_x) &&
        lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-y"}, &reset_y) &&
        reset_x.IsScalar() && reset_y.IsScalar() && reset_x.as<double>() == 46.0 && reset_y.as<double>() == 77.0) {
      break;
    }
  }
  live_playtracker_config = newest_live_playtracker_config();
  live_playtracker =
      fs::exists(live_playtracker_config) ? YAML::LoadFile(live_playtracker_config.string()) : YAML::Node();
  YAML::Node reset_x;
  YAML::Node reset_y;
  const bool live_kept_x_after_y_reset =
      lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-x"}, &reset_x) &&
      reset_x.IsScalar() && reset_x.as<double>() == 46.0;
  const bool live_restored_base_y_on_reset =
      lookup_yaml_path(live_playtracker, {"play-tracker", "live-boxes", "1", "max-speed-y"}, &reset_y) &&
      reset_y.IsScalar() && reset_y.as<double>() == 77.0;
  if (!expect(live_kept_x_after_y_reset, "Resetting Y should preserve prior live X override") ||
      !expect(live_restored_base_y_on_reset, "Resetting Y should restore custom base Y speed")) {
    std::cerr << live_playtracker << '\n';
    activate(stop);
    return false;
  }

  activate(stop);
  qputenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL", "1");
  qputenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS", "40");
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->pipelineStateText() == "PLAYING", "Fake runner should restart for stalled-control test")) {
    qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
    qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
    return false;
  }
  max_speed_x->setValue(470);
  QTest::qWait(220);
  for (int value = 471; value <= 490; ++value) {
    max_speed_x->setValue(value);
  }
  QTest::qWait(220);
  const fs::path runtime_dir = fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui";
  auto runtime_snapshot_count = [&]() {
    return static_cast<int>(std::count_if(
        fs::directory_iterator(runtime_dir), fs::directory_iterator(), [](const fs::directory_entry& entry) {
          return entry.path().filename().string().rfind("play_tracker_runtime_", 0) == 0;
        }));
  };
  const bool stalled_controls_bounded =
      expect(
          runtime_snapshot_count() <= 2,
          "A non-acknowledging backend should retain at most the last acknowledged and one in-flight snapshot") &&
      expect(
          window->logText().contains(
              "camera control Max_Speed_X_x10=470 apply=failed "
              "reason=acknowledgement-timeout") &&
              window->logText().contains(
                  "camera control Max_Speed_X_x10=490 apply=failed "
                  "reason=acknowledgement-timeout"),
          "A stalled live-control backend should time out both the in-flight and coalesced latest values");
  const auto timeout_hugin_generation = write_live_hugin_generation_fixture(config.parent_path());
  if (!timeout_hugin_generation.ok()) {
    qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
    qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
    return false;
  }
  {
    auto timeout_fixture_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
    if (!timeout_fixture_lock.ok()) {
      qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
      qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
      return false;
    }
    YAML::Node timeout_fixture = YAML::LoadFile(config.string());
    timeout_fixture["rink"]["stitched_output_generation"] =
        "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(timeout_hugin_generation->size()) + "\n" +
        *timeout_hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
    const auto published = hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(timeout_fixture) + "\n");
    if (!published.ok()) {
      qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
      qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
      return false;
    }
  }
  const int stalled_rotation_value = rotate->value() == 67 ? 68 : 67;
  const int stalled_epoch_commands_before =
      window->logText().count("stdin:@set-property hmstitcher0 stitched-output-epoch=");
  rotate->setValue(stalled_rotation_value);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 200 && HStreamWindowTestAccess::liveRotationAuthorizationPending(window); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const YAML::Node after_stalled_rotation = YAML::LoadFile(config.string());
  const QString stalled_rotation_log = window->logText();
  const bool stalled_rotation_published_one_epoch =
      stalled_rotation_log.count("stdin:@set-property hmstitcher0 stitched-output-epoch=") >
      stalled_epoch_commands_before;
  const bool stalled_rotation_reconciled =
      expect(
          window->pipelineStateText() == "STOPPED" &&
              window->logText().contains(
                  QString("camera control Stitch_Rotate_Degrees=%1 apply=failed reason=acknowledgement-timeout")
                      .arg(stalled_rotation_value)),
          "An ambiguous live-rotation timeout must stop the pipeline") &&
      expect(
          !HStreamWindowTestAccess::liveRotationAuthorizationPending(window) &&
              !after_stalled_rotation["rink"]["stitched_output_pending_generation"].IsDefined() &&
              !after_stalled_rotation["rink"]["stitched_output_pending_authorization_id"].IsDefined() &&
              stalled_rotation_published_one_epoch,
          "Pipeline-stop reconciliation must retire a timed-out live-rotation authorization without exposing stale "
          "frame epoch state");
  bool timeout_fixture_cleaned = false;
  {
    auto timeout_fixture_cleanup_lock = hm::stitching::GameConfigTransactionLock::Acquire(config.parent_path());
    if (timeout_fixture_cleanup_lock.ok()) {
      YAML::Node timeout_fixture_cleanup = YAML::LoadFile(config.string());
      timeout_fixture_cleanup["rink"].remove("stitched_output_generation");
      timeout_fixture_cleanup["rink"].remove("stitched_output_persisted_rotation_degrees");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_generation");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_authorization_id");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_owner_process");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_previous_generation");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_previous_authorization_id");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_previous_owner_process");
      timeout_fixture_cleanup["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
      timeout_fixture_cleaned =
          hm::stitching::publish_game_config(config.parent_path(), YAML::Dump(timeout_fixture_cleanup) + "\n").ok();
    }
  }
  qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
  qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
  if (!stalled_controls_bounded || !stalled_rotation_reconciled || !timeout_fixture_cleaned) {
    std::cerr << window->logText().toStdString() << '\n';
    activate(stop);
    return false;
  }
  activate(stop);

  activate(save);
  YAML::Node same_prefix = YAML::LoadFile(config.string());
  YAML::Node same_prefix_tracker_path;
  const bool has_same_prefix_tracker_path =
      lookup_yaml_path(same_prefix, {"pipeline", "ds-playtracker", "config-file"}, &same_prefix_tracker_path);
  YAML::Node same_prefix_tracker =
      has_same_prefix_tracker_path && fs::exists(same_prefix_tracker_path.as<std::string>())
      ? YAML::LoadFile(same_prefix_tracker_path.as<std::string>())
      : YAML::Node();
  YAML::Node preserved_custom_tracker_value;
  const bool preserved_custom_tracker_config =
      lookup_yaml_path(
          same_prefix_tracker,
          {"play-tracker", "live-boxes", "1", "sticky-translation-gaussian-mult"},
          &preserved_custom_tracker_value) &&
      preserved_custom_tracker_value.IsScalar() && preserved_custom_tracker_value.as<double>() == 9.5;
  YAML::Node saved_playtracker_base;
  const bool remembered_custom_tracker_base =
      lookup_yaml_path(same_prefix, {"hstream_ui", "playtracker_config_base"}, &saved_playtracker_base) &&
      saved_playtracker_base.IsScalar() &&
      saved_playtracker_base.as<std::string>() == custom_playtracker_config.string();
  if (!expect(preserved_custom_tracker_config, "Generated playtracker config should preserve custom base config") ||
      !expect(remembered_custom_tracker_base, "Generated playtracker config should remember custom base path")) {
    std::cerr << same_prefix << '\n';
    std::cerr << same_prefix_tracker << '\n';
    return false;
  }

  activate(reset);
  activate(save);
  YAML::Node cleaned = YAML::LoadFile(config.string());
  YAML::Node cleaned_controls;
  const bool has_cleaned_controls = lookup_yaml_path(cleaned, {"hstream_ui", "camera_controls"}, &cleaned_controls);
  YAML::Node preserved_gamma;
  const bool has_preserved_gamma =
      lookup_yaml_path(cleaned, {"stitching", "right", "color", "gamma"}, &preserved_gamma);
  const bool preserved_manual_gamma =
      has_preserved_gamma && preserved_gamma.IsScalar() && preserved_gamma.as<double>() == 1.75;
  YAML::Node preserved_left_gamma_node;
  const bool preserved_manual_left_gamma =
      lookup_yaml_path(cleaned, {"stitching", "left", "color", "gamma"}, &preserved_left_gamma_node) &&
      preserved_left_gamma_node.IsScalar() && preserved_left_gamma_node.as<double>() == 1.75;
  YAML::Node restored_playtracker_config_path;
  const bool restored_custom_playtracker_config =
      lookup_yaml_path(cleaned, {"pipeline", "ds-playtracker", "config-file"}, &restored_playtracker_config_path) &&
      restored_playtracker_config_path.IsScalar() &&
      restored_playtracker_config_path.as<std::string>() == custom_playtracker_config.string();
  if (!preserved_manual_gamma || !preserved_manual_left_gamma) {
    std::cerr << cleaned << '\n';
  }
  const bool cleaned_defaults = expect(
                                    has_cleaned_controls && cleaned_controls.IsMap() && cleaned_controls.size() == 0,
                                    "Saving defaults should clear saved camera controls") &&
      expect(!lookup_yaml_path(cleaned, {"stitching", "post_stitch_rotate_degrees"}, nullptr),
             "Saving defaults should clear UI-generated stitch runtime override") &&
      expect(!lookup_yaml_path(cleaned, {"rink", "camera", "fixed_edge_rotation_angle"}, nullptr),
             "Saving defaults should clear UI-generated fixed-edge rotation override") &&
      expect(restored_custom_playtracker_config, "Saving defaults should restore custom playtracker config override") &&
      expect(preserved_manual_gamma, "Saving defaults should preserve non-UI-authored runtime config") &&
      expect(preserved_manual_left_gamma, "Saving defaults should preserve same-prefix manual color config");
  if (!cleaned_defaults) {
    return false;
  }

  const fs::path one_box_playtracker_config =
      fs::path(window->gameDirectoryText().toStdString()) / "one_box_playtracker.yaml";
  YAML::Node one_box_tracker(YAML::NodeType::Map);
  YAML::Node one_box_sequence(YAML::NodeType::Sequence);
  YAML::Node one_box(YAML::NodeType::Map);
  one_box["name"] = "operator_only";
  one_box["sticky-translation-gaussian-mult"] = 8.5;
  one_box_sequence.push_back(one_box);
  one_box_tracker["play-tracker"]["live-boxes"] = one_box_sequence;
  std::ofstream(one_box_playtracker_config) << YAML::Dump(one_box_tracker) << '\n';
  YAML::Node one_box_game_config = YAML::Clone(cleaned);
  one_box_game_config["pipeline"]["ds-playtracker"]["config-file"] = one_box_playtracker_config.string();
  one_box_game_config["hstream_ui"].remove("playtracker_config_base");
  std::ofstream(config) << YAML::Dump(one_box_game_config) << '\n';
  activate(create);
  max_speed_x->setValue(333);
  activate(save);
  const YAML::Node saved_one_box_game = YAML::LoadFile(config.string());
  const fs::path saved_one_box_path = saved_one_box_game["pipeline"]["ds-playtracker"]["config-file"].as<std::string>();
  const YAML::Node saved_one_box = YAML::LoadFile(saved_one_box_path.string());
  const YAML::Node saved_one_box_sequence = saved_one_box["play-tracker"]["live-boxes"];
  if (!expect(
          saved_one_box_sequence.IsSequence() && saved_one_box_sequence.size() == 1 &&
              saved_one_box_sequence[0]["name"].as<std::string>() == "operator_only" &&
              saved_one_box_sequence[0]["sticky-translation-gaussian-mult"].as<double>() == 8.5 &&
              saved_one_box_sequence[0]["max-speed-x"].as<double>() == 33.3,
          "Saving a one-box tracker preset must tune its shared role without silently adding a second box")) {
    std::cerr << saved_one_box << '\n';
    return false;
  }

  const fs::path aged_active_sidecar = runtime_dir / "play_tracker_config_aged-active.yaml";
  fs::copy_file(custom_playtracker_config, aged_active_sidecar, fs::copy_options::overwrite_existing);
  fs::last_write_time(aged_active_sidecar, fs::file_time_type::clock::now() - std::chrono::hours(25));
  cleaned["pipeline"]["ds-playtracker"]["config-file"] = aged_active_sidecar.string();
  cleaned["hstream_ui"]["generated_runtime_keys"] = YAML::Node(YAML::NodeType::Sequence);
  cleaned["hstream_ui"]["generated_runtime_values"] = YAML::Node(YAML::NodeType::Map);
  cleaned["hstream_ui"].remove("playtracker_config_base");
  {
    std::ofstream out(config);
    out << cleaned << "\n";
  }
  activate(create);
  rotate->setValue(72);
  stitch_frame_time->setTime(QTime(0, 0, 0));
  activate(save);
  const YAML::Node after_aged_active_save = YAML::LoadFile(config.string());
  YAML::Node active_sidecar_node;
  const bool retained_aged_active_sidecar =
      lookup_yaml_path(after_aged_active_save, {"pipeline", "ds-playtracker", "config-file"}, &active_sidecar_node) &&
      active_sidecar_node.IsScalar() && active_sidecar_node.as<std::string>() == aged_active_sidecar.string() &&
      fs::exists(aged_active_sidecar);
  return expect(
             retained_aged_active_sidecar,
             "Preset GC should never delete the aged playtracker sidecar referenced by the committed config") &&
      expect(!lookup_yaml_path(after_aged_active_save, {"stitching", "stitch_frame_time"}, nullptr),
             "Saving the default stitch-frame time should omit stitching.stitch_frame_time");
}

bool test_nonzero_user_stitch_frame_default(const QString& source_game_directory) {
  const QByteArray original_home = qgetenv("HOME");
  const QByteArray original_config_root = qgetenv("HM_CONFIG_ROOT");
  QTemporaryDir user_home;
  if (!user_home.isValid())
    return false;
  QTemporaryDir baseline_root;
  if (!baseline_root.isValid())
    return false;
  const auto source_baseline = find_test_baseline_yaml();
  if (!source_baseline.has_value())
    return expect(false, "Could not locate bundled baseline.yaml for user-default fixture");
  YAML::Node baseline = YAML::LoadFile(source_baseline->string());
  baseline["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = YAML::Node(YAML::NodeType::Null);
  baseline["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 1234;
  {
    std::ofstream out(QDir(baseline_root.path()).filePath("baseline.yaml").toStdString());
    out << YAML::Dump(baseline) << '\n';
  }
  const QString user_config_directory = QDir(user_home.path()).filePath(".hstream");
  if (!QDir().mkpath(user_config_directory))
    return false;
  YAML::Node user_config(YAML::NodeType::Map);
  user_config["stitching"]["stitch_frame_time"] = "00:00:08";
  user_config["stitching"]["max_output_width"] = YAML::Node(YAML::NodeType::Null);
  user_config["stitching"]["post_stitch_rotate_degrees"] = 20;
  user_config["stitching"]["control_point_matcher"] = "dedode-lightglue";
  user_config["stitching"]["mapping_backend"] = "RANSAC";
  user_config["stitching"]["run_autooptimizer"] = true;
  user_config["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Node(YAML::NodeType::Null);
  {
    std::ofstream out(QDir(user_config_directory).filePath("hstream.yaml").toStdString());
    out << YAML::Dump(user_config) << '\n';
  }

  const fs::path game_root(qgetenv("HM_GAME_DIR").constData());
  const fs::path copied_game = game_root / "ui-user-stitch-default";
  std::error_code copy_error;
  fs::remove_all(copied_game, copy_error);
  copy_error.clear();
  fs::create_directories(copied_game, copy_error);
  const fs::path source_game(source_game_directory.toStdString());
  for (fs::directory_iterator it(source_game, copy_error), end; !copy_error && it != end; it.increment(copy_error)) {
    fs::copy(
        it->path(),
        copied_game / it->path().filename(),
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        copy_error);
  }
  if (copy_error)
    return expect(false, "Could not prepare the nonzero user stitch-frame fixture: " + copy_error.message());
  const fs::path copied_config = copied_game / "config.yaml";
  YAML::Node copied_config_node =
      fs::is_regular_file(copied_config) ? YAML::LoadFile(copied_config.string()) : YAML::Node(YAML::NodeType::Map);
  if (copied_config_node["stitching"] && copied_config_node["stitching"].IsMap())
    copied_config_node["stitching"].remove("stitch_frame_time");
  copied_config_node["stitching"]["post_stitch_rotate_degrees"] = YAML::Node(YAML::NodeType::Null);
  copied_config_node["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  copied_config_node["stitching"]["mapping_backend"] = "nona";
  copied_config_node["stitching"]["run_autooptimizer"] = true;
  copied_config_node["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  copied_config_node["hstream_ui"]["stitching_calibration"].remove("stale_from");
  copied_config_node["hstream_ui"]["stitching_calibration"].remove("artifacts_invalidated");
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] =
      "superpoint-lightglue";
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = "nona";
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["run_autooptimizer"] = true;
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_control_point_matcher"] =
      "superpoint-lightglue";
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"] =
      "opencv-affine-ransac";
  copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_run_autooptimizer"] = true;
  if (copied_config_node["rink"] && copied_config_node["rink"].IsMap() && copied_config_node["rink"]["camera"] &&
      copied_config_node["rink"]["camera"].IsMap()) {
    copied_config_node["rink"]["camera"].remove("fixed_edge_rotation_angle");
  }
  std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
  std::ofstream(copied_game / "seam_file.png") << "preserved optimizer-normalization artifact\n";

  qputenv("HOME", user_home.path().toLocal8Bit());
  qputenv("HM_CONFIG_ROOT", baseline_root.path().toLocal8Bit());
  bool ok = true;
  {
    HStreamWindow user_default_window;
    user_default_window.show();
    auto* game_id = require_child<QLineEdit>(&user_default_window, "gameIdEdit");
    auto* create = require_child<QPushButton>(&user_default_window, "createGameButton");
    auto* save = require_child<QPushButton>(&user_default_window, "savePresetButton");
    auto* start = require_child<QPushButton>(&user_default_window, "startPipelineButton");
    auto* stop = require_child<QPushButton>(&user_default_window, "stopPipelineButton");
    auto* mode = require_child<QComboBox>(&user_default_window, "runModeCombo");
    auto* stitch_frame_time = require_child<QTimeEdit>(&user_default_window, "stitchFrameTimeEdit");
    auto* stitch_max_output_width = require_child<QSpinBox>(&user_default_window, "stitchMaxOutputWidthSpin");
    auto* control_point_matcher = require_child<QComboBox>(&user_default_window, "controlPointMatcherCombo");
    auto* mapping_backend = require_child<QComboBox>(&user_default_window, "mappingBackendCombo");
    auto* run_autooptimizer = require_child<QCheckBox>(&user_default_window, "runAutooptimizerCheck");
    auto* projection = require_child<QComboBox>(&user_default_window, "stitchProjectionCombo");
    auto* stitch_rotation = require_child<QSlider>(&user_default_window, "cameraSlider_Stitch_Rotate_Degrees");
    auto* fixed_edge_left =
        require_child<QSlider>(&user_default_window, "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10");
    auto* fixed_edge_right =
        require_child<QSlider>(&user_default_window, "cameraSlider_Right_Fixed_Edge_Rotation_Angle_x10");
    ok = game_id && create && save && start && stop && mode && stitch_frame_time && stitch_max_output_width &&
        control_point_matcher && mapping_backend && projection && run_autooptimizer && stitch_rotation &&
        fixed_edge_left && fixed_edge_right;
    if (ok) {
      game_id->setText("ui-user-stitch-default");
      activate(create);
      ok &= expect(
          stitch_frame_time->time() == QTime(0, 0, 8) && stitch_rotation->value() == 90 &&
              stitch_max_output_width->value() == 0 &&
              control_point_matcher->currentData().toString() == "superpoint-lightglue" &&
              mapping_backend->currentData().toString() == "opencv-affine-ransac" && !run_autooptimizer->isChecked() &&
              !run_autooptimizer->isEnabled() && projection->currentData().toString() == "rectilinear" &&
              fixed_edge_left->value() == 0 && fixed_edge_right->value() == 0 && save->isEnabled(),
          "User-level defaults must initialize the UI, reject unimplemented matchers, accept mapping aliases, and "
          "generated private backend choices and lower-layer max-width aliases must not mask them");
      activate(save);
      const YAML::Node normalized_inactive_optimizer = YAML::LoadFile(copied_config.string());
      ok &= expect(
          normalized_inactive_optimizer["stitching"]["mapping_backend"].as<std::string>() == "opencv-affine-ransac" &&
              !normalized_inactive_optimizer["stitching"]["run_autooptimizer"].as<bool>() &&
              normalized_inactive_optimizer["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() ==
                  "complete" &&
              fs::is_regular_file(copied_game / "seam_file.png") && !save->isEnabled(),
          "Normalizing an inactive OpenCV optimizer flag must preserve completed calibration artifacts");
      stitch_frame_time->setTime(QTime(0, 0, 0));
      QApplication::processEvents();
      ok &= expect(save->isEnabled(), "Zero must remain an explicit edit against a nonzero user-level default");
      activate(save);
      const YAML::Node saved_zero = YAML::LoadFile(copied_config.string());
      ok &= expect(
          saved_zero["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00" &&
              saved_zero["stitching"]["post_stitch_rotate_degrees"].IsNull() &&
              saved_zero["stitching"]["mapping_backend"].as<std::string>() == "opencv-affine-ransac" &&
              saved_zero["stitching"]["projection"].as<std::string>() == "rectilinear" &&
              !lookup_yaml_path(saved_zero, {"stitching", "max_output_width"}, nullptr) &&
              !saved_zero["stitching"]["run_autooptimizer"].as<bool>() &&
              !lookup_yaml_path(saved_zero, {"hstream_ui", "generated_stitching_backend_choices"}, nullptr) &&
              !lookup_yaml_path(saved_zero, {"rink", "camera", "fixed_edge_rotation_angle"}, nullptr) &&
              !save->isEnabled(),
          "Saving against user defaults must preserve game null semantics and make UI-owned backends explicit");

      copied_config_node = YAML::LoadFile(copied_config.string());
      copied_config_node["stitching"]["mapping_backend"] = "opencv-magsac";
      copied_config_node["stitching"]["run_autooptimizer"] = false;
      copied_config_node["stitching"]["projection"] = "rectilinear";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] =
          "superpoint-lightglue";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = "opencv-magsac";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["projection"] = "rectilinear";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["run_autooptimizer"] = false;
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_control_point_matcher"] =
          "superpoint-lightglue";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"] =
          "opencv-affine-ransac";
      copied_config_node["hstream_ui"]["generated_stitching_backend_choices"]["previous_run_autooptimizer"] = true;
      std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
      activate(create);
      ok &= expect(
          mapping_backend->currentData().toString() == "opencv-affine-ransac" && !run_autooptimizer->isChecked() &&
              !run_autooptimizer->isEnabled() && projection->currentData().toString() == "rectilinear" &&
              save->isEnabled(),
          "UI load must restore previous explicit stitching algorithm choices displaced by CLI materialization");

      copied_config_node = YAML::LoadFile(copied_config.string());
      copied_config_node["stitching"]["mapping_backend"] = "nona";
      copied_config_node["stitching"]["run_autooptimizer"] = false;
      copied_config_node["hstream_ui"].remove("generated_stitching_backend_choices");
      std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
      activate(create);
      ok &= expect(
          mapping_backend->currentData().toString() == "opencv-magsac" && !run_autooptimizer->isChecked() &&
              projection->currentData().toString() == "rectilinear" && save->isEnabled(),
          "UI load must normalize an invalid NONA-without-autooptimizer pair and offer to save the correction");
      activate(save);
      const YAML::Node normalized_backend = YAML::LoadFile(copied_config.string());
      ok &= expect(
          normalized_backend["stitching"]["mapping_backend"].as<std::string>() == "opencv-magsac" &&
              !normalized_backend["stitching"]["run_autooptimizer"].as<bool>() &&
              normalized_backend["stitching"]["projection"].as<std::string>() == "rectilinear" && !save->isEnabled(),
          "Saving a normalized backend pair must persist MAGSAC with the autooptimizer disabled");

      copied_config_node = YAML::LoadFile(copied_config.string());
      copied_config_node["stitching"]["mapping_backend"] = "nona";
      copied_config_node["stitching"]["run_autooptimizer"] = true;
      copied_config_node["stitching"]["projection_framing"]["horizontal_fov"] = 179;
      std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
      activate(create);
      ok &= expect(
          mapping_backend->currentData().toString() == "nona" && run_autooptimizer->isChecked() &&
              run_autooptimizer->isEnabled() && projection->currentData().toString() == "rectilinear",
          "Reloading a valid NONA preset after OpenCV must enable its required autooptimizer control");

      copied_config_node = YAML::LoadFile(copied_config.string());
      copied_config_node["stitching"]["mapping_backend"] = "opencv-magsac";
      copied_config_node["stitching"]["run_autooptimizer"] = false;
      std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
      activate(create);
      ok &= expect(
          mapping_backend->currentData().toString() == "opencv-magsac" && !run_autooptimizer->isChecked() &&
              !run_autooptimizer->isEnabled() && projection->currentData().toString() == "rectilinear",
          "Reloading OpenCV after NONA must disable the inactive autooptimizer control");

      mode->setCurrentIndex(mode->findData("program"));
      const int zero_argument_count = user_default_window.logText().count("--stitch-frame-time=00:00:00");
      qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
      activate(start);
      for (int i = 0;
           i < 50 && user_default_window.logText().count("--stitch-frame-time=00:00:00") == zero_argument_count;
           ++i) {
        QApplication::processEvents();
        QTest::qWait(10);
      }
      const YAML::Node played_zero = YAML::LoadFile(copied_config.string());
      ok &= expect(
          user_default_window.logText().count("--stitch-frame-time=00:00:00") == zero_argument_count + 1 &&
              played_zero["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00" &&
              !lookup_yaml_path(played_zero, {"stitching", "max_output_width"}, nullptr),
          "Play must retain explicit non-default values without materializing an inherited Auto max width");
      activate(stop);
      qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
    }
  }
  user_config["stitching"].remove("max_output_width");
  user_config["pipeline"]["hmstitcher"]["private-properties"]["stitch_max_output_width"] = 4321;
  {
    std::ofstream out(QDir(user_config_directory).filePath("hstream.yaml").toStdString());
    out << YAML::Dump(user_config) << '\n';
  }
  {
    HStreamWindow user_native_default_window;
    auto* game_id = require_child<QLineEdit>(&user_native_default_window, "gameIdEdit");
    auto* create = require_child<QPushButton>(&user_native_default_window, "createGameButton");
    auto* save = require_child<QPushButton>(&user_native_default_window, "savePresetButton");
    auto* stitch_max_output_width = require_child<QSpinBox>(&user_native_default_window, "stitchMaxOutputWidthSpin");
    if (game_id && create) {
      game_id->setText("ui-user-stitch-default");
      activate(create);
    }
    ok &= expect(
        game_id && create && save && stitch_max_output_width && stitch_max_output_width->value() == 4321,
        "A later user-level private max-width default must reach a game that previously inherited Auto");
    if (save && stitch_max_output_width) {
      stitch_max_output_width->setValue(0);
      QApplication::processEvents();
      activate(save);
      const YAML::Node saved_auto_override = YAML::LoadFile(copied_config.string());
      ok &= expect(
          saved_auto_override["stitching"]["max_output_width"].IsNull(),
          "Selecting Auto against a nonzero inherited max width must persist an explicit game override");
    }
  }
  user_config["pipeline"]["hmstitcher"]["private-properties"].remove("stitch_max_output_width");
  {
    std::ofstream out(QDir(user_config_directory).filePath("hstream.yaml").toStdString());
    out << YAML::Dump(user_config) << '\n';
  }
  copied_config_node = YAML::LoadFile(copied_config.string());
  copied_config_node["stitching"].remove("max_output_width");
  if (copied_config_node["pipeline"] && copied_config_node["pipeline"]["hmstitcher"]) {
    copied_config_node["pipeline"]["hmstitcher"]["properties"].remove("max-output-width");
    copied_config_node["pipeline"]["hmstitcher"]["private-properties"].remove("stitch_max_output_width");
  }
  std::ofstream(copied_config) << YAML::Dump(copied_config_node) << '\n';
  {
    HStreamWindow baseline_alias_default_window;
    auto* game_id = require_child<QLineEdit>(&baseline_alias_default_window, "gameIdEdit");
    auto* create = require_child<QPushButton>(&baseline_alias_default_window, "createGameButton");
    auto* save = require_child<QPushButton>(&baseline_alias_default_window, "savePresetButton");
    auto* stitch_max_output_width = require_child<QSpinBox>(&baseline_alias_default_window, "stitchMaxOutputWidthSpin");
    if (game_id && create) {
      game_id->setText("ui-user-stitch-default");
      activate(create);
    }
    ok &= expect(
        game_id && create && save && stitch_max_output_width && stitch_max_output_width->value() == 1234 &&
            !save->isEnabled(),
        "A baseline native null must not mask a later numeric private max-width alias used by the pipeline");
  }
  baseline["stitching"].remove("mapping_backend");
  baseline["stitching"].remove("run_autooptimizer");
  {
    std::ofstream out(QDir(baseline_root.path()).filePath("baseline.yaml").toStdString());
    out << YAML::Dump(baseline) << '\n';
  }
  {
    std::ofstream out(QDir(user_config_directory).filePath("hstream.yaml").toStdString());
    out << "{}\n";
  }
  try {
    HStreamWindow legacy_baseline_window;
    auto* mapping_backend = require_child<QComboBox>(&legacy_baseline_window, "mappingBackendCombo");
    auto* run_autooptimizer = require_child<QCheckBox>(&legacy_baseline_window, "runAutooptimizerCheck");
    ok &= expect(
        mapping_backend && run_autooptimizer && mapping_backend->currentData().toString() == "opencv-magsac" &&
            !run_autooptimizer->isChecked(),
        "A baseline predating stitching backend keys must start with MAGSAC and the autooptimizer disabled");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: Older baseline UI startup threw: " << error.what() << '\n';
    ok = false;
  }
  if (original_home.isEmpty())
    qunsetenv("HOME");
  else
    qputenv("HOME", original_home);
  if (original_config_root.isEmpty())
    qunsetenv("HM_CONFIG_ROOT");
  else
    qputenv("HM_CONFIG_ROOT", original_config_root);
  return ok;
}

bool test_window_close_stops_pipeline(HStreamWindow* window) {
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* pause = require_child<QPushButton>(window, "pausePipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* drivegpt_csv = require_child<QCheckBox>(window, "drivegptCsvCheck");
  auto* render_video = require_child<QCheckBox>(window, "renderVideoCheck");
  auto* seek_slider = require_child<QSlider>(window, "playbackSeekSlider");
  auto* seek_back = require_child<QPushButton>(window, "playbackSeekBack10Button");
  auto* seek_forward = require_child<QPushButton>(window, "playbackSeekForward10Button");
  auto* program_control_tabs = require_child<QTabWidget>(window, "programControlTabs");
  auto* stitched_control_tabs = require_child<QTabWidget>(window, "stitchedControlTabs");
  if (!start || !pause || !mode || !drivegpt_csv || !render_video || !seek_slider || !seek_back || !seek_forward ||
      !program_control_tabs || !stitched_control_tabs) {
    return false;
  }
  mode->setCurrentIndex(mode->findData("program"));
  for (QCheckBox* toggle : window->findChildren<QCheckBox*>()) {
    if (toggle->objectName().startsWith("outputToggle_"))
      toggle->setChecked(false);
  }
  render_video->setChecked(true);
  drivegpt_csv->setChecked(true);
  activate(start);
  for (int i = 0; i < 100 &&
       (window->pipelineStateText() != "PLAYING" ||
        HStreamWindowTestAccess::playbackDurationNs(window) != 600'000'000'000LL ||
        !seek_slider->toolTip().contains("DriveGPT CSV capture"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->pipelineStateText() == "PLAYING", "Close-event test pipeline should start")) {
    return false;
  }
  if (!expect(
          HStreamWindowTestAccess::playbackDurationNs(window) == 600'000'000'000LL && !seek_slider->isEnabled() &&
              !seek_back->isEnabled() && !seek_forward->isEnabled() &&
              seek_slider->toolTip().contains("DriveGPT CSV capture") &&
              seek_back->toolTip().contains("DriveGPT CSV capture") &&
              seek_forward->toolTip().contains("DriveGPT CSV capture"),
          "Lossless DriveGPT capture must disable every seek control and explain the reason on hover")) {
    return false;
  }
  const quint64 reset_generation_before_write_error = HStreamWindowTestAccess::playbackResetGeneration(window);
  HStreamWindowTestAccess::beginPendingResumedPlaybackSeek(window, 999);
  if (!expect(!pause->isEnabled(), "A pending playback seek should disable Pause before channel failure")) {
    return false;
  }
  HStreamWindowTestAccess::reportPipelineError(window, QProcess::WriteError);
  QApplication::processEvents();
  if (!expect(
          window->logText().contains("playback seek failed: pipeline command channel write error"),
          "A pending seek should report a command-channel write failure") ||
      !expect(pause->isEnabled(), "A command-channel write failure should restore Pause") ||
      !expect(
          !seek_slider->isEnabled() && !seek_forward->isEnabled() &&
              seek_slider->toolTip().contains("command channel failed"),
          "A command-channel write failure should permanently disable seeking for the run") ||
      !expect(
          HStreamWindowTestAccess::playbackResetGeneration(window) == reset_generation_before_write_error + 1 &&
              !HStreamWindowTestAccess::resumeProgressResetWaitingForSeek(window),
          "A command-channel write failure must fulfill a resumed seek's deferred progress-reset obligation")) {
    return false;
  }

  const quint64 reset_generation_before_read_error = HStreamWindowTestAccess::playbackResetGeneration(window);
  HStreamWindowTestAccess::beginPendingResumedPlaybackSeek(window, 1000);
  HStreamWindowTestAccess::reportPipelineError(window, QProcess::ReadError);
  QApplication::processEvents();
  if (!expect(
          window->logText().contains("playback seek failed: pipeline command channel read error") &&
              HStreamWindowTestAccess::playbackResetGeneration(window) == reset_generation_before_read_error + 1 &&
              !HStreamWindowTestAccess::resumeProgressResetWaitingForSeek(window),
          "A command-channel read failure must fulfill a resumed seek's deferred progress-reset obligation")) {
    return false;
  }

  HStreamWindowTestAccess::beginTimedOutPlaybackSeekRecovery(window, 1001);
  if (!expect(
          !pause->isEnabled() && !program_control_tabs->isEnabled() && !stitched_control_tabs->isEnabled(),
          "A timed-out reconstruction should keep transport and tuning disabled before command-channel failure")) {
    return false;
  }
  HStreamWindowTestAccess::reportPipelineError(window, QProcess::ReadError);
  QApplication::processEvents();
  if (!expect(
          window->logText().contains("playback seek failed: pipeline command channel read error") &&
              pause->isEnabled() && program_control_tabs->isEnabled() && stitched_control_tabs->isEnabled(),
          "A terminal command-channel error after reconstruction timeout must release every seek-recovery lock") ||
      !expect(
          !seek_slider->isEnabled() && !seek_forward->isEnabled() &&
              seek_slider->toolTip().contains("command channel failed"),
          "Recovery cleanup must not advertise seeking after the command channel has failed")) {
    return false;
  }

  HStreamWindowTestAccess::beginTimedOutPlaybackSeekRecovery(window, 1002);
  if (!expect(
          !pause->isEnabled() && !program_control_tabs->isEnabled() && !stitched_control_tabs->isEnabled(),
          "A second timed-out reconstruction should lock controls before process completion")) {
    return false;
  }
  if (!expect(
          HStreamWindowTestAccess::requestPipelineProcessExit(window) == 11,
          "The fake pipeline process must accept its exit command")) {
    return false;
  }
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->pipelineStateText() == "STOPPED" && !pause->isEnabled() && program_control_tabs->isEnabled() &&
              stitched_control_tabs->isEnabled(),
          "Process completion after reconstruction timeout must clear recovery state for the next run")) {
    return false;
  }
  const bool closed = window->close();
  QApplication::processEvents();
  return expect(closed, "Window close should complete after graceful pipeline shutdown") &&
      expect(window->pipelineStateText() == "STOPPED", "Window close should stop the pipeline process group");
}

bool prepare_real_e2e_game(const QString& game_id) {
  if (!qEnvironmentVariableIsSet("HSTREAM_UI_E2E_PREPARE_GAME")) {
    return true;
  }
  const fs::path game_dir = fs::path(qgetenv("HM_GAME_DIR").toStdString()) / game_id.toStdString();
  const fs::path config_path = game_dir / "config.yaml";
  const fs::path panorama_path = game_dir / "panorama.tif";
  if (!fs::is_regular_file(config_path) || !fs::is_regular_file(panorama_path)) {
    std::cerr << "E2E sandbox requires config.yaml and panorama.tif in " << game_dir << '\n';
    return false;
  }
  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    const int configured_control_points = qEnvironmentVariableIntValue("HSTREAM_UI_E2E_CONTROL_POINTS");
    config["hstream_ui"]["stitching_calibration"]["control_points"] =
        configured_control_points > 0 ? configured_control_points : 1500;
    config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    if (qEnvironmentVariableIsSet("HSTREAM_UI_E2E_REQUIRE_SCOREBOARD_SELECTOR")) {
      YAML::Node rink = config["rink"];
      if (rink && rink.IsMap()) {
        YAML::Node scoreboard = rink["scoreboard"];
        if (scoreboard && scoreboard.IsMap()) {
          scoreboard.remove("perspective_polygon");
        }
      }
    }
    const auto published = hm::stitching::publish_game_config(game_dir, YAML::Dump(config) + "\n");
    if (!published.ok()) {
      std::cerr << "Could not prepare E2E game config: " << published << '\n';
      return false;
    }
  } catch (const std::exception& exception) {
    std::cerr << "Could not prepare E2E game config: " << exception.what() << '\n';
    return false;
  }
  return true;
}

bool submit_no_scoreboard(HStreamWindow* window, QString* error) {
  QDialog* dialog = window ? window->findChild<QDialog*>("scoreboardSelectionDialog") : nullptr;
  QPushButton* button = dialog ? dialog->findChild<QPushButton*>("scoreboardNoScoreboardButton") : nullptr;
  if (!dialog || !button) {
    if (error) {
      *error = "native scoreboard selector dialog is not available";
    }
    return false;
  }
  // QMessageBox::question() enters a nested event loop. Poll through that loop: a single zero-delay lookup may run
  // before the confirmation becomes active and silently leave the default "No" selected.
  auto* confirmer = new QTimer(dialog);
  confirmer->setInterval(10);
  QObject::connect(confirmer, &QTimer::timeout, dialog, [confirmer]() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      auto* confirmation = qobject_cast<QMessageBox*>(widget);
      if (confirmation && confirmation->objectName() == "scoreboardNoScoreboardConfirmation" &&
          confirmation->isVisible()) {
        confirmer->stop();
        if (QAbstractButton* yes = confirmation->button(QMessageBox::Yes)) {
          yes->click();
        }
        return;
      }
    }
  });
  confirmer->start();
  button->click();
  confirmer->stop();
  confirmer->deleteLater();
  auto* status = dialog->findChild<QLabel*>("scoreboardStatusTitle");
  const bool submitted = status && status->text() == "Saving selection";
  if (!submitted && error) {
    *error = status ? QString("No-scoreboard confirmation did not submit (status: %1)").arg(status->text())
                    : "scoreboard submission status is unavailable";
  }
  return submitted;
}

QString find_encoded_e2e_output(const QString& output_root, const QString& game_id) {
  const QDir game_output(QDir(output_root).filePath(game_id));
  if (!game_output.exists()) {
    return {};
  }
  QFileInfo newest;
  const QFileInfoList candidates =
      game_output.entryInfoList({"*.mkv", "*.mp4", "*.mov"}, QDir::Files | QDir::Readable, QDir::Time);
  for (const QFileInfo& candidate : candidates) {
    if (candidate.size() > 64 * 1024 && (!newest.exists() || candidate.lastModified() > newest.lastModified())) {
      newest = candidate;
    }
  }
  return newest.exists() ? newest.absoluteFilePath() : QString();
}

bool write_e2e_text(const QString& path, const QString& contents) {
  const QByteArray bytes = contents.toUtf8();
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

struct NativePreviewCapture {
  bool passed{false};
  int width{0};
  int height{0};
  double mean_luminance{0.0};
  double luminance_deviation{0.0};
  int luminance_range{0};
};

QString diagnostic_capture_attempt_path(const QString& artifact_dir, const QString& output_name, int attempt_number) {
  const QFileInfo canonical_output(QDir(artifact_dir).filePath(output_name));
  return canonical_output.dir().filePath(QString("%1-attempt-%2.%3")
                                             .arg(canonical_output.completeBaseName())
                                             .arg(attempt_number)
                                             .arg(canonical_output.suffix()));
}

bool promote_diagnostic_capture_artifact(const QString& attempt_path, const QString& canonical_path) {
  if (QFileInfo::exists(canonical_path)) {
    return true;
  }
  return QFile::rename(attempt_path, canonical_path) || QFile::copy(attempt_path, canonical_path);
}

bool clear_diagnostic_capture_artifact(const QString& artifact_dir, const QString& output_name) {
  const QString canonical_path = QDir(artifact_dir).filePath(output_name);
  return !QFileInfo::exists(canonical_path) || QFile::remove(canonical_path);
}

bool test_diagnostic_capture_attempt_paths() {
  QTemporaryDir artifact_dir;
  const QString first = diagnostic_capture_attempt_path("/tmp/hstream-e2e", "program-preview.png", 1);
  const QString second = diagnostic_capture_attempt_path("/tmp/hstream-e2e", "program-preview.png", 2);
  const QString stale_failure = QString("runtime preview frame unavailable channel=program path=%1").arg(first);
  const QString successful_attempt = artifact_dir.filePath("successful-attempt.png");
  const QString canonical = artifact_dir.filePath("program-preview.png");
  const QString failed_attempt = artifact_dir.filePath("failed-attempt.png");
  const QString retry_attempt = artifact_dir.filePath("retry-attempt.png");
  const QString inaccessible_canonical = artifact_dir.filePath("missing/program-preview.png");
  const bool wrote_artifacts = write_e2e_text(successful_attempt, "captured") &&
      write_e2e_text(failed_attempt, "captured") && write_e2e_text(retry_attempt, "new capture") &&
      write_e2e_text(canonical, "stale capture");
  const bool cleared = wrote_artifacts && clear_diagnostic_capture_artifact(artifact_dir.path(), "program-preview.png");
  const bool promoted = cleared && promote_diagnostic_capture_artifact(successful_attempt, canonical);
  const qint64 published_size = QFileInfo(canonical).size();
  const bool retained = promote_diagnostic_capture_artifact(retry_attempt, canonical);
  const bool rejected = !promote_diagnostic_capture_artifact(failed_attempt, inaccessible_canonical);
  return expect(
      artifact_dir.isValid() && first != second && !stale_failure.contains(second) && cleared && promoted &&
          published_size > 0 && retained && QFileInfo(canonical).size() == published_size &&
          QFileInfo::exists(retry_attempt) && rejected && QFileInfo::exists(failed_attempt),
      "Diagnostic retries must use distinct markers, publish once, and retain a valid current-run artifact");
}

NativePreviewCapture inspect_native_preview_capture(const QString& output_path) {
  NativePreviewCapture capture;
  const QImage image = QImage(output_path).convertToFormat(QImage::Format_RGB32);
  capture.width = image.width();
  capture.height = image.height();
  if (capture.width < 100 || capture.height < 100) {
    return capture;
  }

  double luminance_sum = 0.0;
  double luminance_square_sum = 0.0;
  size_t samples = 0;
  int minimum_luminance = 255;
  int maximum_luminance = 0;
  for (int y = 0; y < image.height(); y += 4) {
    for (int x = 0; x < image.width(); x += 4) {
      const int luminance = qGray(image.pixel(x, y));
      luminance_sum += luminance;
      luminance_square_sum += static_cast<double>(luminance) * luminance;
      minimum_luminance = std::min(minimum_luminance, luminance);
      maximum_luminance = std::max(maximum_luminance, luminance);
      ++samples;
    }
  }
  if (samples == 0) {
    return capture;
  }
  capture.mean_luminance = luminance_sum / samples;
  const double variance =
      std::max(0.0, luminance_square_sum / samples - capture.mean_luminance * capture.mean_luminance);
  capture.luminance_deviation = std::sqrt(variance);
  capture.luminance_range = maximum_luminance - minimum_luminance;
  capture.passed = capture.luminance_deviation >= 8.0 && capture.luminance_range >= 40;
  return capture;
}

QString native_preview_report_line(const QString& name, const NativePreviewCapture& capture) {
  return QString("%1_preview: %2 width=%3 height=%4 mean_luminance=%5 luminance_deviation=%6 luminance_range=%7\n")
      .arg(name)
      .arg(capture.passed ? "PASS" : "FAIL")
      .arg(capture.width)
      .arg(capture.height)
      .arg(capture.mean_luminance, 0, 'f', 2)
      .arg(capture.luminance_deviation, 0, 'f', 2)
      .arg(capture.luminance_range);
}

bool run_real_pipeline_e2e(HStreamWindow* window, const QString& game_id) {
  auto* game_id_edit = require_child<QLineEdit>(window, "gameIdEdit");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* control_points = require_child<QSpinBox>(window, "controlPointsSpin");
  auto* render_video = require_child<QCheckBox>(window, "renderVideoCheck");
  auto* show_player_tracking = require_child<QCheckBox>(window, "showPlayerTrackingCheck");
  auto* show_play_tracking = require_child<QCheckBox>(window, "showPlayTrackingCheck");
  auto* show_rink_mask = require_child<QCheckBox>(window, "showRinkMaskCheck");
  auto* archive = require_child<QCheckBox>(window, "outputToggle_archive-file");
  auto* preview_tabs = require_child<QTabWidget>(window, "previewTabs");
  auto* program_surface = require_child<QWidget>(window, "previewSurface");
  auto* program_render_target = require_child<QWidget>(window, "previewRenderTarget");
  auto* stitched_surface = require_child<QWidget>(window, "stitchedPreviewSurface");
  auto* stitched_render_target = require_child<QWidget>(window, "stitchedPreviewRenderTarget");
  auto* camera1_surface = require_child<QWidget>(window, "camera1PreviewSurface");
  auto* camera1_render_target = require_child<QWidget>(window, "camera1PreviewRenderTarget");
  auto* playback_progress = require_child<QProgressBar>(window, "playbackProgress");
  if (!game_id_edit || !create || !start || !stop || !mode || !control_points || !render_video ||
      !show_player_tracking || !show_play_tracking || !show_rink_mask || !archive || !preview_tabs ||
      !program_surface || !program_render_target || !stitched_surface || !stitched_render_target || !camera1_surface ||
      !camera1_render_target || !playback_progress) {
    return false;
  }
  const bool verify_x11_preview = qEnvironmentVariableIsSet("HSTREAM_UI_E2E_VERIFY_X11_PREVIEW");

  QString artifact_dir = qEnvironmentVariable("HSTREAM_UI_E2E_ARTIFACT_DIR");
  if (artifact_dir.isEmpty()) {
    artifact_dir = QDir::current().filePath(QString("test-artifacts/hstream-ui-%1").arg(game_id));
  }
  if (!QDir().mkpath(artifact_dir)) {
    std::cerr << "Could not create E2E artifact directory: " << artifact_dir.toStdString() << '\n';
    return false;
  }
  std::cout << "HStream UI E2E artifacts: " << artifact_dir.toStdString() << '\n';
  if (verify_x11_preview &&
      (!clear_diagnostic_capture_artifact(artifact_dir, "program-preview-surface.png") ||
       !clear_diagnostic_capture_artifact(artifact_dir, "stitched-preview-surface.png") ||
       !clear_diagnostic_capture_artifact(artifact_dir, "camera1-preview-surface.png"))) {
    std::cerr << "Could not clear stale E2E preview artifacts in " << artifact_dir.toStdString() << '\n';
    return false;
  }

  game_id_edit->setText(game_id);
  activate(create);

  const QString run_mode = QString::fromLocal8Bit(qgetenv("HSTREAM_UI_E2E_RUN_MODE"));
  if (!run_mode.isEmpty()) {
    const int mode_index = mode->findData(run_mode);
    if (mode_index < 0) {
      std::cerr << "Missing e2e run mode: " << run_mode.toStdString() << '\n';
      return false;
    }
    mode->setCurrentIndex(mode_index);
  }
  const int configured_control_points = qEnvironmentVariableIntValue("HSTREAM_UI_E2E_CONTROL_POINTS");
  if (configured_control_points > 0) {
    control_points->setValue(configured_control_points);
  }
  if (render_video->isChecked() && !verify_x11_preview) {
    activate(render_video);
  } else if (!render_video->isChecked() && verify_x11_preview) {
    activate(render_video);
  }
  const QStringList preview_overlays =
      qEnvironmentVariable("HSTREAM_UI_E2E_PREVIEW_OVERLAYS").split(',', Qt::SkipEmptyParts);
  if (preview_overlays.contains("players") && !show_player_tracking->isChecked())
    activate(show_player_tracking);
  if (preview_overlays.contains("play") && !show_play_tracking->isChecked())
    activate(show_play_tracking);
  if (preview_overlays.contains("rink") && !show_rink_mask->isChecked())
    activate(show_rink_mask);
  if (!archive->isChecked()) {
    activate(archive);
  }
  activate(start);
  const auto stop_and_preserve_failure = [&]() {
    activate(stop);
    for (int i = 0; i < 300 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(100);
    }
    window->grab().save(QDir(artifact_dir).filePath("ui-failed.png"));
    write_e2e_text(QDir(artifact_dir).filePath("pipeline.log"), window->completeLogText());
  };

  const int timeout_ms = qEnvironmentVariableIntValue("HSTREAM_UI_E2E_TIMEOUT_MS");
  const int deadline_ms = timeout_ms > 0 ? timeout_ms : 120000;
  QElapsedTimer timer;
  timer.start();
  bool observed_first_frame = false;
  bool submitted_scoreboard = false;
  qint64 first_frame_at_ms = -1;
  QString interaction_error;
  NativePreviewCapture program_preview;
  NativePreviewCapture stitched_preview;
  NativePreviewCapture camera1_preview;
  bool x11_previews_captured = false;
  int x11_preview_attempts = 0;
  qint64 next_x11_preview_attempt_ms = 0;
  bool stitched_target_acknowledged = false;
  bool program_target_acknowledged = false;
  bool camera1_target_acknowledged = false;
  auto capture_x11_previews = [&]() {
    if (!verify_x11_preview || x11_previews_captured || window->pipelineStateText() != "PLAYING" ||
        timer.elapsed() < next_x11_preview_attempt_ms) {
      return;
    }
    ++x11_preview_attempts;
    interaction_error.clear();
    QProcess* pipeline = window->findChild<QProcess*>();
    if (!pipeline || pipeline->state() == QProcess::NotRunning) {
      interaction_error = "pipeline process is unavailable for diagnostic preview capture";
      return;
    }
    auto capture_channel = [&](const QString& channel, const QString& output_name) {
      const QFileInfo canonical_output(QDir(artifact_dir).filePath(output_name));
      const QString output_path = diagnostic_capture_attempt_path(artifact_dir, output_name, x11_preview_attempts);
      QFile::remove(output_path);
      const QByteArray command = QString("@capture-preview-frame %1 %2\n").arg(channel, output_path).toUtf8();
      if (pipeline->write(command) != command.size()) {
        interaction_error = QString("could not request a diagnostic %1 preview frame").arg(channel);
        return NativePreviewCapture{};
      }
      const QString completion = QString("runtime preview frame channel=%1 path=%2").arg(channel, output_path);
      const QString failure = QString("runtime preview frame failed channel=%1 path=%2").arg(channel, output_path);
      const QString unavailable =
          QString("runtime preview frame unavailable channel=%1 path=%2").arg(channel, output_path);
      const qint64 capture_deadline_ms = std::min<qint64>(deadline_ms, timer.elapsed() + 5000);
      while (timer.elapsed() < capture_deadline_ms) {
        QApplication::processEvents();
        if (window->completeLogText().contains(completion) && QFileInfo(output_path).size() > 0) {
          NativePreviewCapture capture = inspect_native_preview_capture(output_path);
          if (capture.passed) {
            const QString canonical_path = canonical_output.absoluteFilePath();
            if (!promote_diagnostic_capture_artifact(output_path, canonical_path)) {
              capture.passed = false;
              interaction_error =
                  QString("diagnostic %1 preview was captured but could not be published to %2; attempt remains at %3")
                      .arg(channel, canonical_path, output_path);
            }
          }
          return capture;
        }
        if (window->completeLogText().contains(failure) || window->completeLogText().contains(unavailable))
          break;
        QTest::qWait(50);
      }
      interaction_error = QString("diagnostic %1 preview capture did not complete").arg(channel);
      return NativePreviewCapture{};
    };
    auto wait_for_renderer = [&](QWidget* surface) {
      const qint64 renderer_deadline_ms = std::min<qint64>(deadline_ms, timer.elapsed() + 5000);
      while (timer.elapsed() < renderer_deadline_ms &&
             surface->property("previewRendererState").toString() != "ready") {
        QApplication::processEvents();
        QTest::qWait(50);
      }
      return surface->property("previewRendererState").toString() == "ready";
    };
    preview_tabs->setCurrentIndex(0);
    program_target_acknowledged = wait_for_renderer(program_surface);
    program_preview = capture_channel("program", "program-preview-surface.png");

    if (timer.elapsed() >= deadline_ms)
      return;
    preview_tabs->setCurrentIndex(2);
    QApplication::processEvents();
    camera1_target_acknowledged = wait_for_renderer(camera1_surface);
    camera1_preview = capture_channel("source0", "camera1-preview-surface.png");

    if (timer.elapsed() >= deadline_ms)
      return;
    preview_tabs->setCurrentIndex(1);
    stitched_target_acknowledged = wait_for_renderer(stitched_surface);
    stitched_preview = capture_channel("stitched", "stitched-preview-surface.png");

    if (timer.elapsed() >= deadline_ms)
      return;
    preview_tabs->setCurrentIndex(0);
    program_target_acknowledged = program_target_acknowledged || wait_for_renderer(program_surface);
    x11_previews_captured = program_target_acknowledged && stitched_target_acknowledged &&
        camera1_target_acknowledged && program_preview.passed && stitched_preview.passed && camera1_preview.passed;
    if (!x11_previews_captured && interaction_error.isEmpty())
      interaction_error = "one or more GPU preview captures were blank or not acknowledged";
    if (!x11_previews_captured) {
      next_x11_preview_attempt_ms = timer.elapsed() + 500;
    } else {
      interaction_error.clear();
    }
  };
  const QRegularExpression positive_fps(R"(\*\*PERF:\s+([0-9]+(?:\.[0-9]+)?))");
  while (timer.elapsed() < deadline_ms) {
    QApplication::processEvents();
    QTest::qWait(100);
    const QString log = window->completeLogText();
    if (log.contains("asset setup failed") || log.contains("pipeline process error")) {
      std::cerr << log.toStdString() << '\n';
      stop_and_preserve_failure();
      return false;
    }
    if (!submitted_scoreboard && !window->scoreboardSelectorUrl().isEmpty()) {
      submitted_scoreboard = submit_no_scoreboard(window, &interaction_error);
      if (!submitted_scoreboard) {
        std::cerr << "Could not submit the scoreboard selector: " << interaction_error.toStdString() << '\n';
        stop_and_preserve_failure();
        return false;
      }
    }
    auto match = positive_fps.globalMatch(log);
    while (match.hasNext()) {
      bool parsed = false;
      const double fps = match.next().captured(1).toDouble(&parsed);
      if (parsed && fps > 0.0) {
        observed_first_frame = true;
        if (first_frame_at_ms < 0) {
          first_frame_at_ms = timer.elapsed();
        }
        break;
      }
    }
    if (observed_first_frame) {
      capture_x11_previews();
    }
    const int configured_record_ms = qEnvironmentVariableIntValue("HSTREAM_UI_E2E_RECORD_MS");
    const int record_ms = configured_record_ms > 0 ? configured_record_ms : 6000;
    if (observed_first_frame && timer.elapsed() - first_frame_at_ms >= record_ms &&
        (!verify_x11_preview || x11_previews_captured)) {
      break;
    }
    if (window->pipelineStateText() == "STOPPED") {
      break;
    }
  }

  QString preview_report;
  preview_report += QString("x11_preview_requested: %1\n").arg(verify_x11_preview ? "true" : "false");
  if (verify_x11_preview) {
    preview_report += QString("x11_preview_attempts: %1\n").arg(x11_preview_attempts);
    preview_report += native_preview_report_line("program", program_preview);
    preview_report += native_preview_report_line("stitched", stitched_preview);
    preview_report += native_preview_report_line("camera1", camera1_preview);
    preview_report +=
        QString("stitched_target_acknowledged: %1\n").arg(stitched_target_acknowledged ? "true" : "false");
    preview_report += QString("program_target_acknowledged: %1\n").arg(program_target_acknowledged ? "true" : "false");
    preview_report += QString("camera1_target_acknowledged: %1\n").arg(camera1_target_acknowledged ? "true" : "false");
  }
  write_e2e_text(QDir(artifact_dir).filePath("preview-report.txt"), preview_report);

  window->grab().save(QDir(artifact_dir).filePath("ui-running.png"));
  const QString log = window->completeLogText();
  const bool observed_native_asset_setup = log.contains("pretrained assets will be verified by hstream-cli");
  const bool observed_command = log.contains("pipeline command");
  const QRegularExpression playback_fps_label(R"(\b[0-9]+\.[0-9]{2} FPS\b)");
  const bool observed_playback_progress = playback_progress->isVisible() && playback_progress->maximum() == 1000 &&
      playback_progress->toolTip().contains("Elapsed: 00:") && playback_progress->toolTip().contains("Remaining:") &&
      playback_progress->toolTip().contains("ETA:") && playback_progress->format().contains("ETA ") &&
      playback_fps_label.match(playback_progress->format()).hasMatch();
  const bool require_scoreboard = qEnvironmentVariableIsSet("HSTREAM_UI_E2E_REQUIRE_SCOREBOARD_SELECTOR");
  activate(stop);
  for (int i = 0; i < 300 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(100);
  }
  window->grab().save(QDir(artifact_dir).filePath("ui-stopped.png"));
  const QString final_log = window->completeLogText();
  write_e2e_text(QDir(artifact_dir).filePath("pipeline.log"), final_log);
  const bool program_channel_observed = final_log.contains("GPU preview ready channel=program");
  const bool stitched_channel_observed = final_log.contains("GPU preview ready channel=stitched");
  const bool camera1_channel_observed = final_log.contains("GPU preview ready channel=source0");

  QString log_issues;
  int log_issue_count = 0;
  const QRegularExpression issue_pattern(
      R"((warning|error|critical|failed))", QRegularExpression::CaseInsensitiveOption);
  for (const QString& line : final_log.split('\n')) {
    const bool expected_control_line =
        line.contains("may also log a model-engine-file open/deserialize warning") || line.contains("User Interrupted");
    if (!expected_control_line && issue_pattern.match(line).hasMatch()) {
      log_issues += line + '\n';
      ++log_issue_count;
    }
  }
  write_e2e_text(QDir(artifact_dir).filePath("log-issues.txt"), log_issues);
  const bool fatal_log_issue = final_log.contains("ERROR from element") || final_log.contains("FAILED_PRECONDITION:") ||
      final_log.contains("Segmentation fault") || final_log.contains("CUDA error:");

  const QString output_path = find_encoded_e2e_output(qEnvironmentVariable("HM_OUTPUT_WORK_DIR"), game_id);
  const QString panorama_path = QDir(window->gameDirectoryText()).filePath("panorama.tif");
  bool visual_match = false;
  QString visual_verifier_output;
  const QString visual_verifier = qEnvironmentVariable("HSTREAM_UI_E2E_VISUAL_VERIFIER");
  if (!output_path.isEmpty() && QFileInfo(panorama_path).isFile() && !visual_verifier.isEmpty()) {
    QProcess verifier;
    verifier.setProcessChannelMode(QProcess::MergedChannels);
    verifier.start(visual_verifier, {output_path, panorama_path, artifact_dir});
    if (verifier.waitForStarted(5000) && verifier.waitForFinished(180000)) {
      visual_verifier_output = QString::fromLocal8Bit(verifier.readAll()).trimmed();
      visual_match = verifier.exitStatus() == QProcess::NormalExit && verifier.exitCode() == 0;
    } else {
      visual_verifier_output = QString("visual verifier process failed: %1").arg(verifier.errorString());
    }
  } else {
    visual_verifier_output = "visual verifier, encoded output, or panorama.tif is unavailable";
  }
  write_e2e_text(QDir(artifact_dir).filePath("visual-verifier.log"), visual_verifier_output + "\n");

  QString report;
  report += QString("game_id: %1\n").arg(game_id);
  report += QString("preview_overlays: %1\n").arg(preview_overlays.join(','));
  report += QString("output: %1\n").arg(output_path);
  report += QString("panorama: %1\n").arg(panorama_path);
  report += QString("scoreboard_selector_observed: %1\n").arg(submitted_scoreboard ? "true" : "false");
  report += QString("positive_fps_observed: %1\n").arg(observed_first_frame ? "true" : "false");
  report += QString("playback_progress_observed: %1\n").arg(observed_playback_progress ? "true" : "false");
  report += QString("log_issue_lines: %1\n").arg(log_issue_count);
  report += QString("fatal_log_issue: %1\n").arg(fatal_log_issue ? "true" : "false");
  report += QString("x11_program_preview: %1\n")
                .arg(program_preview.passed ? "PASS" : (verify_x11_preview ? "FAIL" : "NOT_RUN"));
  report += QString("x11_stitched_preview: %1\n")
                .arg(stitched_preview.passed ? "PASS" : (verify_x11_preview ? "FAIL" : "NOT_RUN"));
  report += QString("x11_camera1_preview: %1\n")
                .arg(camera1_preview.passed ? "PASS" : (verify_x11_preview ? "FAIL" : "NOT_RUN"));
  report += QString("program_preview_channel: %1\n").arg(program_channel_observed ? "OBSERVED" : "MISSING");
  report += QString("stitched_preview_channel: %1\n").arg(stitched_channel_observed ? "OBSERVED" : "MISSING");
  report += QString("camera1_preview_channel: %1\n").arg(camera1_channel_observed ? "OBSERVED" : "MISSING");
  report += QString("visual_match: %1\n").arg(visual_match ? "PASS" : "FAIL");
  write_e2e_text(QDir(artifact_dir).filePath("report.txt"), report);
  std::cout << report.toStdString();
  if (!visual_verifier_output.isEmpty()) {
    std::cout << visual_verifier_output.toStdString() << '\n';
  }

  if (!expect(observed_native_asset_setup, "Real UI run should delegate native asset verification to hstream-cli") ||
      !expect(observed_command, "Real UI run should launch hstream-cli") ||
      !expect(
          !require_scoreboard || submitted_scoreboard,
          "Real UI run should launch and complete the scoreboard selector") ||
      !expect(observed_first_frame, "Real UI run should process frames at positive FPS") ||
      !expect(observed_playback_progress, "Real UI run should expose backend playback progress in the Qt bar") ||
      !expect(
          !verify_x11_preview || (stitched_target_acknowledged && program_target_acknowledged),
          "Program and Stitched tabs should be acknowledged as live native render targets") ||
      !expect(
          !verify_x11_preview || (program_preview.passed && stitched_preview.passed && camera1_preview.passed),
          "Program, Stitched, and Camera 1 surfaces should all contain non-blank video") ||
      !expect(
          !verify_x11_preview || (program_channel_observed && stitched_channel_observed && camera1_channel_observed),
          "Program, Stitched, and Camera 1 must be supplied by their distinct backend preview channels") ||
      !expect(!fatal_log_issue, "Real UI run should not emit a fatal pipeline log signature") ||
      !expect(!output_path.isEmpty(), "Archive output should contain a finalized encoded video") ||
      !expect(visual_match, "Encoded output should geometrically match panorama.tif")) {
    std::cerr << final_log.toStdString() << '\n';
    return false;
  }
  return expect(window->pipelineStateText() == "STOPPED", "Real UI run should stop cleanly after e2e smoke");
}

bool test_cleanup_transaction_protocol() {
#ifndef Q_OS_UNIX
  return true;
#else
  QTemporaryDir root;
  if (!root.isValid())
    return false;
  const auto write_file = [](const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
  };
  const auto cleanup_transaction = [](const QString& directory) {
    const QStringList names = QDir(directory).entryList(
        {".hstream-cleanup-v2-*"}, QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    return names.isEmpty() ? QString() : QDir(directory).filePath(names.front());
  };
  const auto file_identity = [](const QString& path, struct stat* identity) {
    return ::lstat(QFile::encodeName(path).constData(), identity) == 0;
  };

  bool unsupported_rename_fallback = true;
  for (const QByteArray& unsupported_errno : {QByteArray("EINVAL"), QByteArray("EOPNOTSUPP"), QByteArray("ENOSYS")}) {
    const QString unsupported_rename_dir =
        QDir(root.path()).filePath(QString("unsupported-rename-flags-%1").arg(QString::fromLatin1(unsupported_errno)));
    QDir().mkpath(unsupported_rename_dir);
    const QString unsupported_rename_target = QDir(unsupported_rename_dir).filePath("completed.mp4");
    struct stat unsupported_rename_stat{};
    QString unsupported_rename_error;
    const bool unsupported_rename_setup = write_file(unsupported_rename_target, "trusted NFS cleanup") &&
        file_identity(unsupported_rename_target, &unsupported_rename_stat);
    qputenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", unsupported_errno);
    const bool unsupported_rename_removed = unsupported_rename_setup &&
        hm::ui_internal::remove_owned_path_for_test(
                                                unsupported_rename_target,
                                                static_cast<quint64>(unsupported_rename_stat.st_dev),
                                                static_cast<quint64>(unsupported_rename_stat.st_ino),
                                                &unsupported_rename_error);
    qunsetenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
    unsupported_rename_fallback &= unsupported_rename_removed && !QFileInfo::exists(unsupported_rename_target) &&
        !QFileInfo::exists(unsupported_rename_target + ".hstream-cleanup-pin") &&
        cleanup_transaction(unsupported_rename_dir).isEmpty();
    if (!unsupported_rename_removed) {
      std::cerr << "unsupported rename fallback failed for " << unsupported_errno.constData() << ": "
                << unsupported_rename_error.toStdString() << '\n';
    }
  }
  unsupported_rename_fallback = expect(
      unsupported_rename_fallback, "UI cleanup must atomically fall back when no-replace rename flags are unsupported");

  const QString unsupported_race_dir = QDir(root.path()).filePath("unsupported-rename-source-race");
  QDir().mkpath(unsupported_race_dir);
  const QString unsupported_race_target = QDir(unsupported_race_dir).filePath("completed.mp4");
  struct stat unsupported_race_stat{};
  QString unsupported_race_error;
  const bool unsupported_race_setup = write_file(unsupported_race_target, "trusted cleanup race source") &&
      file_identity(unsupported_race_target, &unsupported_race_stat);
  qputenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "EOPNOTSUPP");
  qputenv("HSTREAM_UI_TEST_REPLACE_SOURCE_BEFORE_PRIVATE_RENAME_FALLBACK", "1");
  const bool unsupported_race_removed = unsupported_race_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                            unsupported_race_target,
                                            static_cast<quint64>(unsupported_race_stat.st_dev),
                                            static_cast<quint64>(unsupported_race_stat.st_ino),
                                            &unsupported_race_error);
  qunsetenv("HSTREAM_UI_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  qunsetenv("HSTREAM_UI_TEST_REPLACE_SOURCE_BEFORE_PRIVATE_RENAME_FALLBACK");
  QFile unsupported_race_file(unsupported_race_target);
  QFile unsupported_race_guard(unsupported_race_target + ".hstream-cleanup-pin");
  const bool unsupported_race_file_opened = unsupported_race_file.open(QIODevice::ReadOnly);
  const bool unsupported_race_guard_opened = unsupported_race_guard.open(QIODevice::ReadOnly);
  const bool unsupported_rename_race = expect(
      unsupported_race_setup && !unsupported_race_removed && unsupported_race_file_opened &&
          unsupported_race_file.readAll() == "injected foreign source before private rename fallback" &&
          unsupported_race_guard_opened && unsupported_race_guard.readAll() == "trusted cleanup race source",
      "Unsupported-flag cleanup fallback must atomically quarantine and retain a concurrent source replacement");

  const QString preclose_sync_dir = QDir(root.path()).filePath("nfs-preclose-sync-failure");
  QDir().mkpath(preclose_sync_dir);
  const QString preclose_sync_target = QDir(preclose_sync_dir).filePath("completed.mp4");
  struct stat preclose_sync_stat{};
  QString preclose_sync_error;
  const bool preclose_sync_setup = write_file(preclose_sync_target, "trusted NFS preclose recovery") &&
      file_identity(preclose_sync_target, &preclose_sync_stat);
  qputenv("HSTREAM_UI_TEST_FORCE_NFS_SILLY_RENAME_RETIREMENT", preclose_sync_target.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_NFS_PRECLOSE_PARENT_SYNC_FAILURE", preclose_sync_target.toLocal8Bit());
  const bool preclose_sync_removed = preclose_sync_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                         preclose_sync_target,
                                         static_cast<quint64>(preclose_sync_stat.st_dev),
                                         static_cast<quint64>(preclose_sync_stat.st_ino),
                                         &preclose_sync_error);
  qunsetenv("HSTREAM_UI_TEST_FORCE_NFS_SILLY_RENAME_RETIREMENT");
  qunsetenv("HSTREAM_UI_TEST_NFS_PRECLOSE_PARENT_SYNC_FAILURE");
  QString preclose_reconciliation_error;
  const bool preclose_reconciled =
      hm::ui_internal::reconcile_cleanup_directory_for_test(preclose_sync_dir, &preclose_reconciliation_error);
  QFile preclose_recovered_file(preclose_sync_target);
  const bool preclose_recovered_opened = preclose_recovered_file.open(QIODevice::ReadOnly);
  const bool preclose_sync_recovery = expect(
      preclose_sync_setup && !preclose_sync_removed && preclose_reconciled && preclose_recovered_opened &&
          preclose_recovered_file.readAll() == "trusted NFS preclose recovery" &&
          cleanup_transaction(preclose_sync_dir).isEmpty(),
      "NFS cleanup must restore the pinned identity if durability sync fails before releasing a silly-rename pin");
  if (!preclose_sync_recovery) {
    std::cerr << "preclose recovery result=" << preclose_sync_removed
              << " remove-error=" << preclose_sync_error.toStdString() << " reconciled=" << preclose_reconciled
              << " reconcile-error=" << preclose_reconciliation_error.toStdString()
              << " recovered-open=" << preclose_recovered_opened
              << " transaction=" << cleanup_transaction(preclose_sync_dir).toStdString() << '\n';
  }

  const QString committed_dir = QDir(root.path()).filePath("committed-interruption");
  QDir().mkpath(committed_dir);
  const QString committed_target = QDir(committed_dir).filePath("committed.mp4");
  struct stat committed_stat{};
  QString committed_error;
  const bool committed_setup =
      write_file(committed_target, "trusted committed UI cleanup") && file_identity(committed_target, &committed_stat);
  qputenv(
      "HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_RETIREMENT", (committed_target + ".hstream-cleanup-pin").toLocal8Bit());
  const bool committed_first = committed_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                   committed_target,
                                   static_cast<quint64>(committed_stat.st_dev),
                                   static_cast<quint64>(committed_stat.st_ino),
                                   &committed_error);
  qunsetenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_FALLBACK_RETIREMENT");
  const QString committed_transaction = cleanup_transaction(committed_dir);
  const bool committed_authenticated = !committed_transaction.isEmpty() &&
      QFileInfo::exists(QDir(committed_transaction).filePath("owner")) &&
      QFileInfo::exists(QDir(committed_transaction).filePath("committed")) &&
      QFileInfo::exists(QDir(committed_transaction).filePath("guard")) &&
      !QFileInfo::exists(QDir(committed_transaction).filePath("fallback")) &&
      !QFileInfo::exists(QDir(committed_transaction).filePath("entry"));
  QString committed_restart_error;
  const bool committed_restart =
      hm::ui_internal::reconcile_cleanup_directory_for_test(committed_dir, &committed_restart_error);
  bool ok = unsupported_rename_fallback && unsupported_rename_race && preclose_sync_recovery &&
      expect(committed_setup && !committed_first && committed_authenticated && committed_restart &&
                 !QFileInfo::exists(committed_target) && !QFileInfo::exists(committed_transaction),
             "UI cleanup must finish a durable commit interrupted between fallback and guard retirement");

  const QString pending_commit_dir = QDir(root.path()).filePath("pending-commit-publication");
  QDir().mkpath(pending_commit_dir);
  const QString pending_commit_target = QDir(pending_commit_dir).filePath("pending-commit.mp4");
  struct stat pending_commit_stat{};
  QString pending_commit_error;
  const bool pending_commit_setup = write_file(pending_commit_target, "trusted pending-commit UI cleanup") &&
      file_identity(pending_commit_target, &pending_commit_stat);
  qputenv("HSTREAM_UI_TEST_INTERRUPT_BEFORE_CLEANUP_COMMIT_PUBLISH", "1");
  const bool pending_commit_first = pending_commit_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                        pending_commit_target,
                                        static_cast<quint64>(pending_commit_stat.st_dev),
                                        static_cast<quint64>(pending_commit_stat.st_ino),
                                        &pending_commit_error);
  qunsetenv("HSTREAM_UI_TEST_INTERRUPT_BEFORE_CLEANUP_COMMIT_PUBLISH");
  const QString pending_commit_transaction = cleanup_transaction(pending_commit_dir);
  const bool pending_commit_unpublished = !pending_commit_transaction.isEmpty() &&
      QFileInfo::exists(QDir(pending_commit_transaction).filePath("owner")) &&
      QFileInfo::exists(QDir(pending_commit_transaction).filePath("committed.pending")) &&
      !QFileInfo::exists(QDir(pending_commit_transaction).filePath("committed")) &&
      QFileInfo::exists(QDir(pending_commit_transaction).filePath("guard")) &&
      QFileInfo::exists(QDir(pending_commit_transaction).filePath("fallback")) &&
      !QFileInfo::exists(pending_commit_target);
  QString pending_commit_restart_error;
  const bool pending_commit_restart =
      hm::ui_internal::reconcile_cleanup_directory_for_test(pending_commit_dir, &pending_commit_restart_error);
  QFile pending_commit_restored_file(pending_commit_target);
  const bool pending_commit_restored_opened = pending_commit_restored_file.open(QIODevice::ReadOnly);
  ok &= expect(
      pending_commit_setup && !pending_commit_first && pending_commit_unpublished && pending_commit_restart &&
          pending_commit_restored_opened &&
          pending_commit_restored_file.readAll() == "trusted pending-commit UI cleanup" &&
          !QFileInfo::exists(pending_commit_transaction),
      "UI interruption before atomic commit publication must roll deletion back without exposing a partial marker");

  const QString missing_fallback_dir = QDir(root.path()).filePath("missing-fallback-before-commit");
  QDir().mkpath(missing_fallback_dir);
  const QString missing_fallback_target = QDir(missing_fallback_dir).filePath("missing-fallback.mp4");
  struct stat missing_fallback_stat{};
  QString missing_fallback_error;
  const bool missing_fallback_setup = write_file(missing_fallback_target, "trusted missing-fallback UI cleanup") &&
      file_identity(missing_fallback_target, &missing_fallback_stat);
  qputenv(
      "HSTREAM_UI_TEST_REMOVE_FALLBACK_BEFORE_QUARANTINE",
      (missing_fallback_target + ".hstream-cleanup-pin").toLocal8Bit());
  const bool missing_fallback_first = missing_fallback_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                          missing_fallback_target,
                                          static_cast<quint64>(missing_fallback_stat.st_dev),
                                          static_cast<quint64>(missing_fallback_stat.st_ino),
                                          &missing_fallback_error);
  qunsetenv("HSTREAM_UI_TEST_REMOVE_FALLBACK_BEFORE_QUARANTINE");
  const QString missing_fallback_transaction = cleanup_transaction(missing_fallback_dir);
  const bool missing_fallback_uncommitted = !missing_fallback_transaction.isEmpty() &&
      QFileInfo::exists(QDir(missing_fallback_transaction).filePath("owner")) &&
      QFileInfo::exists(QDir(missing_fallback_transaction).filePath("guard")) &&
      !QFileInfo::exists(QDir(missing_fallback_transaction).filePath("committed")) &&
      !QFileInfo::exists(missing_fallback_target) &&
      !QFileInfo::exists(missing_fallback_target + ".hstream-cleanup-pin");
  QString missing_fallback_restart_error;
  const bool missing_fallback_restart =
      hm::ui_internal::reconcile_cleanup_directory_for_test(missing_fallback_dir, &missing_fallback_restart_error);
  QFile missing_fallback_restored_file(missing_fallback_target);
  const bool missing_fallback_restored_opened = missing_fallback_restored_file.open(QIODevice::ReadOnly);
  ok &= expect(
      missing_fallback_setup && !missing_fallback_first && missing_fallback_uncommitted && missing_fallback_restart &&
          missing_fallback_restored_opened &&
          missing_fallback_restored_file.readAll() == "trusted missing-fallback UI cleanup" &&
          !QFileInfo::exists(missing_fallback_transaction),
      "A missing public fallback must not bypass the UI commit protocol or lose its private guard");

  const QString foreign_dir = QDir(root.path()).filePath("foreign-fallback");
  const QString foreign_target = QDir(foreign_dir).filePath("foreign-fallback.mp4");
  const QString foreign_transaction =
      QDir(foreign_dir).filePath(".hstream-cleanup-v2-12345678-90ab-4cde-8fab-1234567890ab");
  QDir().mkpath(foreign_transaction);
  const QByteArray foreign_target_name = QFile::encodeName(QFileInfo(foreign_target).fileName());
  const bool foreign_setup = write_file(
                                 QDir(foreign_transaction).filePath("owner"),
                                 QByteArray("hstream-cleanup-v2\n") + foreign_target_name.toBase64()) &&
      write_file(QDir(foreign_transaction).filePath("guard"), "trusted private UI cleanup guard") &&
      write_file(QDir(foreign_transaction).filePath("fallback"), "foreign private UI cleanup fallback");
  QString foreign_error;
  const bool foreign_reconciled = hm::ui_internal::reconcile_cleanup_directory_for_test(foreign_dir, &foreign_error);
  QFile foreign_guard_file(QDir(foreign_transaction).filePath("guard"));
  QFile foreign_fallback_file(QDir(foreign_transaction).filePath("fallback"));
  const bool foreign_guard_opened = foreign_guard_file.open(QIODevice::ReadOnly);
  const bool foreign_fallback_opened = foreign_fallback_file.open(QIODevice::ReadOnly);
  ok &= expect(
      foreign_setup && !foreign_reconciled && !QFileInfo::exists(foreign_target) &&
          QFileInfo::exists(QDir(foreign_transaction).filePath("owner")) && foreign_guard_opened &&
          foreign_guard_file.readAll() == "trusted private UI cleanup guard" && foreign_fallback_opened &&
          foreign_fallback_file.readAll() == "foreign private UI cleanup fallback",
      "UI cleanup must not treat a foreign private fallback as authorization to delete its trusted guard");

  const QString failed_unlink_dir = QDir(root.path()).filePath("failed-private-unlink");
  QDir().mkpath(failed_unlink_dir);
  const QString failed_unlink_target = QDir(failed_unlink_dir).filePath("failed-private-unlink.mp4");
  struct stat failed_unlink_stat{};
  QString failed_unlink_error;
  const bool failed_unlink_setup = write_file(failed_unlink_target, "trusted failed-private-unlink UI cleanup") &&
      file_identity(failed_unlink_target, &failed_unlink_stat);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE", failed_unlink_target.toLocal8Bit());
  const bool failed_unlink_result = failed_unlink_setup &&
      hm::ui_internal::remove_owned_path_for_test(
                                        failed_unlink_target,
                                        static_cast<quint64>(failed_unlink_stat.st_dev),
                                        static_cast<quint64>(failed_unlink_stat.st_ino),
                                        &failed_unlink_error);
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_PRIVATE_ENTRY_UNLINK_FAILURE");
  const QString failed_unlink_transaction = cleanup_transaction(failed_unlink_dir);
  const bool failed_unlink_authenticated = !failed_unlink_transaction.isEmpty() &&
      QFileInfo::exists(QDir(failed_unlink_transaction).filePath("owner")) &&
      QFileInfo::exists(QDir(failed_unlink_transaction).filePath("entry")) &&
      QFileInfo::exists(QDir(failed_unlink_transaction).filePath("guard"));
  QString failed_unlink_restart_error;
  const bool failed_unlink_restart =
      hm::ui_internal::reconcile_cleanup_directory_for_test(failed_unlink_dir, &failed_unlink_restart_error);
  QFile failed_unlink_restored_file(failed_unlink_target);
  const bool failed_unlink_restored_opened = failed_unlink_restored_file.open(QIODevice::ReadOnly);
  ok &= expect(
      failed_unlink_setup && !failed_unlink_result && failed_unlink_authenticated && failed_unlink_restart &&
          failed_unlink_restored_opened &&
          failed_unlink_restored_file.readAll() == "trusted failed-private-unlink UI cleanup" &&
          !QFileInfo::exists(failed_unlink_transaction),
      "UI cleanup must retain authenticated ownership after a private unlink failure until restart rollback");

  const QString deep_cleanup_chain_dir = QDir(root.path()).filePath("deep-cleanup-chain");
  QDir().mkpath(deep_cleanup_chain_dir);
  const QStringList deep_cleanup_ids = {
      "00000000-0000-4000-8000-000000000010",
      "00000000-0000-4000-8000-000000000020",
      "00000000-0000-4000-8000-000000000030",
      "00000000-0000-4000-8000-000000000040",
      "00000000-0000-4000-8000-000000000050",
      "00000000-0000-4000-8000-000000000060",
  };
  bool deep_cleanup_chain_setup = true;
  for (qsizetype index = 0; index < deep_cleanup_ids.size(); ++index) {
    const QString transaction = QDir(deep_cleanup_chain_dir).filePath(".hstream-cleanup-v2-" + deep_cleanup_ids[index]);
    const QString target_name = index == 0
        ? "deep-cleanup-root.mp4"
        : ".hstream-reconcile-" + deep_cleanup_ids[index - 1] + "-target-ffffffff-ffff-4fff-8fff-ffffffffffff";
    deep_cleanup_chain_setup = deep_cleanup_chain_setup && QDir().mkpath(transaction) &&
        write_file(QDir(transaction).filePath("owner"),
                   QByteArray("hstream-cleanup-v2\n") + target_name.toLocal8Bit().toBase64());
  }
  QString deep_cleanup_chain_error;
  const bool deep_cleanup_chain_reconciled =
      hm::ui_internal::reconcile_cleanup_directory_for_test(deep_cleanup_chain_dir, &deep_cleanup_chain_error);
  const bool deep_cleanup_chain_retired =
      QDir(deep_cleanup_chain_dir)
          .entryList(
              {".hstream-cleanup-v2-*"}, QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)
          .isEmpty();
  ok &= expect(
      deep_cleanup_chain_setup && deep_cleanup_chain_reconciled && deep_cleanup_chain_retired,
      "UI cleanup reconciliation must reach a fixed point beyond the former five-pass dependency limit");

  const QString blocked_cleanup_chain_dir = QDir(root.path()).filePath("blocked-cleanup-chain");
  const QString blocked_outer_id = "00000000-0000-4000-8000-000000000070";
  const QString blocked_nested_id = "00000000-0000-4000-8000-000000000080";
  const QString blocked_outer = QDir(blocked_cleanup_chain_dir).filePath(".hstream-cleanup-v2-" + blocked_outer_id);
  const QString blocked_nested = QDir(blocked_cleanup_chain_dir).filePath(".hstream-cleanup-v2-" + blocked_nested_id);
  const QString blocked_nested_target =
      ".hstream-reconcile-" + blocked_outer_id + "-target-ffffffff-ffff-4fff-8fff-ffffffffffff";
  const bool blocked_cleanup_chain_setup = QDir().mkpath(blocked_outer) && QDir().mkpath(blocked_nested) &&
      write_file(QDir(blocked_outer).filePath("owner"),
                 QByteArray("hstream-cleanup-v2\n") + QByteArray("blocked-cleanup-root.mp4").toBase64()) &&
      write_file(QDir(blocked_nested).filePath("owner"),
                 QByteArray("hstream-cleanup-v2\n") + blocked_nested_target.toLocal8Bit().toBase64());
  const int blocked_nested_fd =
      ::open(QFile::encodeName(blocked_nested).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const bool blocked_cleanup_chain_locked =
      blocked_nested_fd >= 0 && ::flock(blocked_nested_fd, LOCK_EX | LOCK_NB) == 0;
  QString blocked_cleanup_chain_error;
  const bool blocked_cleanup_chain_reconciled =
      hm::ui_internal::reconcile_cleanup_directory_for_test(blocked_cleanup_chain_dir, &blocked_cleanup_chain_error);
  const bool blocked_cleanup_chain_retained = QFileInfo::exists(blocked_outer) && QFileInfo::exists(blocked_nested);
  if (blocked_nested_fd >= 0)
    ::close(blocked_nested_fd);
  QString blocked_cleanup_chain_resume_error;
  const bool blocked_cleanup_chain_resumed = hm::ui_internal::reconcile_cleanup_directory_for_test(
      blocked_cleanup_chain_dir, &blocked_cleanup_chain_resume_error);
  ok &= expect(
      blocked_cleanup_chain_setup && blocked_cleanup_chain_locked && !blocked_cleanup_chain_reconciled &&
          blocked_cleanup_chain_retained && blocked_cleanup_chain_resumed && !QFileInfo::exists(blocked_outer) &&
          !QFileInfo::exists(blocked_nested),
      "UI cleanup reconciliation must fail closed at a blocked fixed point and resume after the blocker releases");

  const QString concurrent_dir = QDir(root.path()).filePath("concurrent-removers");
  QDir().mkpath(concurrent_dir);
  const QString concurrent_target = QDir(concurrent_dir).filePath("concurrent.mp4");
  struct stat concurrent_stat{};
  const bool concurrent_setup = write_file(concurrent_target, "trusted concurrent UI cleanup") &&
      file_identity(concurrent_target, &concurrent_stat);
  std::atomic<bool> concurrent_start{false};
  bool concurrent_first = false;
  bool concurrent_second = false;
  QString concurrent_first_error;
  QString concurrent_second_error;
  qputenv("HSTREAM_UI_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION", concurrent_target.toLocal8Bit());
  const auto remove_concurrently = [&](bool* result, QString* error) {
    while (!concurrent_start.load(std::memory_order_acquire))
      std::this_thread::yield();
    *result = hm::ui_internal::remove_owned_path_for_test(
        concurrent_target,
        static_cast<quint64>(concurrent_stat.st_dev),
        static_cast<quint64>(concurrent_stat.st_ino),
        error);
  };
  std::thread concurrent_thread_one(remove_concurrently, &concurrent_first, &concurrent_first_error);
  std::thread concurrent_thread_two(remove_concurrently, &concurrent_second, &concurrent_second_error);
  concurrent_start.store(true, std::memory_order_release);
  concurrent_thread_one.join();
  concurrent_thread_two.join();
  qunsetenv("HSTREAM_UI_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION");
  const QStringList concurrent_artifacts =
      QDir(concurrent_dir)
          .entryList(
              {".hstream-cleanup-v2-*", "*.hstream-cleanup-pin"},
              QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
              QDir::Name);
  ok &= expect(
      concurrent_setup && concurrent_first && concurrent_second && !QFileInfo::exists(concurrent_target) &&
          concurrent_artifacts.isEmpty(),
      "UI cleanup must serialize concurrent removers before they share a deterministic fallback");

  const QString interrupted_concurrent_dir = QDir(root.path()).filePath("interrupted-concurrent-removers");
  QDir().mkpath(interrupted_concurrent_dir);
  const QString interrupted_concurrent_target = QDir(interrupted_concurrent_dir).filePath("interrupted-concurrent.mp4");
  struct stat interrupted_concurrent_stat{};
  const bool interrupted_concurrent_setup =
      write_file(interrupted_concurrent_target, "trusted interrupted concurrent UI cleanup") &&
      file_identity(interrupted_concurrent_target, &interrupted_concurrent_stat);
  std::atomic<bool> interrupted_concurrent_start{false};
  bool interrupted_concurrent_first = true;
  bool interrupted_concurrent_second = true;
  QString interrupted_concurrent_first_error;
  QString interrupted_concurrent_second_error;
  qputenv("HSTREAM_UI_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION", interrupted_concurrent_target.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE", interrupted_concurrent_target.toLocal8Bit());
  const auto remove_with_interruption = [&](bool* result, QString* error) {
    while (!interrupted_concurrent_start.load(std::memory_order_acquire))
      std::this_thread::yield();
    *result = hm::ui_internal::remove_owned_path_for_test(
        interrupted_concurrent_target,
        static_cast<quint64>(interrupted_concurrent_stat.st_dev),
        static_cast<quint64>(interrupted_concurrent_stat.st_ino),
        error);
  };
  std::thread interrupted_concurrent_thread_one(
      remove_with_interruption, &interrupted_concurrent_first, &interrupted_concurrent_first_error);
  std::thread interrupted_concurrent_thread_two(
      remove_with_interruption, &interrupted_concurrent_second, &interrupted_concurrent_second_error);
  interrupted_concurrent_start.store(true, std::memory_order_release);
  interrupted_concurrent_thread_one.join();
  interrupted_concurrent_thread_two.join();
  qunsetenv("HSTREAM_UI_TEST_DELAY_BEFORE_CLEANUP_SERIALIZATION");
  qunsetenv("HSTREAM_UI_TEST_INTERRUPT_AFTER_ARCHIVE_QUARANTINE");
  const QString interrupted_concurrent_transaction = cleanup_transaction(interrupted_concurrent_dir);
  QString interrupted_concurrent_restart_error;
  const bool interrupted_concurrent_restart = hm::ui_internal::reconcile_cleanup_directory_for_test(
      interrupted_concurrent_dir, &interrupted_concurrent_restart_error);
  QFile interrupted_concurrent_restored_file(interrupted_concurrent_target);
  const bool interrupted_concurrent_restored_opened = interrupted_concurrent_restored_file.open(QIODevice::ReadOnly);
  ok &= expect(
      interrupted_concurrent_setup && !interrupted_concurrent_first && !interrupted_concurrent_second &&
          !interrupted_concurrent_transaction.isEmpty() && interrupted_concurrent_restart &&
          interrupted_concurrent_restored_opened &&
          interrupted_concurrent_restored_file.readAll() == "trusted interrupted concurrent UI cleanup" &&
          !QFileInfo::exists(interrupted_concurrent_transaction),
      "A second UI remover must not report success while an interrupted concurrent transaction can restore the target");
  return ok;
#endif
}

bool test_early_finalization_failure_retains_log_guard(HStreamWindow* window, const QString& root) {
#ifndef Q_OS_UNIX
  Q_UNUSED(window);
  Q_UNUSED(root);
  return true;
#else
  const QString configured_path = QDir(root).filePath("early-failure-configured.mkv");
  const QString versioned_source =
      QDir(root).filePath("early-failure.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-aabbccddeeff.mkv");
  QString versioned_log;
  QString versioned_guard;
  HStreamWindowTestAccess::finishArchiveJobLogAfterFinalizationFailure(
      window, configured_path, versioned_source, "early-failure", &versioned_log, &versioned_guard);
  const bool versioned_pair_guarded = QFileInfo::exists(versioned_log) && QFileInfo::exists(versioned_guard);

  const QString recovered_source = QDir(root).filePath("early-failure-finalization-failed.mkv");
  QString recovered_log;
  QString recovered_guard;
  HStreamWindowTestAccess::finishArchiveJobLogAfterFinalizationFailure(
      window, configured_path, recovered_source, "recovered-failure", &recovered_log, &recovered_guard);
  const bool recovered_guard_retired = QFileInfo::exists(recovered_log) && !QFileInfo::exists(recovered_guard);

  QFile::remove(versioned_guard);
  QFile::remove(versioned_log);
  QFile::remove(recovered_guard);
  QFile::remove(recovered_log);
  return expect(
      versioned_pair_guarded && recovered_guard_retired,
      "An early finalization failure must retain the versioned log guard until backend recovery can move the pair");
#endif
}

bool test_wheel_routing_log_follow_and_calibration_analysis(HStreamWindow* window) {
  auto* stitched_tabs = require_child<QTabWidget>(window, "stitchedControlTabs");
  auto* algorithms_scroll = require_child<QScrollArea>(window, "stitchingAlgorithmsScrollArea");
  auto* mapping_backend = require_child<QComboBox>(window, "mappingBackendCombo");
  auto* max_width = require_child<QSpinBox>(window, "stitchMaxOutputWidthSpin");
  auto* auto_canvas = require_child<QCheckBox>(window, "projectionAutoCanvasCheck");
  auto* role_left = require_child<QRadioButton>(window, "videoRole_left");
  auto* runtime_log = require_child<QTextEdit>(window, "runtimeLog");
  if (!stitched_tabs || !algorithms_scroll || !mapping_backend || !max_width || !auto_canvas || !role_left ||
      !runtime_log) {
    return false;
  }

  const int original_tab = stitched_tabs->currentIndex();
  stitched_tabs->setCurrentIndex(stitched_tabs->count() - 1);
  QApplication::processEvents();
  QScrollBar* pane_scroll = algorithms_scroll->verticalScrollBar();
  pane_scroll->setValue(pane_scroll->minimum());
  const int backend_before = mapping_backend->currentIndex();
  QWheelEvent combo_wheel(
      mapping_backend->rect().center(),
      mapping_backend->mapToGlobal(mapping_backend->rect().center()),
      QPoint(),
      QPoint(0, -120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(mapping_backend, &combo_wheel);
  QApplication::processEvents();
  const bool combo_protected = mapping_backend->currentIndex() == backend_before;
  const bool pane_scrolled =
      pane_scroll->maximum() == pane_scroll->minimum() || pane_scroll->value() > pane_scroll->minimum();

  const int width_before = max_width->value();
  QWheelEvent spin_wheel(
      max_width->rect().center(),
      max_width->mapToGlobal(max_width->rect().center()),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(max_width, &spin_wheel);
  const bool spin_protected = max_width->value() == width_before;

  const bool canvas_before = auto_canvas->isChecked();
  QWheelEvent check_wheel(
      auto_canvas->rect().center(),
      auto_canvas->mapToGlobal(auto_canvas->rect().center()),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(auto_canvas, &check_wheel);
  const bool check_protected = auto_canvas->isChecked() == canvas_before;

  const bool role_before = role_left->isChecked();
  QWheelEvent radio_wheel(
      role_left->rect().center(),
      role_left->mapToGlobal(role_left->rect().center()),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(role_left, &radio_wheel);
  const bool radio_protected = role_left->isChecked() == role_before;
  stitched_tabs->setCurrentIndex(original_tab);

  HStreamWindowTestAccess::clearLog(window);
  for (int index = 0; index < 80; ++index)
    HStreamWindowTestAccess::appendLog(window, QString("tail-follow fixture %1").arg(index));
  QScrollBar* log_scroll = runtime_log->verticalScrollBar();
  log_scroll->setValue(log_scroll->maximum());
  HStreamWindowTestAccess::appendLog(window, "tail-follow newest");
  const bool follows_tail = log_scroll->value() == log_scroll->maximum();
  log_scroll->setValue(log_scroll->minimum());
  HStreamWindowTestAccess::appendLog(window, "manual-scroll newest");
  const bool preserves_manual_scroll = log_scroll->value() == log_scroll->minimum();
  HStreamWindowTestAccess::clearLog(window);

  HStreamWindowTestAccess::recordCalibrationDiagnostic(
      window, "Trying pooled stitching calibration across 4 frame pairs with 521 selected control points");
  HStreamWindowTestAccess::recordCalibrationDiagnostic(
      window, "Rejected calibrated MAGSAC hypothesis 1 with 84/521 inliers: FAILED_PRECONDITION: unsafe canvas extent");
  HStreamWindowTestAccess::recordCalibrationDiagnostic(
      window, "Skipping pooled stitching calibration: FAILED_PRECONDITION: OpenCV transform has unsafe canvas extent");
  const QString analysis = HStreamWindowTestAccess::calibrationFailureAnalysis(
      window, "No stitching calibration frame pair produced a usable solution after 2 candidate attempts");
  const bool diagnosis_is_actionable = analysis.contains("Why it failed") &&
      analysis.contains("unsafe or implausibly large canvas") && analysis.contains("What to try") &&
      analysis.contains("Bounded fallback search") && analysis.contains("1 projective hypothesis") &&
      analysis.contains("1 frame-set candidate") && analysis.contains("pressing Play is required");

  return expect(
             combo_protected && spin_protected && check_protected && radio_protected && pane_scrolled,
             "Mouse-wheel input over value controls must scroll the pane without changing values") &&
      expect(follows_tail && preserves_manual_scroll,
             "Runtime log must follow new output only while the operator remains at the bottom") &&
      expect(diagnosis_is_actionable,
             "Calibration failures must explain the cause, bounded fallbacks, and corrective action");
}

} // namespace

int main(int argc, char** argv) {
  hm::ui_internal::configure_application_identity();
  if (!test_path_scoped_auto_rollback() || !test_matching_development_runtime_selection() ||
      !test_stitching_canvas_constraint_decisions() || !test_diagnostic_capture_attempt_paths()) {
    return 1;
  }
  const QByteArray e2e_game_id = qgetenv("HSTREAM_UI_E2E_GAME_ID");
  if (!e2e_game_id.isEmpty()) {
    if (!prepare_real_e2e_game(QString::fromLocal8Bit(e2e_game_id))) {
      return 1;
    }
    QApplication app(argc, argv);
    HStreamWindow window;
    window.show();
    if (!run_real_pipeline_e2e(&window, QString::fromLocal8Bit(e2e_game_id))) {
      std::cerr << "run_real_pipeline_e2e failed\n";
      return 1;
    }
    return 0;
  }

  QTemporaryDir game_root;
  QTemporaryDir source_root;
  if (!game_root.isValid() || !source_root.isValid()) {
    return 1;
  }
  qputenv("HM_GAME_DIR", game_root.path().toLocal8Bit());
  qunsetenv("USE_NEW_NVSTREAMMUX");
  const QString fake_runner = source_root.path() + "/hstream-ui-fake-runner.sh";
  const QString fake_ffmpeg = source_root.path() + "/hstream-ui-fake-ffmpeg.py";
  const QString fake_sync = source_root.path() + "/hstream-ui-fake-sync.py";
  if (!write_fake_runner(fake_runner) || !write_fake_ffmpeg(fake_ffmpeg) || !write_fake_sync(fake_sync)) {
    return 1;
  }
  qputenv("HSTREAM_UI_TEST_RUNNER", fake_runner.toLocal8Bit());
  qputenv("HSTREAM_UI_FFMPEG", fake_ffmpeg.toLocal8Bit());
  qputenv("HSTREAM_UI_SYNC", fake_sync.toLocal8Bit());
  QApplication app(argc, argv);
  if (!test_cleanup_transaction_protocol()) {
    std::cerr << "test_cleanup_transaction_protocol failed\n";
    return 1;
  }
  HStreamWindow window;
  window.show();

  if (!test_wheel_routing_log_follow_and_calibration_analysis(&window)) {
    std::cerr << "test_wheel_routing_log_follow_and_calibration_analysis failed\n";
    return 1;
  }

  if (!test_early_finalization_failure_retains_log_guard(&window, source_root.path())) {
    std::cerr << "test_early_finalization_failure_retains_log_guard failed\n";
    return 1;
  }

  if (!test_game_setup(&window, source_root.path())) {
    std::cerr << "test_game_setup failed\n";
    return 1;
  }
  if (!test_nonzero_user_stitch_frame_default(window.gameDirectoryText())) {
    std::cerr << "test_nonzero_user_stitch_frame_default failed\n";
    return 1;
  }
  if (!test_calibration_progress_dialog(&window)) {
    std::cerr << "test_calibration_progress_dialog failed\n";
    return 1;
  }
  if (!test_pipeline_buttons(&window)) {
    std::cerr << "test_pipeline_buttons failed\n";
    return 1;
  }
  if (!test_projection_parameter_persistence(&window)) {
    std::cerr << "test_projection_parameter_persistence failed\n";
    return 1;
  }
  if (!test_output_controls(&window)) {
    std::cerr << "test_output_controls failed\n";
    return 1;
  }
  if (!test_dual_archive_finalization(&window)) {
    std::cerr << "test_dual_archive_finalization failed\n";
    return 1;
  }
  if (!test_camera_controls(&window)) {
    std::cerr << "test_camera_controls failed\n";
    return 1;
  }
  if (!test_window_close_stops_pipeline(&window)) {
    std::cerr << "test_window_close_stops_pipeline failed\n";
    return 1;
  }
  return 0;
}
