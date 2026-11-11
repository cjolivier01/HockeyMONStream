#include "src/apps/hstream-ui/CameraChapterWarning.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    QTemporaryDir temporary;
    const std::filesystem::path game(temporary.path().toStdString());
    std::filesystem::create_directories(game / "_hockeymon");
    const auto save = [&](const std::string& right) {
      std::ofstream(game / "_hockeymon/config.yaml")
          << "hstream_ui:\n  video_roles:\n    left: [GX010001.MP4, GX020001.MP4, GX030001.MP4]\n    right: [" << right
          << "]\n";
    };
    save("GH010002.MP4, GH030002.MP4");
    const auto warning = CameraChapterWarning(game);
    Check(
        warning.contains("right") && warning.contains("chapter 2"), "Explicit inputs must report the missing chapter");
    bool saw_prompt = false;
    const auto answer = [&](bool proceed) {
      QTimer::singleShot(0, [&] {
        auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        Check(prompt && prompt->objectName() == "cameraChapterWarning", "Expected chapter confirmation");
        saw_prompt = true;
        if (argc == 2)
          Check(prompt->grab().save(QString::fromLocal8Bit(argv[1])), "Cannot capture chapter confirmation");
        Check(prompt->defaultButton()->text() == "Cancel", "Cancel must be the default");
        for (auto* button : prompt->buttons()) {
          Check(!button->icon().isNull(), "Confirmation actions need icons");
          if (button->text() == (proceed ? "Continue anyway" : "Cancel")) {
            button->click();
            return;
          }
        }
        throw std::runtime_error("Missing confirmation action");
      });
      return ConfirmCameraChapterWarning(nullptr, warning);
    };
    Check(!answer(false) && saw_prompt, "Cancel must stop admission");
    Check(answer(true), "Continue must permit admission");
    save("GH020002.MP4, GH030002.MP4");
    Check(CameraChapterWarning(game).contains("chapter 1"), "Missing first chapters must warn");
    save("GH010002.MP4, GH020002.MP4");
    Check(CameraChapterWarning(game).isEmpty(), "Trailing differences must pass silently");
    Check(ConfirmCameraChapterWarning(nullptr, {}), "Complete inputs must not prompt");
    // An irrelevant Auto camera must not override the explicit pair.
    std::filesystem::create_directories(game / "cam3");
    std::ofstream(game / "cam3/GX030003.MP4").put('x');
    Check(CameraChapterWarning(game).isEmpty(), "Unselected Auto inputs must be ignored");
    std::filesystem::remove(game / "_hockeymon/config.yaml");
    std::filesystem::create_directories(game / "cam4");
    std::ofstream(game / "cam4/GX010004.MP4").put('x');
    std::ofstream(game / "cam4/GX030004.MP4").put('x');
    Check(CameraChapterWarning(game).contains("chapter 1"), "Fresh Auto inputs must warn");
    std::ofstream(game / "_hockeymon/config.yaml")
        << "game:\n  videos: {left: [left-1.mp4, left-2.mp4, left-3.mp4], right: [right-1.mp4, right-3.mp4]}\n";
    Check(CameraChapterWarning(game).contains("chapter 1"), "Auto must supersede stale saved playlists");
    std::filesystem::remove_all(game / "cam3");
    std::filesystem::remove_all(game / "cam4");
    std::ofstream(game / "left.mp4").put('x');
    std::ofstream(game / "right.mp4").put('x');
    Check(
        CameraChapterWarning(game).contains("chapter 2"),
        "Saved playlists remain authoritative beside root Left/Right files");
    std::cout << "Camera chapter confirmation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
