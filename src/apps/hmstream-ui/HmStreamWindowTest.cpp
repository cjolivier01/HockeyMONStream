#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
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

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
T* require_child(HmStreamWindow* window, const char* name) {
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
  file.write("hmstream-ui-test-video");
  return true;
}

bool write_fake_runner(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << "Failed to create fake runner: " << path.toStdString() << '\n';
    return false;
  }
  file.write("#!/bin/sh\n");
  file.write("printf '%s\\n' \"$@\"\n");
  file.write("sleep 5\n");
  file.close();
  return QFile::setPermissions(
      path,
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
          QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool test_game_setup(HmStreamWindow* window, const QString& source_dir) {
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
          select_list_item(list, "Center  .hmstream-ui/center/GX010003.MP4"),
          "Explicit Center assignment should remain visible")) {
    return false;
  }
  if (!expect(
          fs::exists(
              fs::path(window->gameDirectoryText().toStdString()) / ".hmstream-ui" / "center" / "GX010003.MP4") &&
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
          "A single explicit Left/Right side should not write a partial runtime video config")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / ".hmstream-ui" / "right" / "GX010002.MP4") &&
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
          yaml["hmstream_ui"]["video_roles"]["center"] &&
              yaml["hmstream_ui"]["video_roles"]["center"][0].as<std::string>() == ".hmstream-ui/center/GX010003.MP4" &&
              !yaml["game"]["videos"]["center"] && text.find("left") != std::string::npos &&
              text.find("GX010005.MP4") != std::string::npos && text.find("right") != std::string::npos &&
              text.find("GX010002.MP4") != std::string::npos && yaml["game"]["videos"]["left"].size() == 1 &&
              yaml["game"]["videos"]["left"][0].as<std::string>() == ".hmstream-ui/left/GX010005.MP4" &&
              yaml["game"]["videos"]["right"].size() == 1 &&
              yaml["game"]["videos"]["right"][0].as<std::string>() == ".hmstream-ui/right/GX010002.MP4" &&
              !yaml["game"]["stitching"]["frame_offsets"] && !yaml["stitching"]["frame_offsets"],
          "Explicit roles should replace stale pipeline config, keep all chapters, and clear stale offsets")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / ".hmstream-ui" / "left" / "GX010005.MP4") &&
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
              matched_explicit["game"]["videos"]["left"][1].as<std::string>() == ".hmstream-ui/left/GX020005.MP4" &&
              matched_explicit["game"]["videos"]["right"][1].as<std::string>() == ".hmstream-ui/right/GX020002.MP4",
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

  if (!select_list_item(list, "Right  .hmstream-ui/right/GX010002.MP4")) {
    return false;
  }
  activate(remove_video);
  std::ifstream updated_input(config);
  const std::string updated_text((std::istreambuf_iterator<char>(updated_input)), std::istreambuf_iterator<char>());
  YAML::Node after_right_remove = YAML::LoadFile(config.string());
  if (!expect(
          updated_text.find("GX010002.MP4") == std::string::npos && !after_right_remove["game"]["videos"]["left"] &&
              !after_right_remove["game"]["videos"]["right"],
          "Removing an explicit role should clear incomplete runtime video config") ||
      !expect(!list_contains(list, "GX010002.MP4"), "Removed explicit imports should not reappear as Auto")) {
    return false;
  }

  YAML::Node generated = YAML::LoadFile(config.string());
  const QString auto_import = window->gameDirectoryText() + "/cam3/GX010001.MP4";
  generated["game"]["videos"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  generated["game"]["videos"]["left"].push_back(auto_import.toStdString());
  generated["game"]["videos"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  generated["game"]["videos"]["right"].push_back("stale-generated-right.mp4");
  generated["hmstream_ui"]["video_roles"]["left"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hmstream_ui"]["video_roles"]["left"].push_back(".hmstream-ui/left/GX010005.MP4");
  generated["hmstream_ui"]["video_roles"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hmstream_ui"]["video_roles"]["right"].push_back(".hmstream-ui/right/GX020002.MP4");
  generated["hmstream_ui"]["video_roles"]["center"] = YAML::Node(YAML::NodeType::Sequence);
  generated["hmstream_ui"]["video_roles"]["center"].push_back(".hmstream-ui/center/GX010003.MP4");
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
  activate(remove_video);
  YAML::Node removed_auto = YAML::LoadFile(config.string());
  if (!expect(
          !removed_auto["game"]["videos"]["left"] && !removed_auto["game"]["videos"]["right"] &&
              !removed_auto["hmstream_ui"]["video_roles"]["left"] &&
              !removed_auto["hmstream_ui"]["video_roles"]["center"] &&
              !removed_auto["hmstream_ui"]["video_roles"]["right"] &&
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
              arbitrary_config["game"]["videos"]["left"][0].as<std::string>() == ".hmstream-ui/left/left-camera.mov" &&
              arbitrary_config["game"]["videos"]["right"][0].as<std::string>() == ".hmstream-ui/right/right-camera.mov",
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
                  ".hmstream-ui/left/left-camera-alt.mov" &&
              arbitrary_multi["game"]["videos"]["right"][1].as<std::string>() ==
                  ".hmstream-ui/right/right-camera-alt.mov",
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
  copied_metadata["hmstream_ui"]["auto_import_sources"].push_back(copied_entry);
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
  fs::create_directories(cleanup_game / ".hmstream-ui" / "left");
  if (!write_fake_video(QString::fromStdString((cleanup_game / "cam1" / "GX010001.MP4").string()))) {
    return false;
  }
  if (!write_fake_video(
          QString::fromStdString((cleanup_game / ".hmstream-ui" / "left" / "copied-left.mp4").string()))) {
    return false;
  }
  YAML::Node cleanup_config;
  cleanup_config["hmstream_ui"]["video_roles"]["left"].push_back(".hmstream-ui/left/copied-left.mp4");
  cleanup_config["hmstream_ui"]["copied_imports"].push_back(".hmstream-ui/left/copied-left.mp4");
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
              !fs::exists(cleanup_game / ".hmstream-ui" / "left" / "copied-left.mp4") &&
              !cleanup_after["hmstream_ui"]["video_roles"]["left"] &&
              cleanup_after["hmstream_ui"]["copied_imports"].size() == 0,
          "Removing Auto should clean copied explicit imports when regular Auto delete is refused")) {
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

bool test_pipeline_buttons(HmStreamWindow* window) {
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* pause = require_child<QPushButton>(window, "pausePipelineButton");
  auto* restart = require_child<QPushButton>(window, "restartStageButton");
  auto* mode = require_child<QComboBox>(window, "runModeCombo");
  auto* control_points = require_child<QSpinBox>(window, "controlPointsSpin");
  if (!stop || !start || !pause || !restart || !mode || !control_points) {
    return false;
  }

  activate(stop);
  if (!expect(window->pipelineStateText() == "STOPPED", "Stop button should stop the pipeline")) {
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
  if (!expect(
          window->logText().contains("ds_hockey_configure_stitching.yaml"),
          "Calibration should use stitching config") ||
      !expect(window->logText().contains("--show-stitching 1"), "Calibration should request stitched display") ||
      !expect(window->pipelineStateText() == "PLAYING", "Test runner should keep calibration process running")) {
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

  mode->setCurrentIndex(mode->findData("program"));
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("ds_hockey_app_config.yaml"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  if (!expect(window->logText().contains("--show"), "Program run should request render output")) {
    return false;
  }
  activate(stop);

  activate(restart);
  const bool restart_logged =
      expect(window->logText().contains("stage restart requested"), "Restart button should log a stage restart");
  activate(stop);
  if (!restart_logged) {
    return false;
  }

  const QByteArray original_runner = qgetenv("HMSTREAM_UI_TEST_RUNNER");
  qputenv("HMSTREAM_UI_TEST_RUNNER", "/tmp/hmstream-ui-missing-runner");
  activate(start);
  for (int i = 0; i < 50 && !window->logText().contains("pipeline process error"); ++i) {
    QApplication::processEvents();
    QTest::qWait(10);
  }
  qputenv("HMSTREAM_UI_TEST_RUNNER", original_runner);
  return expect(window->pipelineStateText() == "STOPPED", "Failed runner should restore stopped state") &&
      expect(window->logText().contains("pipeline process error"), "Failed runner should log process error");
}

bool test_output_controls(HmStreamWindow* window) {
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

bool test_camera_controls(HmStreamWindow* window) {
  if (!expect(window->cameraTabCount() >= 6, "Camera controls should be grouped on tabs")) {
    return false;
  }

  auto* exposure = require_child<QSlider>(window, "cameraSlider_Exposure_EV_x10");
  auto* exposure_value = require_child<QLabel>(window, "cameraValue_Exposure_EV_x10");
  auto* rotate = require_child<QSlider>(window, "cameraSlider_Stitch_Rotate_Degrees");
  auto* left_brightness = require_child<QSlider>(window, "cameraSlider_Left_Brightness_Multiplier_x100");
  auto* left_gamma = require_child<QSlider>(window, "cameraSlider_Left_Gamma_Multiplier_x100");
  auto* max_speed_x = require_child<QSlider>(window, "cameraSlider_Max_Speed_X_x10");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  if (!exposure || !exposure_value || !rotate || !left_brightness || !left_gamma || !max_speed_x || !reset || !save ||
      !create || !game_id) {
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

  activate(save);
  const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  YAML::Node saved = YAML::LoadFile(config.string());
  auto saved_int = [&](const char* key, int expected) {
    YAML::Node value;
    if (!lookup_yaml_path(saved, {"hmstream_ui", "camera_controls", key}, &value)) {
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
      lookup_yaml_path(saved, {"hmstream_ui", "camera_controls", "Apply_To_Follower_Box"}, nullptr);
  if (!saved_controls_ok) {
    std::cerr << saved << '\n';
  }
  if (!expect(window->logText().contains("preset saved"), "Save preset button should log persistence") ||
      !expect(saved_controls_ok, "Save preset should persist non-default control values") ||
      !expect(!has_default_follower, "Save preset should omit default control values") ||
      !expect(has_saved_rotation && saved_rotation_ok, "Stitch slider should save the runtime rotation config") ||
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
      lookup_yaml_path(same_prefix, {"hmstream_ui", "playtracker_config_base"}, &saved_playtracker_base) &&
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
  const bool has_cleaned_controls = lookup_yaml_path(cleaned, {"hmstream_ui", "camera_controls"}, &cleaned_controls);
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

bool run_real_pipeline_e2e(HmStreamWindow* window, const QString& game_id) {
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

  const QString run_mode = QString::fromLocal8Bit(qgetenv("HMSTREAM_UI_E2E_RUN_MODE"));
  if (!run_mode.isEmpty()) {
    const int mode_index = mode->findData(run_mode);
    if (mode_index < 0) {
      std::cerr << "Missing e2e run mode: " << run_mode.toStdString() << '\n';
      return false;
    }
    mode->setCurrentIndex(mode_index);
  }
  const int configured_control_points = qEnvironmentVariableIntValue("HMSTREAM_UI_E2E_CONTROL_POINTS");
  if (configured_control_points > 0) {
    control_points->setValue(configured_control_points);
  }
  activate(start);

  const int timeout_ms = qEnvironmentVariableIntValue("HMSTREAM_UI_E2E_TIMEOUT_MS");
  const int deadline_ms = timeout_ms > 0 ? timeout_ms : 120000;
  QElapsedTimer timer;
  timer.start();
  bool observed_running = false;
  while (timer.elapsed() < deadline_ms) {
    QApplication::processEvents();
    QTest::qWait(100);
    const QString log = window->logText();
    if (log.contains("asset setup failed") || log.contains("pipeline process error")) {
      std::cerr << log.toStdString() << '\n';
      activate(stop);
      return false;
    }
    if (log.contains("** INFO: <bus_callback:314>: Pipeline running") || log.contains("**PERF:")) {
      observed_running = true;
      break;
    }
  }

  const QString log = window->logText();
  const bool observed_asset_python = log.contains("using asset setup python");
  const bool observed_command = log.contains("pipeline command");
  activate(stop);
  for (int i = 0; i < 300 && window->pipelineStateText() != "STOPPED"; ++i) {
    QApplication::processEvents();
    QTest::qWait(100);
  }

  if (!expect(observed_asset_python, "Real UI run should execute pretrained asset setup probe") ||
      !expect(observed_command, "Real UI run should launch hmstream-cli") ||
      !expect(observed_running, "Real UI run should reach GStreamer PLAYING/PERF output")) {
    std::cerr << log.toStdString() << '\n';
    return false;
  }
  return expect(window->pipelineStateText() == "STOPPED", "Real UI run should stop cleanly after e2e smoke");
}

} // namespace

int main(int argc, char** argv) {
  const QByteArray e2e_game_id = qgetenv("HMSTREAM_UI_E2E_GAME_ID");
  if (!e2e_game_id.isEmpty()) {
    QApplication app(argc, argv);
    HmStreamWindow window;
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
  const QString fake_runner = source_root.path() + "/hmstream-ui-fake-runner.sh";
  if (!write_fake_runner(fake_runner)) {
    return 1;
  }
  qputenv("HMSTREAM_UI_TEST_RUNNER", fake_runner.toLocal8Bit());

  QApplication app(argc, argv);
  HmStreamWindow window;
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
  return 0;
}
