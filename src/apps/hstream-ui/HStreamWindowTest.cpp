#include "src/apps/hstream-ui/HStreamWindow.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
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
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QToolButton>

#include <yaml-cpp/yaml.h>

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
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
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
      "            print('HSTREAM_PREVIEW channel=' + initial_preview + ' status=ready generation=2 message=first "
      "GPU frame presented', flush=True)\n");
  file.write("calibration_result = os.environ.get('HSTREAM_UI_TEST_CALIBRATION_RESULT', '')\n");
  file.write("if not calibration_result and os.environ.get('HSTREAM_UI_TEST_COMPLETE_CALIBRATION') == '1':\n");
  file.write("    calibration_result = 'success'\n");
  file.write("if calibration_result in ('success', 'failure', 'exit'):\n");
  file.write("    time.sleep(float(os.environ.get('HSTREAM_UI_TEST_CALIBRATION_START_DELAY_MS', '0')) / 1000.0)\n");
  file.write("    delay = float(os.environ.get('HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS', '0')) / 1000.0\n");
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
  file.write(
      "        print('HSTREAM_CALIBRATION stage=rink-mask status=started message=Looking for the ice surface in the "
      "stitched panorama', flush=True)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=rink-mask status=complete message=Ice surface calibration is ready', "
      "flush=True)\n");
  file.write(
      "        print('HSTREAM_CALIBRATION stage=calibration status=complete message=Stitching calibration is "
      "complete', flush=True)\n");
  file.write("        print('hmstitcher: one-pass stitching configuration complete', flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_CLOSE_STDIN') == '1':\n");
  file.write("    sys.stdin.close()\n");
  file.write("    time.sleep(5.0)\n");
  file.write("    sys.exit(0)\n");
  file.write("def handle_stdin_line(line):\n");
  file.write(
      "    global preview_activation_count, preview_disable_stalled, stall_next_progress_reset, "
      "delayed_progress_generation, drop_progress_resets\n");
  file.write("    print('stdin:' + line.rstrip('\\n'), flush=True)\n");
  file.write("    if line.startswith('@test-stall-preview-disable'):\n");
  file.write("        preview_disable_stalled = True\n");
  file.write("        print('test preview disable stalled', flush=True)\n");
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
  file.write("    if line.startswith('@test-preview-status '):\n");
  file.write("        _, channel, status, generation = line.rstrip('\\n').split(' ', 3)\n");
  file.write(
      "        print('HSTREAM_PREVIEW channel=' + channel + ' status=' + status + ' generation=' + generation + "
      "' message=synthetic review regression', flush=True)\n");
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
      "            print('HSTREAM_PREVIEW channel=' + channel + ' status=ready generation=' + generation + "
      "' message=first GPU frame presented', flush=True)\n");
  file.write("    if line.startswith('@set-properties '):\n");
  file.write("        updates = line.rstrip('\\n').split(' ', 1)[1].split(';')\n");
  file.write("        reject = os.environ.get('HSTREAM_UI_TEST_REJECT_RUNTIME_CONTROL') == '1'\n");
  file.write("        stall = os.environ.get('HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL') == '1'\n");
  file.write("        if reject and updates:\n");
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
  file.write(
      "                print('runtime property ' + element + ' ' + property_name + '=' + runtime_value, flush=True)\n");
  file.write("preview_activation_count = 0\n");
  file.write("preview_disable_stalled = False\n");
  file.write("stall_next_progress_reset = False\n");
  file.write("delayed_progress_generation = ''\n");
  file.write("drop_progress_resets = False\n");
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
  file.write("if os.environ.get('HSTREAM_UI_TEST_FFMPEG_FAIL') == '1':\n");
  file.write("    if os.environ.get('HSTREAM_UI_TEST_FFMPEG_BLOCK_RECOVERY') == '1':\n");
  file.write("        os.chmod(os.path.dirname(source), 0o500)\n");
  file.write("    print('intentional remux failure', file=sys.stderr, flush=True)\n");
  file.write("    sys.exit(17)\n");
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
  file.write("if len(sys.argv) != 3 or sys.argv[1] != '-f' or not os.path.exists(sys.argv[2]):\n");
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
              rink_stage->property("calibrationState").toString() == "pending",
          "Native milestones should advance completed stages and mark the active failure stage") ||
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
  activate(start);
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
  const bool success_ok = expect(
                              !dialog->isVisible(),
                              "Successful rink-complete calibration should close the popup automatically") &&
      expect(window->pipelineStateText() == "PLAYING", "The pipeline should continue after the popup auto-closes") &&
      expect(has_completed_status && completed_status.as<std::string>() == "complete",
             "Only the final rink-complete milestone should persist completed calibration") &&
      expect(rink_stage->property("calibrationState").toString() == "complete",
             "Successful calibration should complete the final ice-surface stage");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  if (!success_ok || !set_test_calibration_status(window, "complete"))
    return false;

  mode->setCurrentIndex(mode->findData("program"));
  qputenv("HSTREAM_UI_TEST_CALIBRATION_RESULT", "success");
  qputenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS", "40");
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
          "Program playback should log why it opened the calibration progress popup");
  for (int i = 0; i < 400 &&
       (dialog->isVisible() ||
        !window->logText().contains("one-pass stitching calibration complete; continuous program playback running"));
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool program_discovery_completed =
      expect(!dialog->isVisible(), "Successful Program calibration should close the progress popup automatically") &&
      expect(
          window->pipelineStateText() == "PLAYING",
          "Program playback should continue after runtime-discovered calibration completes");
  activate(stop);
  for (int i = 0; i < 200 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_RESULT");
  qunsetenv("HSTREAM_UI_TEST_CALIBRATION_STEP_DELAY_MS");

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
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* render_video = require_child<QCheckBox>(window, "renderVideoCheck");
  auto* log = require_child<QTextEdit>(window, "runtimeLog");
  auto* clear_log = require_child<QPushButton>(window, "clearLogButton");
  auto* main_log_splitter = require_child<QSplitter>(window, "mainLogSplitter");
  auto* setup_preview_splitter = require_child<QSplitter>(window, "setupPreviewSplitter");
  auto* output_routing = require_child<QWidget>(window, "outputRoutingGroup");
  auto* preview_tabs = require_child<QTabWidget>(window, "previewTabs");
  auto* program_host = require_child<QWidget>(window, "programLetterboxHost");
  auto* preview_surface = require_child<QWidget>(window, "previewSurface");
  auto* preview_target = require_child<QWidget>(window, "previewRenderTarget");
  auto* stitched_surface = require_child<QWidget>(window, "stitchedPreviewSurface");
  auto* stitched_target = require_child<QWidget>(window, "stitchedPreviewRenderTarget");
  auto* camera1_host = require_child<QWidget>(window, "camera1LetterboxHost");
  auto* camera1_surface = require_child<QWidget>(window, "camera1PreviewSurface");
  auto* camera1_target = require_child<QWidget>(window, "camera1PreviewRenderTarget");
  auto* camera1_focus = require_child<QPushButton>(window, "camera1FocusButton");
  auto* camera2_surface = require_child<QWidget>(window, "camera2PreviewSurface");
  auto* camera3_surface = require_child<QWidget>(window, "camera3PreviewSurface");
  auto* external_notice = require_child<QLabel>(window, "programExternalRenderNotice");
  auto* camera1_notice = require_child<QLabel>(window, "camera1ExternalRenderNotice");
  auto* stitched_status = require_child<QLabel>(window, "stitchedPreviewStatusLabel");
  auto* program_controls = require_child<QWidget>(window, "programAssociatedControls");
  auto* program_controls_toggle = require_child<QToolButton>(window, "programControlsToggle");
  auto* stitched_controls = require_child<QWidget>(window, "stitchedAssociatedControls");
  auto* program_control_tabs = require_child<QTabWidget>(window, "programControlTabs");
  auto* stitched_control_tabs = require_child<QTabWidget>(window, "stitchedControlTabs");
  auto* program_focus = require_child<QPushButton>(window, "programFocusButton");
  auto* top_bar = require_child<QWidget>(window, "topBarPanel");
  auto* playback_progress = require_child<QProgressBar>(window, "playbackProgress");
  auto* setup_row = require_child<QWidget>(window, "setupControlsRow");
  auto* log_panel = require_child<QWidget>(window, "logPanel");
  auto* pipeline_process = window->findChild<QProcess*>();
  if (!stop || !start || !pause || !restart || !mode || !control_points || !game_id || !rotate || !max_speed_x ||
      !render_video || !log || !clear_log || !main_log_splitter || !setup_preview_splitter || !output_routing ||
      !preview_tabs || !program_host || !preview_surface || !preview_target || !stitched_surface || !stitched_target ||
      !camera1_host || !camera1_surface || !camera1_target || !camera1_focus || !camera2_surface || !camera3_surface ||
      !external_notice || !camera1_notice || !stitched_status || !program_controls || !program_controls_toggle ||
      !stitched_controls || !program_control_tabs || !stitched_control_tabs || !program_focus || !top_bar ||
      !setup_row || !log_panel || !playback_progress || !pipeline_process) {
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
  if (!expect(
          program_controls->isAncestorOf(max_speed_x) && stitched_controls->isAncestorOf(rotate) &&
              !camera1_host->isAncestorOf(max_speed_x) && !camera1_host->isAncestorOf(rotate) &&
              program_control_tabs->count() == 3 && stitched_control_tabs->count() == 1,
          "Controls must live in the earliest preview tab whose frames reflect their pipeline stage")) {
    return false;
  }
  if (!expect(
          program_focus->parentWidget() == program_host && program_focus->size() == QSize(24, 24) &&
              program_focus->x() == program_host->width() - program_focus->width() - 6 && program_focus->y() == 6 &&
              program_focus->toolTip().contains("Expand the Program preview") &&
              program_focus->accessibleName() == "Focus video" && program_focus->isHidden() &&
              !program_focus->isEnabled(),
          "The compact focus control must stay hidden until its preview has presented a GPU frame") ||
      !expect_x11_widget_state(
          program_focus,
          false,
          "The stopped focus control must remain an unmapped native child of the video host",
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
    return false;
  }
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
  if (!expect(control_points->value() == 1500, "Stitching calibration CP default should be 1500")) {
    return false;
  }

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
  const fs::path fresh_program_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const YAML::Node fresh_program_saved = YAML::LoadFile(fresh_program_config.string());
  YAML::Node fresh_program_status;
  const bool has_fresh_program_status =
      lookup_yaml_path(fresh_program_saved, {"hstream_ui", "stitching_calibration", "status"}, &fresh_program_status);
  const bool
      fresh_program_tracked =
          expect(
              window->logText().count("stitching calibration clean command") == fresh_program_clean_commands + 1,
              "A fresh Program run should establish tracked one-pass stitching calibration (before=" +
                  std::to_string(fresh_program_clean_commands) +
                  ", after=" + std::to_string(window->logText().count("stitching calibration clean command")) + ")") &&
      expect(has_fresh_program_status && fresh_program_status.IsScalar() &&
                 fresh_program_status.as<std::string>() == "complete",
             "A fresh Program one-pass calibration should persist completed state") &&
      expect(window->logText().contains("GPU preview backend ready channel=program generation=2"),
             "Program startup must acknowledge the selected GPU preview without a tab change") &&
      expect(window->logText().contains("GPU preview first-frame wait exceeded channel=program") &&
                 window->logText().contains("GPU preview requested channel=program generation=3 reason=recovery") &&
                 window->logText().contains("GPU preview ready channel=program generation="),
             "A delayed Program first frame must recover by reactivating the same tab") &&
      expect(!window->logText().contains("stdin:@set-preview-active none") && !preview_surface->isHidden() &&
                 !preview_target->isHidden(),
             "First-frame recovery must never deactivate or hide the selected Program preview");
  if (!expect_x11_widget_state(
          preview_target, true, "Playing Program target must remain aligned with its Qt video host")) {
    return false;
  }
  const int pipeline_start_count = window->logText().count("pipeline started pid=");
  const int ready_count_before_runtime_toggle = window->logText().count("GPU preview ready channel=program");
  if (!expect(
          render_video->isEnabled() && setup_preview_splitter->sizes().at(0) == 0 && program_focus->isVisible() &&
              program_focus->isEnabled() && program_controls->isHidden() && program_controls_toggle->isVisible(),
          "A live embedded preview should enable focus and collapse setup and associated controls for more video "
          "space")) {
    return false;
  }
  if (!capture_interaction_artifact(window, "playing-video-layout.png"))
    return false;
  QTest::mouseClick(render_video, Qt::LeftButton);
  for (int i = 0; i < 100 && !window->logText().contains("GPU preview disabled generation="); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          !render_video->isChecked() && render_video->isEnabled() && preview_target->isHidden() &&
              program_focus->isHidden() && setup_preview_splitter->sizes().at(0) > 0 &&
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
  for (int i = 0;
       i < 100 && window->logText().count("GPU preview ready channel=program") <= ready_count_before_runtime_toggle;
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          render_video->isChecked() && !preview_target->isHidden() && program_focus->isVisible() &&
              program_focus->isEnabled() && setup_preview_splitter->sizes().at(0) == 0 &&
              window->logText().contains("reason=render-toggle") &&
              window->logText().contains("stdin:@set-render-audio-muted 0") &&
              window->logText().count("pipeline started pid=") == pipeline_start_count,
          "Turning rendering back on must restore preview and local monitor audio without restarting the pipeline")) {
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
  QTest::mouseClick(render_video, Qt::LeftButton);
  QApplication::processEvents();
  QTest::qWait(40);
  if (!expect(
          !render_video->isChecked() && preview_target->isHidden() && program_focus->isHidden() &&
              setup_preview_splitter->sizes().at(0) > 0 &&
              window->logText().count("GPU preview disabled generation=") == disabled_count_before_paused_toggle &&
              window->logText().contains("GPU preview will finish disabling when the paused pipeline resumes"),
          "Render-off while paused must immediately unmap native targets and defer its acknowledgement safely")) {
    return false;
  }
  activate(pause);
  for (int i = 0; i < 100 && !window->logText().contains("stdin:@reset-progress-rate"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("stdin:@reset-progress-rate") &&
              playback_progress->toolTip().contains("Pipeline: PLAYING") &&
              playback_progress->toolTip().contains("ETA: Warming up") &&
              playback_progress->toolTip().contains("Processing speed: Warming up") &&
              !playback_progress->toolTip().contains("Processing speed: 0.50x"),
          "Resuming should reset every backend rate and suppress contaminated multi-pipeline samples")) {
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
       !window->logText().contains("playback speed reset was not acknowledged; using recovered adjacent-sample rate");
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

  QTest::mouseDClick(preview_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          !top_bar->isVisible() && !setup_row->isVisible() && !log_panel->isVisible() &&
              !preview_tabs->tabBar()->isVisible() && !program_controls->isVisible() && program_host->isVisible() &&
              playback_progress->isVisible() && !window->isFullScreen() &&
              program_focus->toolTip().contains("Restore the normal HStream layout") &&
              program_focus->accessibleName() == "Restore HStream controls",
          "A real double-click on a ready GPU preview should focus it across the HStream app area")) {
    return false;
  }
  window->resize(1500, 920);
  QApplication::processEvents();
  if (!expect_x11_widget_state(
          preview_target, true, "A focused playing target must preserve its native parent and geometry after resize")) {
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
              program_controls_toggle->isVisible(),
          "A real click on the high-contrast restore control should restore the normal UI")) {
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
  preview_tabs->setCurrentIndex(1);
  for (int i = 0; i < 100 && !window->logText().contains("GPU preview ready channel=stitched generation="); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect_x11_widget_state(
          preview_target,
          false,
          "Failed Program target must remain unmapped after switching to a healthy preview",
          false) ||
      !expect_x11_widget_state(stitched_target, true, "Selected Stitched target must be mapped inside its Qt host")) {
    return false;
  }
  preview_tabs->setCurrentIndex(2);
  for (int i = 0; i < 100 && !window->logText().contains("GPU preview ready channel=source0 generation="); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect_x11_widget_state(
          stitched_target, false, "Inactive Stitched target must be unmapped after switching to Camera 1") ||
      !expect_x11_widget_state(camera1_target, true, "Selected Camera 1 target must be mapped inside its Qt host")) {
    return false;
  }
  QTest::mouseDClick(camera1_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          camera1_host->isVisible() && !preview_tabs->tabBar()->isVisible() && camera1_focus->isVisible(),
          "Every ready camera preview should support the same in-app focus mode")) {
    return false;
  }
  QTest::mouseDClick(camera1_target, Qt::LeftButton);
  QApplication::processEvents();
  if (!expect(
          preview_tabs->tabBar()->isVisible() && top_bar->isVisible(),
          "Double-clicking a focused camera preview should restore the normal layout")) {
    return false;
  }
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
  const bool x11_calibration_preview_ok = window->logText().contains("--ui-preview-windows=program:") &&
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
              !window->logText().contains("pipeline.hmplaycropper.enable=0") &&
              !window->logText().contains("pipeline.ds-playtracker.enable=0"),
          "Stitched preview should batch both cameras on the normal pipeline without legacy OpenGL debug windows") ||
      !expect(
          window->logText().contains("HM_MAX_CONTROL_POINTS=750"),
          "One-pass calibration should pass the selected control-point limit") ||
      !expect(
          QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0
              ? x11_calibration_preview_ok
              : !window->logText().contains("--render-window-id=") &&
                  window->logText().contains("HM_RENDER_SINK=nv3dsink") &&
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
    if (!expect(
            has_saved_control_points && saved_control_points.IsScalar() && saved_control_points.as<int>() == 750,
            "Calibration CP count should be saved to private config") ||
        !expect(
            has_saved_status && saved_status.IsScalar() && saved_status.as<std::string>() == "pending",
            "Calibration CP state should remain pending while the calibration process is running") ||
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
          window->logText().contains("stdin:@set-property hmstitcher0 post-stitch-rotate-degrees=16"),
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
  if (!expect(window->pipelineStateText() == "STOPPED", "Stop should terminate calibration process")) {
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
             QString("camera control Max_Speed_X_x10=%1 apply=live").arg(original_max_speed_x + 1));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    auto contains_runtime_snapshot = [](const fs::path& dir) {
      if (!fs::exists(dir))
        return false;
      return std::any_of(fs::directory_iterator(dir), fs::directory_iterator(), [](const fs::directory_entry& entry) {
        return entry.path().filename().string().rfind("play_tracker_runtime_", 0) == 0;
      });
    };
    const bool runtime_control_used_launched_game =
        contains_runtime_snapshot(active_runtime_dir) && !contains_runtime_snapshot(switched_runtime_dir);
    game_id->setText(launched_game_id);
    max_speed_x->setValue(original_max_speed_x);
    for (int i = 0; i < 50 &&
         !window->logText().contains(QString("camera control Max_Speed_X_x10=%1 apply=live").arg(original_max_speed_x));
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
                !window->logText().contains("pipeline.hmplaycropper.enable=0"),
            "Continuous preview should stay on the normal pipeline without legacy OpenGL debug windows") ||
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
            runtime_control_used_launched_game,
            "Live controls should write runtime config for the game that launched the pipeline")) {
      qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
      activate(stop);
      return false;
    }
    rotate->setValue(73);
    for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=73 apply=live"); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (!expect(
            window->logText().contains("camera control Stitch_Rotate_Degrees=73 apply=live"),
            "Stitch controls should remain live after one-pass calibration completes")) {
      qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
      activate(stop);
      return false;
    }
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
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
    auto* fixed_edge_link = require_child<QSlider>(window, "cameraSlider_Link_Fixed_Edge_Rotation_Left_Right");
    auto* fixed_edge_left = require_child<QSlider>(window, "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10");
    if (!fixed_edge_link || !fixed_edge_left) {
      activate(stop);
      return false;
    }
    fixed_edge_link->setValue(1);
    fixed_edge_left->setValue(310);
    for (int i = 0;
         i < 50 && !window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=failed");
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
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
            window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=pending") &&
                window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=failed") &&
                !window->logText().contains("camera control Left_Fixed_Edge_Rotation_Angle_x10=310 apply=live"),
            "A multi-stage fixed-edge update should fail as one batch when its property commands are rejected")) {
      return false;
    }

    qputenv("HSTREAM_UI_TEST_CLOSE_STDIN", "1");
    render_video->setChecked(false);
    const int disable_recoveries_before_write_error = window->logText().count("GPU preview disable failed (");
    activate(start);
    for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    QTest::qWait(100);
    rotate->setValue(70);
    for (int i = 0; i < 50 && !window->logText().contains("pipeline remains running"); ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const bool write_error_kept_running = window->pipelineStateText() == "PLAYING";
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
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_CLOSE_STDIN");
    render_video->setChecked(true);
    if (!write_error_kept_rendering_disabled) {
      std::cerr << "write-error render state: checked=" << !rendering_stayed_unchecked
                << " target-mapped=" << !target_stayed_unmapped << " backend-disabled=" << backend_reported_disabled
                << " disable-recoveries=" << disable_recoveries_before_write_error << "->"
                << disable_recoveries_after_write_error << '\n';
    }
    if (!expect(write_error_kept_running, "A runtime-control write error should not mark live playback stopped") ||
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
  const bool disabled_sink_selection =
      fake_sink_commands_after == fake_sink_commands_before + (x11_test_backend ? 0 : 2) &&
      render_sink_commands_after == render_sink_commands_before + (x11_test_backend ? 2 : 0);
  if (!disabled_sink_selection) {
    std::cerr << "disabled render sink counts: platform=" << QGuiApplication::platformName().toStdString()
              << " fake=" << fake_sink_commands_before << "->" << fake_sink_commands_after
              << " render=" << render_sink_commands_before << "->" << render_sink_commands_after << '\n';
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
          window->logText().count("--ui-preview-windows=") ==
                  gpu_preview_commands_before + (x11_test_backend ? 1 : 0) &&
              window->logText().count("--ui-preview-active=none") ==
                  inactive_preview_commands_before + (x11_test_backend ? 1 : 0),
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
  auto* archive_path = require_child<QLabel>(window, "archiveOutputPath");
  auto* game_id_edit = require_child<QLineEdit>(window, "gameIdEdit");
  auto* youtube_redirect = require_child<QPushButton>(window, "redirectYoutubeButton");
  auto* add_rtsp = require_child<QPushButton>(window, "addRtspButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  if (!spare || !archive || !archive_path || !game_id_edit || !youtube_redirect || !add_rtsp || !start || !stop) {
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
  const QString restarted_recovery_path =
      QDir(QFileInfo(expected_path).absolutePath()).filePath("custom-archive-finalization-failed.mkv");
  QDir().mkpath(QFileInfo(expected_path).absolutePath());
  QFile::remove(planned_path);
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
  qunsetenv("HSTREAM_UI_TEST_ARCHIVE_RECOVER_EXISTING");

  const QString completed_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("completed-source.mkv");
  const QString concurrent_completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio.mp4").arg(window->gameIdText()));
  const QString completed_target =
      QDir(window->gameDirectoryText())
          .filePath(QString("%1-tracking_output-with-audio-1.mp4").arg(window->gameIdText()));
  const QString ffmpeg_arguments = QDir(output_root.path()).filePath("ffmpeg-arguments.txt");
  QFile::remove(completed_source);
  QFile::remove(concurrent_completed_target);
  QFile::remove(completed_target);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", completed_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_ARCHIVE_WRITE", "1");
  qputenv("HSTREAM_UI_TEST_EXIT_AFTER_PROGRESS", "0");
  qputenv("HSTREAM_UI_TEST_FFMPEG_ARGS", ffmpeg_arguments.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_SYNC_DELAY", "0.25");
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
  for (int i = 0; i < 300 && window->outputStateText("archive-file") != "SAVED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qunsetenv("HSTREAM_UI_TEST_SYNC_DELAY");
  QFile argument_file(ffmpeg_arguments);
  const bool opened_arguments = argument_file.open(QIODevice::ReadOnly);
  const QString argument_text = opened_arguments ? QString::fromUtf8(argument_file.readAll()) : QString();
  const bool archive_deployed = expect(
      finalizer_owner_lock_held && concurrent_completed_archive_created &&
          window->outputStateText("archive-file") == "SAVED" && QFileInfo(completed_target).size() > 0 &&
          QFileInfo(concurrent_completed_target).size() > 0 && !QFileInfo::exists(completed_source) &&
          !QFileInfo::exists(finalizer_owner_lock_path) && argument_text.contains("-n\n") &&
          !argument_text.contains("-y\n") &&
          argument_text.contains(
              QString("/.%1-tracking_output-with-audio.hstream-finalize-").arg(window->gameIdText())) &&
          argument_text.contains("-c\ncopy") && argument_text.contains("-movflags\n+faststart") &&
          argument_text.contains("-tag:v\nhvc1") &&
          window->logText().contains(QString("completed archive published: %1").arg(completed_target)),
      "Concurrent completion must retain finalizer ownership, keep the first MP4, publish a suffixed lossless "
      "faststart MP4, and remove only its own work file");

  for (int i = 0; i < 100 && finalize_dialog && finalize_dialog->isVisible(); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const QString failed_source =
      QDir(QDir(output_root.path()).filePath(window->gameIdText())).filePath("failed-finalization-source.mkv");
  const QString failed_recovery =
      QDir(QFileInfo(failed_source).absolutePath()).filePath("failed-finalization-source-finalization-failed.mkv");
  const QStringList finalized_before_failure =
      QDir(window->gameDirectoryText())
          .entryList(
              {QString("%1-tracking_output-with-audio*.mp4").arg(window->gameIdText())}, QDir::Files, QDir::Name);
  QFile::remove(failed_source);
  QFile::remove(failed_recovery);
  qputenv("HSTREAM_UI_TEST_ARCHIVE_RESOLVED_PATH", failed_source.toLocal8Bit());
  qputenv("HSTREAM_UI_TEST_FFMPEG_FAIL", "1");
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
  const bool failed_archive_retained = expect(
      window->outputStateText("archive-file") == "ERROR" && finalize_dialog && finalize_dialog->isVisible() &&
          finalize_headline && finalize_headline->text() == "Video finalization failed" &&
          finalize_headline->property("finalizationState").toString() == "failed" && finalize_detail &&
          finalize_detail->text().contains(failed_recovery) && finalize_ok && finalize_ok->isVisible() &&
          finalize_ok->toolTip().contains("Close the finalization result") &&
          finalize_ok->statusTip() == finalize_ok->toolTip() && !QFileInfo::exists(failed_source) &&
          QFileInfo(failed_recovery).size() > 0 && finalized_after_failure == finalized_before_failure,
      "A failed remux must show a red dismissible error, preserve a uniquely named recovery MKV, and publish no MP4");
  if (finalize_ok)
    activate(finalize_ok);

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
  return relative_override_resolved && path_refreshes_with_game && path_visible_before_start && path_prepared &&
      interrupted_archive_preserved && missing_new_output_reported && finalization_visible && archive_deployed &&
      durability_sync_responsive && failed_archive_retained && unsafe_retry_blocked && retry_unblocked_after_recovery;
}

bool test_camera_controls(HStreamWindow* window) {
  if (!expect(window->cameraTabCount() == 4, "Native-effective controls should be grouped by associated stage")) {
    return false;
  }

  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* fixed_edge_link = require_child<QSlider>(window, "cameraSlider_Link_Fixed_Edge_Rotation_Left_Right");
  auto* fixed_edge_left = require_child<QSlider>(window, "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10");
  auto* fixed_edge_right = require_child<QSlider>(window, "cameraSlider_Right_Fixed_Edge_Rotation_Angle_x10");
  auto* stop_delay = require_child<QSlider>(window, "cameraSlider_Stop_Direction_Change_Delay_Frames");
  auto* apply_to_fast = require_child<QSlider>(window, "cameraSlider_Apply_To_Fast_Box");
  auto* max_accel_x = require_child<QSlider>(window, "cameraSlider_Max_Accel_X_x10");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* max_speed_y = require_child<QSlider>(window, "cameraSlider_Max_Speed_Y_x10");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  if (!rotate || !fixed_edge_link || !fixed_edge_left || !fixed_edge_right || !stop_delay || !apply_to_fast ||
      !max_accel_x || !max_speed_x || !max_speed_y || !reset || !save || !create || !game_id || !start || !stop ||
      !mode) {
    return false;
  }

  const QStringList documented_controls = {
      "runModeCombo",
      "controlPointsSpin",
      "renderVideoCheck",
      "startPipelineButton",
      "pausePipelineButton",
      "restartStageButton",
      "savePresetButton",
      "resetCameraButton",
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
      "outputToggle_spare-rtmp",
      "redirectYoutubeButton",
      "addRtspButton",
      "clearLogButton",
      "programFocusButton",
      "stitchedFocusButton",
      "programControlsToggle",
      "stitchedControlsToggle",
      "cameraSlider_Stitch_Rotate_Degrees",
      "cameraSlider_Stop_Direction_Change_Delay_Frames",
      "cameraSlider_Left_Fixed_Edge_Rotation_Angle_x10",
  };
  for (const QString& object_name : documented_controls) {
    QWidget* control = window->findChild<QWidget*>(object_name);
    if (!expect(
            control && control->toolTip().trimmed().size() >= 20 && control->statusTip() == control->toolTip(),
            QString("Interactive control should provide detailed hover help: %1").arg(object_name).toStdString())) {
      return false;
    }
  }

  game_id->setText("ui-camera-control-game");
  activate(create);
  const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
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
  stop_delay->setValue(0);
  if (!expect(!save->isEnabled(), "Reverting a control to its loaded value should disable Save Preset")) {
    return false;
  }

  rotate->setValue(72);
  fixed_edge_left->setValue(250);
  fixed_edge_link->setValue(0);
  fixed_edge_right->setValue(750);
  stop_delay->setValue(14);
  max_speed_x->setValue(450);
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

  activate(save);
  if (!expect(!save->isEnabled(), "A successful preset save should disable Save Preset until another change")) {
    return false;
  }
  stop_delay->setValue(15);
  if (!expect(save->isEnabled(), "A new change after saving should re-enable Save Preset")) {
    return false;
  }
  stop_delay->setValue(14);
  if (!expect(!save->isEnabled(), "Reverting to the saved value should clear the preset dirty state")) {
    return false;
  }
  YAML::Node saved = YAML::LoadFile(config.string());
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
      saved_int("Left_Fixed_Edge_Rotation_Angle_x10", 250) && saved_int("Right_Fixed_Edge_Rotation_Angle_x10", 750);
  YAML::Node saved_rotation;
  const bool has_saved_rotation = lookup_yaml_path(saved, {"stitching", "post_stitch_rotate_degrees"}, &saved_rotation);
  const bool saved_rotation_ok = saved_rotation && saved_rotation.IsScalar() && saved_rotation.as<int>() == 18;
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
      !expect(has_saved_rotation && saved_rotation_ok, "Stitch slider should save the runtime rotation config") ||
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
        !expect(kept_ice_mask_keys, "Saving unchanged stitch rotation should preserve cached ice-mask metadata")) {
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
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 0,
          "Reset should restore the native tracker default")) {
    return false;
  }

  activate(create);
  if (!expect(
          window->cameraControlValue("Stop_Direction_Change_Delay_Frames") == 14,
          "Create/Load should restore saved native controls") ||
      !expect(window->cameraControlValue("Stitch_Rotate_Degrees") == 72, "Create/Load should restore stitch control") ||
      !expect(
          window->cameraControlValue("Link_Fixed_Edge_Rotation_Left_Right") == 0 &&
              window->cameraControlValue("Left_Fixed_Edge_Rotation_Angle_x10") == 250 &&
              window->cameraControlValue("Right_Fixed_Edge_Rotation_Angle_x10") == 750,
          "Create/Load should restore independent fixed-edge rotation angles")) {
    return false;
  }

  YAML::Node relative_runtime_config = YAML::LoadFile(config.string());
  relative_runtime_config["pipeline"]["ds-playtracker"]["config-file"] = ".hstream-ui/play_tracker_config.yaml";
  relative_runtime_config["hstream_ui"]["playtracker_config_base"] = custom_playtracker_config.string();
  {
    std::ofstream out(config);
    out << relative_runtime_config << "\n";
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
  const int rotation_commands_before =
      window->logText().count("stdin:@set-property hmstitcher0 post-stitch-rotate-degrees=");
  for (int value = 60; value <= 69; ++value) {
    rotate->setValue(value);
  }
  for (int i = 0; i < 50 && !window->logText().contains("camera control Stitch_Rotate_Degrees=69 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().count("stdin:@set-property hmstitcher0 post-stitch-rotate-degrees=") ==
              rotation_commands_before + 1,
          "Rapid stitch rotation changes should coalesce into one live pipeline command") ||
      !expect(
          window->logText().contains("stdin:@set-property hmstitcher0 post-stitch-rotate-degrees=21") &&
              window->logText().contains("camera control Stitch_Rotate_Degrees=69 apply=live") &&
              !window->logText().contains("camera control Stitch_Rotate_Degrees=60 apply=pending"),
          "Only the final coalesced stitch rotation should become pending and live")) {
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
      !expect(live_preserved_follower_y_speed, "Live playtracker update should preserve untouched motion limits")) {
    std::cerr << live_playtracker << '\n';
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
          "Reset Camera during playback should restore every changed control on both previously tunable boxes")) {
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
  qunsetenv("HSTREAM_UI_TEST_RUNTIME_CONTROL_TIMEOUT_MS");
  qunsetenv("HSTREAM_UI_TEST_STALL_RUNTIME_CONTROL");
  if (!stalled_controls_bounded) {
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
  return expect(
             has_cleaned_controls && cleaned_controls.IsMap() && cleaned_controls.size() == 0,
             "Saving defaults should clear saved camera controls") &&
      expect(!lookup_yaml_path(cleaned, {"stitching", "post_stitch_rotate_degrees"}, nullptr),
             "Saving defaults should clear UI-generated stitch runtime override") &&
      expect(!lookup_yaml_path(cleaned, {"rink", "camera", "fixed_edge_rotation_angle"}, nullptr),
             "Saving defaults should clear UI-generated fixed-edge rotation override") &&
      expect(restored_custom_playtracker_config, "Saving defaults should restore custom playtracker config override") &&
      expect(preserved_manual_gamma, "Saving defaults should preserve non-UI-authored runtime config") &&
      expect(preserved_manual_left_gamma, "Saving defaults should preserve same-prefix manual color config");
}

bool test_window_close_stops_pipeline(HStreamWindow* window) {
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  if (!start || !mode) {
    return false;
  }
  mode->setCurrentIndex(mode->findData("program"));
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->pipelineStateText() == "PLAYING", "Close-event test pipeline should start")) {
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
  auto* archive = require_child<QCheckBox>(window, "outputToggle_archive-file");
  auto* preview_tabs = require_child<QTabWidget>(window, "previewTabs");
  auto* program_surface = require_child<QWidget>(window, "previewSurface");
  auto* program_render_target = require_child<QWidget>(window, "previewRenderTarget");
  auto* stitched_surface = require_child<QWidget>(window, "stitchedPreviewSurface");
  auto* stitched_render_target = require_child<QWidget>(window, "stitchedPreviewRenderTarget");
  auto* camera1_surface = require_child<QWidget>(window, "camera1PreviewSurface");
  auto* camera1_render_target = require_child<QWidget>(window, "camera1PreviewRenderTarget");
  auto* playback_progress = require_child<QProgressBar>(window, "playbackProgress");
  if (!game_id_edit || !create || !start || !stop || !mode || !control_points || !render_video || !archive ||
      !preview_tabs || !program_surface || !program_render_target || !stitched_surface || !stitched_render_target ||
      !camera1_surface || !camera1_render_target || !playback_progress) {
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

} // namespace

int main(int argc, char** argv) {
  hm::ui_internal::configure_application_identity();
  if (!test_path_scoped_auto_rollback() || !test_diagnostic_capture_attempt_paths()) {
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
  HStreamWindow window;
  window.show();

  if (!test_game_setup(&window, source_root.path())) {
    std::cerr << "test_game_setup failed\n";
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
  if (!test_output_controls(&window)) {
    std::cerr << "test_output_controls failed\n";
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
