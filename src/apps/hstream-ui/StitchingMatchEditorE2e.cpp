// Opt-in real-media/GPU UI test. See docs/stitching-experiments.md.
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/MatchEditorDialog.h"
#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QScreen>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T* widget(QObject& parent, const char* name) {
  auto* result = parent.findChild<T*>(name);
  require(result, std::string("Missing widget ") + name);
  return result;
}
void click(QObject& parent, const char* name) {
  auto* button = widget<QPushButton>(parent, name);
  require(button->isEnabled(), std::string("Disabled button ") + name);
  // Modal exec() belongs to a separate event turn so the driver keeps advancing.
  QTimer::singleShot(0, button, &QPushButton::click);
}
void write(const std::filesystem::path& path, const QString& value) {
  std::ofstream(path) << value.toStdString();
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  install_button_icon_style();
  if (argc != 5) {
    std::cerr << "usage: stitching_match_editor_e2e ISOLATED_GAME_DIRECTORY REPOSITORY ARTIFACT_DIRECTORY RUNNER\n";
    return 2;
  }
  const std::filesystem::path game = std::filesystem::absolute(argv[1]);
  const std::filesystem::path repo = std::filesystem::absolute(argv[2]);
  const std::filesystem::path artifacts = std::filesystem::absolute(argv[3]);
  std::filesystem::create_directories(artifacts);
  if (!std::filesystem::exists(game / ".match-editor-e2e-sandbox")) {
    std::cerr << "Refusing a game without the isolated-test marker\n";
    return 2;
  }
  bool applied = false;
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert("HM_GAME_DIR", QString::fromStdString(game.parent_path().string()));
  env.insert("HM_OUTPUT_WORK_DIR", QString::fromStdString((artifacts / "output").string()));
  StitchingExperimentDialog dialog(
      QString::fromStdString(game.string()),
      QString::fromLocal8Bit(argv[4]),
      QString::fromStdString(repo.string()),
      QString::fromStdString((repo / "configs/ds_hockey_app_config.yaml").string()),
      env,
      100,
      2,
      "00:09:42",
      nullptr,
      [&] { applied = true; });
  dialog.show();
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  auto* log = widget<QPlainTextEdit>(dialog, "stitchExperimentLog");
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames")->setChecked(false);
  widget<QCheckBox>(dialog, "stitchExperimentLoop")->setChecked(false);
  widget<QSpinBox>(dialog, "stitchExperimentPreviewDuration")->setValue(3);
  auto* resolution = widget<QComboBox>(dialog, "stitchExperimentControlPointResolution");
  const int two_k = resolution->findData("2k");
  if (two_k >= 0)
    resolution->setCurrentIndex(two_k);
  hm::stitching::CalibrationMatchSet expected;
  QString prior;
  int step = 0;
  int automatic_row = -1;
  int manual_row = -1;
  QElapsedTimer elapsed;
  elapsed.start();
  QElapsedTimer preview_elapsed;
  QTimer driver;
  driver.setInterval(100);
  QObject::connect(&driver, &QTimer::timeout, &app, [&] {
    try {
      const QString current = status->text();
      if (prior != current) {
        std::cerr << current.toStdString() << '\n';
        prior = current;
      }
      require(elapsed.elapsed() < 1200000, "End-to-end test exceeded 20 minutes");
      write(artifacts / "runner.log", log->toPlainText());
      require(!current.startsWith("Could not persist"), current.toStdString());
      if (step == 0) {
        automatic_row = table->rowCount();
        click(dialog, "addStitchExperimentsToBatchButton");
        ++step;
      } else if (
          step == 1 && table->rowCount() > automatic_row &&
          widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->isEnabled()) {
        click(dialog, "startStitchExperimentBatchButton");
        ++step;
      } else if (step == 2 && widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->isEnabled()) {
        require(
            table->item(automatic_row, 5)->text() == "Ready",
            "Automatic calibration failed: " + table->item(automatic_row, 5)->text().toStdString());
        table->selectRow(automatic_row);
        click(dialog, "inspectStitchExperimentFramesButton");
        ++step;
      } else if (step == 3) {
        auto* viewer = dialog.findChild<QDialog*>("stitchExperimentFrameInspector");
        if (viewer && viewer->isVisible()) {
          click(*viewer, "stitchExperimentEditMatches");
          ++step;
        }
      } else if (step == 4) {
        auto* editor = dynamic_cast<MatchEditorDialog*>(dialog.findChild<QDialog*>("matchEditorDialog"));
        if (editor && editor->isVisible()) {
          editor->showMaximized();
          auto* selection = widget<QComboBox>(*editor, "matchEditorSelection");
          require(selection->count() >= 10, "Expected a useful automatic match set");
          selection->setCurrentIndex(0);
          auto* x = widget<QDoubleSpinBox>(*editor, "matchEditorCoordinate0");
          x->setValue(x->value() + 1.25);
          selection->setCurrentIndex(selection->count() - 1);
          widget<QPushButton>(*editor, "matchEditorDelete")->click();
          widget<QPushButton>(*editor, "matchEditorUndo")->click();
          widget<QPushButton>(*editor, "matchEditorRedo")->click();
          widget<QComboBox>(*editor, "matchEditorPair")->setCurrentIndex(1);
          selection->setCurrentIndex(selection->count() - 1);
          widget<QPushButton>(*editor, "matchEditorDelete")->click();
          expected = editor->editedSet();
          require(expected.frames.size() == 2, "Editor must preserve both camera pairs");
          editor->grab().save(QString::fromStdString((artifacts / "editor.png").string()));
          manual_row = table->rowCount();
          click(*editor, "matchEditorSave");
          ++step;
        }
      } else if (
          step == 5 && table->rowCount() > manual_row &&
          widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->isEnabled()) {
        require(
            table->item(manual_row, 5)->text() == "Ready",
            "Manual calibration failed: " + table->item(manual_row, 5)->text().toStdString());
        require(table->item(automatic_row, 5)->text() == "Ready", "Original candidate changed");
        require(
            log->toPlainText().contains("automatic matching is bypassed"), "Real runner did not reuse manual input");
        table->selectRow(manual_row);
        click(dialog, "previewStitchExperimentButton");
        preview_elapsed.start();
        ++step;
      } else if (step == 6 && log->toPlainText().contains("message=first GPU frame presented")) {
        // Xwayland may refuse framebuffer reads even after EGL has presented.
        // The renderer acknowledgement comes after the real GPU swap, and the
        // successful preview shutdown below verifies its full runner lifecycle.
        auto* video = widget<QWidget>(dialog, "stitchExperimentVideo");
        const QImage image = video->screen()->grabWindow(video->winId()).toImage();
        size_t colored = 0;
        for (int y = 0; y < image.height(); y += 5)
          for (int x = 0; x < image.width(); x += 5) {
            const auto color = image.pixelColor(x, y);
            colored += color.red() > 20 || color.green() > 20 || color.blue() > 20;
          }
        if (colored > 100)
          image.save(QString::fromStdString((artifacts / "preview.png").string()));
        write(artifacts / "preview-ready.txt", "GPU renderer acknowledged its first presented frame.\n");
        if (widget<QPushButton>(dialog, "stopStitchExperimentPreviewButton")->isEnabled())
          click(dialog, "stopStitchExperimentPreviewButton");
        ++step;
      } else if (step == 7 && widget<QPushButton>(dialog, "applyStitchExperimentButton")->isEnabled()) {
        click(dialog, "applyStitchExperimentButton");
        ++step;
      } else if (step == 8 && applied) {
        const auto config = YAML::LoadFile((game / "config.yaml").string());
        const auto fingerprint = hm::stitching::manual_control_point_fingerprint(config);
        require(fingerprint.ok() && !fingerprint->empty(), "Promotion lost manual identity");
        auto saved = hm::stitching::LoadCalibrationMatches(game, *fingerprint);
        require(saved.ok(), saved.status().ToString());
        require(saved->frames.size() == expected.frames.size(), "Promotion lost pairs");
        size_t total = 0;
        for (size_t i = 0; i < expected.frames.size(); ++i) {
          require(
              saved->frames[i].matches.size() == expected.frames[i].matches.size(),
              "Saved edits were recapped/refilled");
          total += saved->frames[i].matches.size();
          for (size_t j = 0; j < expected.frames[i].matches.size(); ++j) {
            require(
                saved->frames[i].matches[j].left == expected.frames[i].matches[j].left &&
                    saved->frames[i].matches[j].right == expected.frames[i].matches[j].right,
                "Edited coordinates changed");
          }
        }
        std::ifstream project(game / "hm_project.pto");
        size_t control_points = 0;
        std::string line;
        while (std::getline(project, line))
          if (line.rfind("c ", 0) == 0)
            ++control_points;
        require(control_points == total, "Real Hugin project did not consume all edited matches");
        auto original = hm::stitching::LoadCalibrationMatches(game, saved->automatic_fingerprint);
        require(original.ok(), "Promoted original is unavailable for reset");
        MatchEditorDialog reopened(*saved, *original);
        reopened.showMaximized();
        require(
            reopened.editedSet().frames.front().matches.front().left == expected.frames.front().matches.front().left,
            "Reopened editor lost endpoint edit");
        std::ofstream(artifacts / "result.txt")
            << "PASS: automatic solve -> UI edit/delete/undo/redo -> manual solve -> GPU preview -> promotion -> reopen\n"
            << "pairs=" << saved->frames.size() << " points=" << total << " Hugin points=" << control_points << '\n';
        std::cerr << "E2E PASS: " << total << " edited points in promoted Hugin project\n";
        driver.stop();
        app.exit(0);
      }
    } catch (const std::exception& error) {
      std::cerr << "E2E step " << step << " failed: " << error.what() << '\n';
      write(artifacts / "failure.txt", QString::fromUtf8(error.what()));
      driver.stop();
      app.exit(1);
    }
  });
  driver.start();
  return app.exec();
}
