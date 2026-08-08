#include "src/apps/hstream-ui/HStreamWindow.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTextEdit>

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
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
  file.write("print('LD_LIBRARY_PATH=' + os.environ.get('LD_LIBRARY_PATH', ''), flush=True)\n");
  file.write("if '--clean' in sys.argv[1:]:\n");
  file.write("    print('clean runner exiting', flush=True)\n");
  file.write("    sys.exit(0)\n");
  file.write("sys.stdout.write('\\033[34mANSI')\n");
  file.write("sys.stdout.flush()\n");
  file.write("time.sleep(0.05)\n");
  file.write("sys.stdout.write(' blue runner line\\033[0m\\n')\n");
  file.write("sys.stdout.flush()\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_COMPLETE_CALIBRATION') == '1':\n");
  file.write("    print('hmstitcher: one-pass stitching configuration complete', flush=True)\n");
  file.write("if os.environ.get('HSTREAM_UI_TEST_CLOSE_STDIN') == '1':\n");
  file.write("    sys.stdin.close()\n");
  file.write("    time.sleep(5.0)\n");
  file.write("    sys.exit(0)\n");
  file.write("deadline = time.monotonic() + 5.0\n");
  file.write("while time.monotonic() < deadline:\n");
  file.write("    readable, _, _ = select.select([sys.stdin], [], [], 0.05)\n");
  file.write("    if not readable:\n");
  file.write("        continue\n");
  file.write("    line = sys.stdin.readline()\n");
  file.write("    if line == '':\n");
  file.write("        break\n");
  file.write("    print('stdin:' + line.rstrip('\\n'), flush=True)\n");
  file.write("    if line.startswith('@set-property '):\n");
  file.write("        _, element, assignment = line.rstrip('\\n').split(' ', 2)\n");
  file.write("        property_name, runtime_value = assignment.split('=', 1)\n");
  file.write("        if os.environ.get('HSTREAM_UI_TEST_REJECT_RUNTIME_CONTROL') == '1':\n");
  file.write(
      "            print('runtime command failed: plugin rejected ' + element + '.' + property_name + '=' + "
      "runtime_value, file=sys.stderr, flush=True)\n");
  file.write("        else:\n");
  file.write(
      "            print('runtime property ' + element + ' ' + property_name + '=' + runtime_value, flush=True)\n");
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
  const QString right_video = source_dir + "/GX010002.MP4";
  const QString right_video_2 = source_dir + "/GX020002.MP4";
  if (!write_fake_video(auto_video) || !write_fake_video(suffix_auto_video) || !write_fake_video(center_video) ||
      !write_fake_video(left_video) || !write_fake_video(left_video_2) || !write_fake_video(right_video) ||
      !write_fake_video(right_video_2) || !write_fake_video(left_video_3)) {
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
  YAML::Node mismatched_explicit = YAML::LoadFile(config.string());
  if (!expect(
          !mismatched_explicit["game"]["videos"]["left"] && !mismatched_explicit["game"]["videos"]["right"],
          "Mismatched explicit Left/Right chapter counts should not write runtime video config")) {
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
          !mismatched_chapters["game"]["videos"]["left"] && !mismatched_chapters["game"]["videos"]["right"],
          "Mismatched explicit Left/Right chapter sets should clear runtime video config")) {
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
              updated_text.find("GX010002.MP4") == std::string::npos && !after_right_remove["game"]["videos"]["left"] &&
              !after_right_remove["game"]["videos"]["right"],
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
          !arbitrary_mismatched["game"]["videos"]["left"] && !arbitrary_mismatched["game"]["videos"]["right"],
          "Mismatched arbitrary explicit counts should clear runtime video config")) {
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
  auto* main_log_splitter = require_child<QSplitter>(window, "mainLogSplitter");
  auto* program_host = require_child<QWidget>(window, "programLetterboxHost");
  auto* preview_surface = require_child<QWidget>(window, "previewSurface");
  auto* stitched_surface = require_child<QWidget>(window, "stitchedPreviewSurface");
  auto* external_notice = require_child<QLabel>(window, "programExternalRenderNotice");
  auto* stitched_status = require_child<QLabel>(window, "stitchedPreviewStatusLabel");
  if (!stop || !start || !pause || !restart || !mode || !control_points || !game_id || !rotate || !max_speed_x ||
      !render_video || !log || !main_log_splitter || !program_host || !preview_surface || !stitched_surface ||
      !external_notice || !stitched_status) {
    return false;
  }

  if (!expect(
          main_log_splitter->orientation() == Qt::Vertical && main_log_splitter->count() == 2,
          "Main content and runtime log should be separated by a draggable vertical splitter")) {
    return false;
  }
  if (!expect(
          hm::ui_internal::supports_x11_embedding("xcb") && !hm::ui_internal::supports_x11_embedding("wayland") &&
              !hm::ui_internal::supports_x11_embedding("offscreen"),
          "Native preview embedding should only accept Qt XCB window handles")) {
    return false;
  }

  activate(stop);
  if (!expect(window->pipelineStateText() == "STOPPED", "Stop button should stop the pipeline")) {
    return false;
  }
  if (!expect(control_points->value() == 1500, "Stitching calibration CP default should be 1500")) {
    return false;
  }

  mode->setCurrentIndex(mode->findData("program"));
  const int fresh_program_clean_commands = window->logText().count("stitching calibration clean command");
  qputenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION", "1");
  activate(start);
  for (int i = 0; i < 200 &&
       (!window->logText().contains("one-pass stitching calibration complete; continuous program playback running") ||
        window->pipelineStateText() != "PLAYING");
       ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const fs::path fresh_program_config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  const YAML::Node fresh_program_saved = YAML::LoadFile(fresh_program_config.string());
  YAML::Node fresh_program_status;
  const bool has_fresh_program_status =
      lookup_yaml_path(fresh_program_saved, {"hstream_ui", "stitching_calibration", "status"}, &fresh_program_status);
  const bool fresh_program_tracked =
      expect(
          window->logText().count("stitching calibration clean command") == fresh_program_clean_commands + 1,
          "A fresh Program run should establish tracked one-pass stitching calibration") &&
      expect(
          has_fresh_program_status && fresh_program_status.IsScalar() &&
              fresh_program_status.as<std::string>() == "complete",
          "A fresh Program one-pass calibration should persist completed state");
  activate(stop);
  qunsetenv("HSTREAM_UI_TEST_COMPLETE_CALIBRATION");
  if (!fresh_program_tracked) {
    return false;
  }

  const int calibration_index = mode->findData("stitch-calibration");
  mode->setCurrentIndex(calibration_index);
  control_points->setValue(750);
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  for (int i = 0; i < 50 && !window->logText().contains("HM_MAX_CONTROL_POINTS=750"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(
          window->logText().contains("ds_hockey_app_config.yaml"),
          "Calibration should use the one-pass application config") ||
      !expect(window->logText().contains("--clean"), "Changed calibration CP count should clean stitching artifacts") ||
      !expect(
          window->logText().contains("stitching calibration control points changed 1500 -> 750"),
          "Calibration CP change should be logged") ||
      !expect(
          window->logText().contains("--enable-sinks=RENDER"),
          "One-pass calibration should render the stitched output") ||
      !expect(window->logText().contains("--show-stitching 1"), "Calibration should show stitcher output") ||
      !expect(
          window->logText().contains("HM_MAX_CONTROL_POINTS=750"),
          "One-pass calibration should pass the selected control-point limit") ||
      !expect(
          !window->logText().contains("--render-window-id=") && window->logText().contains("HM_RENDER_SINK=nv3dsink") &&
              stitched_surface->isHidden(),
          "The offscreen test backend should fall back to a separate render window instead of passing a non-X11 "
          "handle") ||
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
    if (!expect(
            has_saved_control_points && saved_control_points.IsScalar() && saved_control_points.as<int>() == 750,
            "Calibration CP count should be saved to private config") ||
        !expect(
            has_saved_status && saved_status.IsScalar() && saved_status.as<std::string>() == "pending",
            "Calibration CP state should remain pending while the calibration process is running")) {
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
            "User-stopped calibration should remain pending so the next run cleans again")) {
      return false;
    }
  }
  {
    const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
    const QString launched_game_id = game_id->text();
    const QString switched_game_id = "ui-switched-during-calibration";
    const fs::path switched_config =
        fs::path(qgetenv("HM_GAME_DIR").toStdString()) / switched_game_id.toStdString() / "config.yaml";
    const fs::path active_runtime_config = config.parent_path() / ".hstream-ui" / "play_tracker_config.yaml";
    const fs::path switched_runtime_config = switched_config.parent_path() / ".hstream-ui" / "play_tracker_config.yaml";
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
    const int original_max_speed_x = max_speed_x->value();
    max_speed_x->setValue(original_max_speed_x + 1);
    for (int i = 0; i < 50 &&
         !window->logText().contains(
             QString("camera control Max_Speed_X_x10=%1 apply=live").arg(original_max_speed_x + 1));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    const bool runtime_control_used_launched_game =
        fs::exists(active_runtime_config) && !fs::exists(switched_runtime_config);
    game_id->setText(launched_game_id);
    max_speed_x->setValue(original_max_speed_x);
    for (int i = 0; i < 50 &&
         !window->logText().contains(QString("camera control Max_Speed_X_x10=%1 apply=live").arg(original_max_speed_x));
         ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    if (!expect(
            window->logText().contains("hmstitcher: one-pass stitching configuration complete"),
            "Successful one-pass calibration should publish its completion marker") ||
        !expect(
            window->logText().count("pipeline command ") == pipeline_commands_before + 1,
            "Calibration and continuous stitched preview should use one application process") ||
        !expect(window->logText().contains("--show-stitching 1"), "Continuous preview should show stitcher output") ||
        !expect(window->pipelineStateText() == "PLAYING", "Continuous stitched preview should remain running") ||
        !expect(
            has_transitioned_status && transitioned_status.IsScalar() &&
                transitioned_status.as<std::string>() == "complete",
            "Calibration should be marked complete while continuous preview keeps running") ||
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
            "Rejected runtime controls should not be reported as live")) {
      return false;
    }

    qputenv("HSTREAM_UI_TEST_CLOSE_STDIN", "1");
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
    activate(stop);
    for (int i = 0; i < 50 && window->pipelineStateText() != "STOPPED"; ++i) {
      QApplication::processEvents();
      QTest::qWait(10);
    }
    qunsetenv("HSTREAM_UI_TEST_CLOSE_STDIN");
    if (!expect(write_error_kept_running, "A runtime-control write error should not mark live playback stopped")) {
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
  const int embedded_commands_before_external_run = window->logText().count("--render-window-id=");
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("HM_RENDER_SINK=nv3dsink"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->logText().contains("--show"), "Program run should request render output") ||
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
          window->logText().contains("HM_NO_SCOREBOARD=1"),
          "Program playback should not block its output thread on the interactive scoreboard selector") ||
      !expect(
          window->logText().contains("separate DeepStream window"),
          "UI must surface self-managed render-window mode") ||
      !expect(
          external_notice->parentWidget() == program_host && preview_surface->isHidden(),
          "External-render notice should use the resizable Qt host instead of the hidden native render surface")) {
    return false;
  }
  window->resize(window->width(), 1200);
  main_log_splitter->setSizes({800, 350});
  QApplication::processEvents();
  QTest::qWait(10);
  if (!expect(
          external_notice->geometry() == program_host->rect(),
          "External-render notice should resize and move with its preview tab when the log splitter moves")) {
    return false;
  }
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
          "UI runner should preserve an explicit embeddable render sink");
  activate(stop);
  qunsetenv("HM_RENDER_SINK");
  if (!explicit_embedding_preserved) {
    return false;
  }

  const int fake_sink_commands_before = window->logText().count("--enable-sinks=FAKE");
  const int embedded_commands_before = window->logText().count("--render-window-id=");
  render_video->setChecked(false);
  activate(start);
  for (int i = 0; i < 50 && window->pipelineStateText() != "PLAYING"; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const bool rendering_disabled = expect(
                                      window->logText().count("--enable-sinks=FAKE") == fake_sink_commands_before + 1,
                                      "Disabling video rendering should use a fake sink when no output is selected") &&
      expect(window->logText().count("--render-window-id=") == embedded_commands_before,
             "Disabling video rendering should not attach a native preview window") &&
      expect(external_notice->text() == "Video rendering is disabled for this run",
             "The active preview tab should explain that rendering is disabled");
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
  if (!restart_logged) {
    return false;
  }

  const QByteArray original_runner = qgetenv("HSTREAM_UI_TEST_RUNNER");
  qputenv("HSTREAM_UI_TEST_RUNNER", "/tmp/hstream-ui-missing-runner");
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("pipeline process error"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HSTREAM_UI_TEST_RUNNER", original_runner);
  return expect(window->pipelineStateText() == "STOPPED", "Failed runner should restore stopped state") &&
      expect(window->logText().contains("pipeline process error"), "Failed runner should log process error");
}

bool test_output_controls(HStreamWindow* window) {
  auto* spare = require_child<QCheckBox>(window, "outputToggle_spare-rtmp");
  auto* youtube_redirect = require_child<QPushButton>(window, "redirectYoutubeButton");
  auto* add_rtsp = require_child<QPushButton>(window, "addRtspButton");
  if (!spare || !youtube_redirect || !add_rtsp) {
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
  return expect(
      window->outputStateText("rtsp-dynamic-1") == "ENABLED", "Add RTSP button should create an enabled RTSP output");
}

bool test_camera_controls(HStreamWindow* window) {
  if (!expect(window->cameraTabCount() >= 6, "Camera controls should be grouped on tabs")) {
    return false;
  }

  auto* exposure = require_child<QSlider>(window, "cameraSlider_Exposure_EV_x10");
  auto* exposure_value = require_child<QLabel>(window, "cameraValue_Exposure_EV_x10");
  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* left_brightness = require_child<QSlider>(window, "cameraSlider_Left_Brightness_Multiplier_x100");
  auto* left_gamma = require_child<QSlider>(window, "cameraSlider_Left_Gamma_Multiplier_x100");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* max_speed_y = require_child<QSlider>(window, "cameraSlider_Max_Speed_Y_x10");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  if (!exposure || !exposure_value || !rotate || !left_brightness || !left_gamma || !max_speed_x || !max_speed_y ||
      !reset || !save || !create || !game_id || !start || !stop || !mode) {
    return false;
  }

  game_id->setText("ui-camera-control-game");
  activate(create);

  exposure->setValue(47);
  rotate->setValue(72);
  left_gamma->setValue(125);
  max_speed_x->setValue(450);
  if (!expect(window->cameraControlValue("Exposure_EV_x10") == 47, "Exposure slider should update controller state") ||
      !expect(
          window->cameraControlValue("Stitch_Rotate_Degrees") == 72,
          "Stitch rotation slider should update controller state") ||
      !expect(
          window->cameraControlValue("Left_Gamma_Multiplier_x100") == 125,
          "Side color slider should update controller state") ||
      !expect(window->cameraControlValue("Max_Speed_X_x10") == 450, "Speed slider should update controller state")) {
    return false;
  }

  const int gamma_before_wheel = left_gamma->value();
  QWheelEvent wheel_event(
      left_gamma->rect().center(),
      left_gamma->mapToGlobal(left_gamma->rect().center()),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::ScrollUpdate,
      false);
  QApplication::sendEvent(left_gamma, &wheel_event);
  QApplication::processEvents();
  if (!expect(
          left_gamma->value() == gamma_before_wheel,
          "Mouse wheel over camera slider should not change live camera control")) {
    return false;
  }

  const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
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
  const bool saved_controls_ok = saved_int("Exposure_EV_x10", 47) && saved_int("Stitch_Rotate_Degrees", 72) &&
      saved_int("Left_Gamma_Multiplier_x100", 125);
  YAML::Node saved_rotation;
  const bool has_saved_rotation = lookup_yaml_path(saved, {"stitching", "post_stitch_rotate_degrees"}, &saved_rotation);
  const bool saved_rotation_ok = saved_rotation && saved_rotation.IsScalar() && saved_rotation.as<int>() == 18;
  YAML::Node saved_max_speed_x;
  const bool has_saved_max_speed_x =
      lookup_yaml_path(saved, {"rink", "camera", "max_speed_ratio_x"}, &saved_max_speed_x);
  const bool saved_max_speed_x_ok =
      has_saved_max_speed_x && saved_max_speed_x.IsScalar() && saved_max_speed_x.as<double>() == 1.5;
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
      !expect(removed_rink_mask, "Saving stitch rotation should remove stale rink mask image") ||
      !expect(removed_scoreboard_polygon, "Saving stitch rotation should invalidate scoreboard perspective") ||
      !expect(removed_ice_mask_keys, "Saving stitch rotation should invalidate cached ice-mask metadata") ||
      !expect(has_saved_max_speed_x && saved_max_speed_x_ok, "Speed slider should save runtime ratio config") ||
      !expect(
          has_saved_playtracker_config_path && saved_follower_max_speed_x && !saved_follower_max_speed_y &&
              !saved_follower_max_accel_x && !saved_follower_max_accel_y && !saved_fast_max_speed_x,
          "Speed slider should save only changed follower playtracker runtime config")) {
    if (!has_saved_rotation || !saved_rotation_ok || !has_saved_max_speed_x || !saved_max_speed_x_ok ||
        !saved_follower_max_speed_x || saved_follower_max_speed_y || saved_follower_max_accel_x ||
        saved_follower_max_accel_y || saved_fast_max_speed_x) {
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
    saved = after_same_rotation_save;
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
  if (!expect(window->cameraControlValue("Exposure_EV_x10") == 40, "Reset should restore exposure default")) {
    return false;
  }

  activate(create);
  if (!expect(window->cameraControlValue("Exposure_EV_x10") == 47, "Create/Load should restore saved controls") ||
      !expect(window->cameraControlValue("Stitch_Rotate_Degrees") == 72, "Create/Load should restore stitch control") ||
      !expect(exposure_value->text() == "47", "Create/Load should refresh visible camera value labels")) {
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
  max_speed_x->setValue(460);
  for (int i = 0; i < 50 && !window->logText().contains("camera control Max_Speed_X_x10=460 apply=live"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  const fs::path live_playtracker_config =
      fs::path(window->gameDirectoryText().toStdString()) / ".hstream-ui" / "play_tracker_config.yaml";
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
          window->logText().contains("stdin:@set-property dsplaytracker0 config-file="),
          "Live speed slider should send playtracker config-file update to the running pipeline") ||
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
  max_speed_y->setValue(300);
  for (int i = 0; i < 50; ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
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

  left_gamma->setValue(100);
  left_brightness->setValue(110);
  activate(save);
  YAML::Node same_prefix = YAML::LoadFile(config.string());
  YAML::Node same_prefix_left_gamma;
  YAML::Node same_prefix_left_brightness;
  const bool preserved_left_gamma =
      lookup_yaml_path(same_prefix, {"stitching", "left", "color", "gamma"}, &same_prefix_left_gamma) &&
      same_prefix_left_gamma.IsScalar() && same_prefix_left_gamma.as<double>() == 1.75;
  const bool saved_left_brightness =
      lookup_yaml_path(same_prefix, {"stitching", "left", "color", "brightness"}, &same_prefix_left_brightness) &&
      same_prefix_left_brightness.IsScalar() && same_prefix_left_brightness.as<double>() == 1.1;
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
  if (!expect(preserved_left_gamma, "Saving one color leaf should preserve manual same-prefix color edits") ||
      !expect(saved_left_brightness, "Saving one color leaf should persist that leaf") ||
      !expect(preserved_custom_tracker_config, "Generated playtracker config should preserve custom base config") ||
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
      expect(restored_custom_playtracker_config, "Saving defaults should restore custom playtracker config override") &&
      expect(!lookup_yaml_path(cleaned, {"stitching", "left", "color", "brightness"}, nullptr),
             "Saving defaults should clear UI-generated side color leaf") &&
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

bool run_real_pipeline_e2e(HStreamWindow* window, const QString& game_id) {
  auto* game_id_edit = require_child<QLineEdit>(window, "gameIdEdit");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* control_points = require_child<QSpinBox>(window, "controlPointsSpin");
  if (!game_id_edit || !create || !start || !stop || !mode || !control_points) {
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
  activate(start);

  const int timeout_ms = qEnvironmentVariableIntValue("HSTREAM_UI_E2E_TIMEOUT_MS");
  const int deadline_ms = timeout_ms > 0 ? timeout_ms : 120000;
  QElapsedTimer timer;
  timer.start();
  bool observed_first_frame = false;
  const QRegularExpression positive_fps(R"(\*\*PERF:\s+([0-9]+(?:\.[0-9]+)?))");
  while (timer.elapsed() < deadline_ms) {
    QApplication::processEvents();
    QTest::qWait(100);
    const QString log = window->logText();
    if (log.contains("asset setup failed") || log.contains("pipeline process error")) {
      std::cerr << log.toStdString() << '\n';
      activate(stop);
      return false;
    }
    auto match = positive_fps.globalMatch(log);
    while (match.hasNext()) {
      bool parsed = false;
      const double fps = match.next().captured(1).toDouble(&parsed);
      if (parsed && fps > 0.0) {
        observed_first_frame = true;
        break;
      }
    }
    if (observed_first_frame) {
      break;
    }
  }

  const QString log = window->logText();
  const bool observed_native_asset_setup = log.contains("pretrained assets will be verified by hstream-cli");
  const bool observed_command = log.contains("pipeline command");
  activate(stop);
  for (int i = 0; i < 300 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(100);
  }

  if (!expect(observed_native_asset_setup, "Real UI run should delegate native asset verification to hstream-cli") ||
      !expect(observed_command, "Real UI run should launch hstream-cli") ||
      !expect(observed_first_frame, "Real UI run should process frames at positive FPS")) {
    std::cerr << log.toStdString() << '\n';
    return false;
  }
  return expect(window->pipelineStateText() == "STOPPED", "Real UI run should stop cleanly after e2e smoke");
}

} // namespace

int main(int argc, char** argv) {
  if (!test_path_scoped_auto_rollback()) {
    return 1;
  }
  const QByteArray e2e_game_id = qgetenv("HSTREAM_UI_E2E_GAME_ID");
  if (!e2e_game_id.isEmpty()) {
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
  if (!write_fake_runner(fake_runner)) {
    return 1;
  }
  qputenv("HSTREAM_UI_TEST_RUNNER", fake_runner.toLocal8Bit());

  QApplication app(argc, argv);
  HStreamWindow window;
  window.show();

  if (!test_game_setup(&window, source_root.path())) {
    std::cerr << "test_game_setup failed\n";
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
