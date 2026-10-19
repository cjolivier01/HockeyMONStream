#include "src/apps/hstream-ui/RinkMapDialog.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"

#include <tiffio.h>
#include <unistd.h>
#include <clocale>
#include <cmath>
#include <fstream>
#include <iostream>

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>

namespace {
namespace rink = hm::stitching;
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
bool write(const QString& path, const QByteArray& text) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
}
bool placement(const QString& path, float left) {
  TIFF* file = TIFFOpen(path.toStdString().c_str(), "w");
  if (!file)
    return false;
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 900);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 1200);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
  TIFFSetField(file, TIFFTAG_XRESOLUTION, 1.0);
  TIFFSetField(file, TIFFTAG_YRESOLUTION, 1.0);
  TIFFSetField(file, TIFFTAG_XPOSITION, left);
  TIFFSetField(file, TIFFTAG_YPOSITION, 300.0);
  std::vector<unsigned char> row(900);
  bool ok = true;
  for (uint32_t y = 0; y < 1200 && ok; ++y)
    ok = TIFFWriteScanline(file, row.data(), y, 0) >= 0;
  TIFFClose(file);
  return ok;
}
bool waitUntil(const std::function<bool()>& ready, int timeout = 30000) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < timeout)
    QTest::qWait(10);
  return ready();
}
bool idle(RinkMapDialog& dialog, int timeout = 30000) {
  return waitUntil([&] { return !dialog.findChild<QPushButton*>("stopRinkMapButton")->isEnabled(); }, timeout);
}
bool mark(RinkMapDialog& dialog, int camera, const QString& feature, QPointF point) {
  auto* combo = dialog.findChild<QComboBox*>("rinkMapFeatureCombo");
  combo->setCurrentIndex(combo->findData(feature));
  dialog.findChild<QTabWidget*>("rinkMapCameraTabs")->setCurrentIndex(camera);
  auto* view = dialog.findChild<QGraphicsView*>(QString("rinkMapCamera%1").arg(camera));
  QTest::qWait(20);
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->mapFromScene(point));
  return idle(dialog);
}
cv::Point2d sourcePoint(cv::Point2d rink_point) {
  cv::Vec3d ray(rink_point.x - 3, rink_point.y + 19, -7);
  ray /= cv::norm(ray);
  constexpr double pi = 3.14159265358979323846;
  return {1799.5 - std::atan2(ray[1], ray[0]) * 1800 / pi, 899.5 - std::asin(ray[2]) * 1800 / pi};
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (argc == 4 && QString(argv[1]) == "--inspect-saved") {
    RinkMapDialog dialog(QString::fromLocal8Bit(argv[2]));
    dialog.show();
    if (!idle(dialog))
      return 1;
    const QString prefix = QString::fromLocal8Bit(argv[3]);
    if (!dialog.grab().save(prefix + "-ready.png"))
      return 1;
    auto* table = dialog.findChild<QTableWidget*>("rinkMapObservationsTable");
    for (int row = 1; row < table->rowCount(); ++row) {
      table->item(row, 0)->setCheckState(Qt::Unchecked);
      if (!idle(dialog))
        return 1;
    }
    return dialog.grab().save(prefix + "-partial.png") ? 0 : 1;
  }
  // Read-only real-game visual/inference check. Optional marks are source
  // pixels: camera feature x y, one per line. Screenshots stay outside Git.
  if (argc >= 4 && QString(argv[1]) == "--inspect-game") {
    RinkMapDialog dialog(QString::fromLocal8Bit(argv[2]));
    dialog.show();
    if (!idle(dialog))
      return 1;
    auto* detect = dialog.findChild<QPushButton*>("detectRinkLandmarksButton");
    if (!expect(detect->isEnabled(), qPrintable(dialog.findChild<QLabel*>("rinkMapStatus")->text())))
      return 1;
    auto* table = dialog.findChild<QTableWidget*>("rinkMapObservationsTable");
    const int initial_rows = table->rowCount();
    detect->click();
    if (!idle(dialog, 240000))
      return 1;
    if (!expect(
            dialog.findChild<QPushButton*>("rinkMapUndoButton")->isEnabled(),
            "Successful native detection must create an undo step"))
      return 1;
    const int detected_rows = table->rowCount();
    dialog.findChild<QPushButton*>("rinkMapUndoButton")->click();
    if (!expect(
            idle(dialog) && table->rowCount() == initial_rows && !dialog.isWindowModified(),
            "Undo a real detection restores the initial inputs"))
      return 1;
    dialog.findChild<QPushButton*>("rinkMapRedoButton")->click();
    if (!expect(idle(dialog) && table->rowCount() == detected_rows, "Redo restores the detected observations"))
      return 1;
    std::cout << dialog.findChild<QLabel*>("rinkMapStatus")->text().toStdString() << '\n';
    if (qEnvironmentVariableIsSet("HSTREAM_INSPECT_RINK_MASK")) {
      dialog.findChild<QPushButton*>("suggestRinkMaskButton")->click();
      if (!idle(dialog, 60000))
        return 1;
      std::cout << dialog.findChild<QLabel*>("rinkMapStatus")->text().toStdString() << '\n';
      if (!expect(table->rowCount() == detected_rows + 1, "Real mask should add one reviewable far-board suggestion"))
        return 1;
      const int board_row = table->rowCount() - 1;
      if (!expect(
              table->item(board_row, 0)->checkState() == Qt::Unchecked &&
                  table->item(board_row, 2)->text().contains("mask"),
              "Mask evidence must start unchecked with its source visible"))
        return 1;
      dialog.findChild<QPushButton*>("rinkMapUndoButton")->click();
      if (!expect(idle(dialog) && table->rowCount() == detected_rows, "Undo removes the mask suggestion"))
        return 1;
      dialog.findChild<QPushButton*>("rinkMapRedoButton")->click();
      if (!expect(idle(dialog) && table->rowCount() == detected_rows + 1, "Redo restores the mask suggestion"))
        return 1;
      table->item(board_row, 0)->setCheckState(Qt::Checked);
      if (!expect(
              idle(dialog) && dialog.findChild<QPushButton*>("saveRinkMapButton")->isEnabled(),
              "Reviewed mask evidence can be retained in a partial map"))
        return 1;
      dialog.findChild<QPushButton*>("rinkMapUndoButton")->click();
      if (!expect(
              idle(dialog) && table->item(board_row, 0)->checkState() == Qt::Unchecked,
              "Undo restores the mask review state"))
        return 1;
      dialog.findChild<QPushButton*>("suggestRinkMaskButton")->click();
      if (!expect(
              idle(dialog, 60000) && table->rowCount() == detected_rows + 1,
              "Refreshing the mask replaces its old suggestion without duplicating evidence"))
        return 1;
      size_t guides = 0;
      for (auto* view : dialog.findChildren<QGraphicsView*>())
        for (auto* item : view->scene()->items())
          guides += item->data(0).toString() == "rinkMaskIntersectionGuide";
      if (!expect(guides > 0, "The real centre line has a visual far-board intersection guide"))
        return 1;
    }
    const QString prefix = QString::fromLocal8Bit(argv[3]);
    dialog.grab().save(prefix + "-partial.png");
    if (argc >= 5) {
      std::ifstream marks(argv[4]);
      int camera;
      std::string feature;
      double x, y;
      while (marks >> camera >> feature >> x >> y)
        if (!mark(dialog, camera, QString::fromStdString(feature), {x, y}))
          return 1;
      std::cout << dialog.findChild<QLabel*>("rinkMapStatus")->text().toStdString() << '\n';
      dialog.findChild<QTabWidget*>("rinkMapCameraTabs")->setCurrentIndex(0);
      QTest::qWait(30);
      dialog.grab().save(prefix + "-reviewed.png");
      dialog.findChild<QTabWidget*>("rinkMapCameraTabs")->setCurrentIndex(1);
      QTest::qWait(30);
      dialog.grab().save(prefix + "-right.png");
      if (argc == 6 && QString(argv[5]) == "--save-reviewed-map") {
        auto* save = dialog.findChild<QPushButton*>("saveRinkMapButton");
        if (!expect(
                save->isEnabled() && save->text() == "Save reviewed map",
                "Only a reviewed map can be prepared for playback"))
          return 1;
        save->click();
        if (!expect(
                dialog.findChild<QLabel*>("rinkMapStatus")->text().startsWith("Saved reviewed rink map"),
                "Playback map preparation must save successfully"))
          return 1;
      }
    }
    return 0;
  }
  QTemporaryDir game, bin;
  bool ok = game.isValid() && bin.isValid();
  QImage source(3600, 1800, QImage::Format_RGB32);
  source.fill(QColor(120, 150, 160));
  ok &= source.save(game.filePath("left.png")) && source.save(game.filePath("right.png"));
  const QByteArray pto =
      "p f2 w3600 h1800 v360 n\"PNG\"\ni w3600 h1800 f4 v360 y0 p0 r0 n\"left.png\"\ni w3600 h1800 f4 v360 y0 p0 r0 n\"right.png\"\n";
  ok &= write(game.filePath("autooptimiser_out.pto"), pto);
  ok &= write(game.filePath("hm_project.pto"), pto);
  for (const char* name : {"mapping_0000.tif", "mapping_0000_x.tif", "mapping_0000_y.tif"})
    ok &= placement(game.filePath(name), 900);
  for (const char* name : {"mapping_0001.tif", "mapping_0001_x.tif", "mapping_0001_y.tif"})
    ok &= placement(game.filePath(name), 1800);
  ok &= write(game.filePath("seam_file.png"), "fixture");
  const QByteArray config = "hstream_ui:\n  stitching_calibration:\n    status: complete\n";
  ok &= write(game.filePath("config.yaml"), config);
  ok &= write(
      game.filePath("stitching_canvas_provenance"),
      "version=3\nmax-output-width=1800\nmax-canvas-dimension=0\nsource-canvas-width=7200\nsource-canvas-height=3600\ncanvas-width=1800\ncanvas-height=1200\nmax-output-width-applied=1\nmax-canvas-dimension-applied=0\nmapping-backend=nona\nprojection=equirectangular\n");
  ok &= write(bin.filePath("pano_trafo"), "#!/bin/sh\nawk '{print $2, $3}'\n");
  QFile::setPermissions(bin.filePath("pano_trafo"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  qputenv("PATH", bin.path().toLocal8Bit() + ":" + qgetenv("PATH"));
  auto snapshot = LoadRinkMapSnapshot(game.path());
  if (!expect(ok && snapshot.ok(), snapshot.ok() ? "fixture" : snapshot.status().ToString().c_str()))
    return 1;
  auto competitor = *snapshot;
  rink::RinkMapDocument document;
  document.generation = snapshot->generation;
  document.calibration_revision = snapshot->revision.toStdString();
  rink::RinkMapObservation centre;
  centre.feature = "center-line";
  centre.line = true;
  centre.pixels = {sourcePoint({0, -5}), sourcePoint({0, 5})};
  document.observations = {centre};
  auto partial = UpdateRinkMap(*snapshot, document, false, {});
  if (!expect(
          partial.ok() && partial->fit.state == rink::RinkMapState::kPartial, "A single centre line remains partial"))
    return 1;
  ok &= expect(
      partial->projected_layout[0].empty() && SaveRinkMap(*snapshot, partial->document).ok(),
      "Partial evidence saves without fabricating an outline");
  {
    RinkMapDialog dialog(game.path());
    dialog.show();
    ok &= expect(idle(dialog), "Partial reopen completes");
    auto wheel = [](QWidget* widget, bool pixels = false, bool include_angle = false) {
      const QPoint position = widget->rect().center();
      QWheelEvent event(
          position,
          widget->mapToGlobal(position),
          pixels ? QPoint(0, 24) : QPoint(),
          pixels && !include_angle ? QPoint() : QPoint(0, 120),
          Qt::NoButton,
          Qt::NoModifier,
          Qt::NoScrollPhase,
          false);
      QApplication::sendEvent(widget, &event);
    };
    auto* feature = dialog.findChild<QComboBox*>("rinkMapFeatureCombo");
    auto* table = dialog.findChild<QTableWidget*>("rinkMapObservationsTable");
    auto* identity = qobject_cast<QComboBox*>(table->cellWidget(0, 1));
    for (bool focused : {false, true}) {
      for (bool pixels : {false, true}) {
        for (auto* combo : {feature, identity}) {
          const int before = combo->currentIndex();
          if (focused)
            combo->setFocus();
          else
            combo->clearFocus();
          wheel(combo, pixels);
          ok &= expect(combo->currentIndex() == before, "Wheel must not change feature or observed identity");
        }
        for (auto* spin : dialog.findChildren<QDoubleSpinBox*>()) {
          const double before = spin->value();
          if (focused)
            spin->setFocus();
          else
            spin->clearFocus();
          wheel(spin, pixels);
          wheel(spin->findChild<QLineEdit*>(), pixels);
          ok &= expect(spin->value() == before, "Wheel must not change rink dimensions, even with focus");
        }
      }
    }
    auto* camera = dialog.findChild<QGraphicsView*>("rinkMapCamera0");
    const double zoom = camera->transform().m11();
    wheel(camera->viewport());
    ok &= expect(camera->transform().m11() > zoom, "Image wheel zoom remains available");
    auto* save = dialog.findChild<QPushButton*>("saveRinkMapButton");
    ok &= expect(
        save->isEnabled() && save->text() == "Save partial map", "A saved single line stays partial after reopening");
    ok &= expect(
        dialog.findChild<QLabel*>("rinkMapMinimapReadiness")->text().contains("Not ready") &&
            dialog.findChild<QLabel*>("rinkMapFramingReadiness")->text().contains("Not ready") &&
            dialog.findChild<QLabel*>("rinkMapGuidanceLabel")->text().contains("parallel") &&
            dialog.findChild<QLabel*>("rinkMapGuidanceLabel")->text().contains("add or enable"),
        "Partial maps explain both blocked uses, the geometry gap and a concrete completion option");
    // Exercise the same table with enough rows to expose pixel/row confusion.
    // This extra selector has no document binding; it only tests wheel routing.
    table->setRowCount(80);
    auto* scrolling_identity = new QComboBox();
    scrolling_identity->addItems({"Centre", "Blue", "Goal"});
    scrolling_identity->setCurrentIndex(1);
    table->setCellWidget(40, 1, scrolling_identity);
    for (auto mode : {QAbstractItemView::ScrollPerItem, QAbstractItemView::ScrollPerPixel}) {
      table->setVerticalScrollMode(mode);
      QTest::qWait(10);
      auto* bar = table->verticalScrollBar();
      const int start = bar->maximum() / 2;
      ok &= expect(start > 24, "Observation table must have room to scroll");
      for (bool pixels : {false, true}) {
        bar->setValue(start);
        wheel(table->viewport(), pixels, true);
        const int expected = bar->value();
        ok &= expect(expected < start, "Native table wheel input scrolls upwards");
        bar->setValue(start);
        wheel(scrolling_identity, pixels, true);
        ok &= expect(bar->value() == expected, "Wheel over a selector must match native table scrolling");
        ok &= expect(scrolling_identity->currentIndex() == 1, "Scrolling must preserve the observed identity");
      }
    }
  }
  {
    RinkMapDialog dialog(game.path());
    dialog.show();
    ok &= expect(idle(dialog), "History fixture opens");
    auto* undo = dialog.findChild<QPushButton*>("rinkMapUndoButton");
    auto* redo = dialog.findChild<QPushButton*>("rinkMapRedoButton");
    auto* table = dialog.findChild<QTableWidget*>("rinkMapObservationsTable");
    auto* save = dialog.findChild<QPushButton*>("saveRinkMapButton");
    auto* width = dialog.findChild<QDoubleSpinBox*>("rinkMapDimension1");
    ok &= expect(!undo->isEnabled() && !redo->isEnabled() && !dialog.isWindowModified(), "Opening a map is clean");
    ok &= expect(
        dialog.findChild<QLabel*>("rinkMapGuidanceLabel")->text().contains("parallel", Qt::CaseInsensitive),
        "Parallel-only evidence gives a concrete next step");
    auto* drag_view = dialog.findChild<QGraphicsView*>("rinkMapCamera0");
    auto find_anchor = [&]() -> QGraphicsItem* {
      for (auto* item : drag_view->scene()->items())
        if (item->data(0) == "rinkMapAnchor" && item->data(1).toInt() == 0 && item->data(2).toInt() == 0)
          return item;
      return nullptr;
    };
    auto* handle = find_anchor();
    QGraphicsPathItem* line_path = nullptr;
    for (auto* item : drag_view->scene()->items())
      if (item->data(0) == "rinkMapObservationPath" && item->data(1).toInt() == 0)
        line_path = dynamic_cast<QGraphicsPathItem*>(item);
    if (!expect(handle && line_path, "Manual line has a draggable endpoint and visible path"))
      return 1;
    const QPointF original = handle->pos();
    const QPoint start = drag_view->mapFromScene(original);
    const QPoint finish = start + QPoint(24, 18);
    QTest::mousePress(drag_view->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent motion(
        QEvent::MouseMove,
        finish,
        drag_view->viewport()->mapToGlobal(finish),
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(drag_view->viewport(), &motion);
    const auto endpoint = line_path->path().elementAt(0);
    ok &= expect(
        QLineF(original, handle->pos()).length() > 10 &&
            QLineF(handle->pos(), QPointF(endpoint.x, endpoint.y)).length() < .01,
        "Point and attached line move before release");
    ok &= expect(
        !dialog.isWindowModified() && !undo->isEnabled() && table->rowCount() == 1,
        "Drag preview leaves document and history untouched");
    QTest::mouseRelease(drag_view->viewport(), Qt::LeftButton, Qt::NoModifier, finish);
    ok &= expect(
        waitUntil([&] { return undo->isEnabled(); }) && idle(dialog) && dialog.isWindowModified() &&
            table->rowCount() == 1,
        "Release commits one edit without adding a mark");
    undo->click();
    ok &= expect(
        idle(dialog) && find_anchor() && QLineF(find_anchor()->pos(), original).length() < .01 &&
            !dialog.isWindowModified() && !undo->isEnabled(),
        "One undo restores the original line");
    const auto anchor = sourcePoint({0, -12.954});
    ok &= expect(mark(dialog, 0, "center-near", {anchor.x, anchor.y}), "Add a manual anchor");
    ok &= expect(
        table->rowCount() == 2 && table->currentRow() == 1 && dialog.isWindowModified(),
        "New mark is selected and unsaved");
    undo->click();
    ok &= expect(idle(dialog) && table->rowCount() == 1 && !dialog.isWindowModified(), "Undo restores clean inputs");
    const QByteArray model = qgetenv("HM_RINK_LANDMARKS_ONNX_MODEL");
    qputenv("HM_RINK_LANDMARKS_ONNX_MODEL", game.filePath("missing.onnx").toLocal8Bit());
    dialog.findChild<QPushButton*>("detectRinkLandmarksButton")->click();
    ok &= expect(idle(dialog) && table->rowCount() == 1 && redo->isEnabled(), "Failed detection preserves redo");
    if (model.isNull())
      qunsetenv("HM_RINK_LANDMARKS_ONNX_MODEL");
    else
      qputenv("HM_RINK_LANDMARKS_ONNX_MODEL", model);
    redo->click();
    ok &= expect(idle(dialog) && table->rowCount() == 2 && dialog.isWindowModified(), "Redo restores manual evidence");
    const double original_width = width->value();
    width->setValue(1);
    width->setValue(.5);
    undo->click();
    ok &= expect(
        idle(dialog) && width->value() == original_width && table->rowCount() == 2,
        "Rapid invalid dimension edits form one undo operation");
    redo->click();
    QTest::qWait(10);
    ok &= expect(width->value() == .5 && !save->isEnabled(), "Redo restores invalid inputs without stale fit");
    undo->click();
    ok &= expect(idle(dialog) && width->value() == original_width, "Invalid dimension redo can be undone");
    save->click();
    ok &= expect(!dialog.isWindowModified(), "Successful save establishes a clean baseline");
    table->item(1, 0)->setCheckState(Qt::Unchecked);
    ok &= expect(idle(dialog) && dialog.isWindowModified() && !redo->isEnabled(), "New edit clears redo branch");
    undo->click();
    ok &= expect(
        idle(dialog) && table->item(1, 0)->checkState() == Qt::Checked && !dialog.isWindowModified(),
        "Undo restores the saved use flag");
    auto* feature = dialog.findChild<QComboBox*>("rinkMapFeatureCombo");
    auto* camera = dialog.findChild<QGraphicsView*>("rinkMapCamera0");
    dialog.findChild<QPushButton*>("rinkMapMarkNearBoardsButton")->click();
    ok &= expect(feature->currentData() == "near-boards", "Guidance shortcut selects the board line tool");
    QTest::mouseClick(camera->viewport(), Qt::LeftButton, Qt::NoModifier, camera->mapFromScene(QPointF(1500, 850)));
    const auto pending = [&] {
      for (auto* item : camera->scene()->items())
        if (item->data(0) == "rinkPendingLinePoint")
          return true;
      return false;
    };
    ok &= expect(pending() && table->rowCount() == 2, "First line point is visible without creating an observation");
    QTest::keyClick(&dialog, Qt::Key_Escape);
    ok &=
        expect(dialog.isVisible() && !pending() && !dialog.isWindowModified(), "Escape cancels unfinished line first");
    width->setValue(original_width - .1);
    QTimer::singleShot(0, &dialog, [&] {
      auto* prompt = dialog.findChild<QMessageBox*>("rinkMapDiscardPrompt");
      if (prompt)
        prompt->findChild<QPushButton*>("rinkMapKeepEditing")->click();
    });
    dialog.reject();
    ok &=
        expect(dialog.isVisible() && idle(dialog) && dialog.isWindowModified(), "Keep editing protects unsaved inputs");
    ok &= write(bin.filePath("pano_trafo"), "#!/bin/sh\nsleep 0.25\nawk '{print $2, $3}'\n");
    width->setValue(original_width - .2);
    auto* stop = dialog.findChild<QPushButton*>("stopRinkMapButton");
    ok &= expect(waitUntil([&] { return stop->isEnabled(); }), "Dimension edit starts a refit");
    bool kept_editing = false;
    QTimer keep_editing;
    QObject::connect(&keep_editing, &QTimer::timeout, &dialog, [&] {
      if (auto* prompt = dialog.findChild<QMessageBox*>("rinkMapDiscardPrompt")) {
        kept_editing = true;
        keep_editing.stop();
        prompt->findChild<QPushButton*>("rinkMapKeepEditing")->click();
      }
    });
    keep_editing.start(10);
    dialog.reject();
    ok &= expect(
        waitUntil([&] { return kept_editing && !stop->isEnabled() && save->isEnabled(); }) && dialog.isVisible() &&
            dialog.isWindowModified(),
        "Keep editing after cancelling a refit restores Save without another edit");
    keep_editing.stop();
    ok &= write(bin.filePath("pano_trafo"), "#!/bin/sh\nawk '{print $2, $3}'\n");
    QTimer::singleShot(0, &dialog, [&] {
      auto* prompt = dialog.findChild<QMessageBox*>("rinkMapDiscardPrompt");
      if (prompt)
        prompt->findChild<QPushButton*>("rinkMapDiscardChanges")->click();
    });
    dialog.close();
    ok &= expect(!dialog.isVisible(), "Window close discards only after explicit selection");
  }
  snapshot = LoadRinkMapSnapshot(game.path());
  if (!expect(snapshot.ok(), "Reload the saved history fixture"))
    return 1;
  document.observations.clear();
  for (const char* id : {"center-near", "center-far", "blue-left-near", "blue-left-far"}) {
    rink::RinkMapObservation observation;
    observation.feature = id;
    observation.pixels = {sourcePoint(*rink::RinkFeaturePoint(document.layout, id))};
    document.observations.push_back(observation);
  }
  auto updated = UpdateRinkMap(*snapshot, document, false, {});
  if (!expect(updated.ok(), updated.ok() ? "update" : updated.status().ToString().c_str()))
    return 1;
  ok &= expect(
      updated->fit.state == rink::RinkMapState::kReady && !updated->projected_layout[0].empty(),
      "Neutral-zone anchors project the entire rink");
  {
    const auto level = RinkLandmarkLeveling(*updated, {0, 0, 0}, 23);
    ok &= expect(
        level.ok() && std::abs(level->rotation_degrees[0] - 23) < 1e-8 && std::abs(level->rotation_degrees[1]) < 0.01 &&
            std::abs(level->rotation_degrees[2]) < 0.01,
        "Saved metric landmarks recover the ice normal while preserving yaw");
    RinkMapWorkResult corners;
    for (const char* id : {"goal-left-near", "goal-left-far", "goal-right-near", "goal-right-far"}) {
      rink::RinkMapObservation mark;
      mark.feature = id;
      const auto point = *rink::RinkFeaturePoint(document.layout, id);
      cv::Vec3d ray(point.x - 3, point.y + 19, -7);
      ray /= cv::norm(ray);
      mark.rays = {ray};
      corners.document.observations.push_back(mark);
    }
    const auto partial_level = RinkLandmarkLeveling(corners, {0, 0, 0}, 17);
    ok &= expect(
        partial_level.ok() && partial_level->rotation_degrees[0] == 17,
        "Explicit goal/board corners can level even when the metric map is partial");
    corners.document.observations.back().enabled = false;
    const auto incomplete_level = RinkLandmarkLeveling(corners, {0, 0, 0}, 0);
    ok &= expect(
        !incomplete_level.ok() && incomplete_level.status().message().find("four corners") != std::string::npos,
        "Missing or disabled goal corners explain why leveling needs more evidence");
    ok &= expect(
        !RinkLandmarkLeveling(*partial, {0, 0, 0}, 0).ok(),
        "Automatic line fragments are not fabricated rectangle corners");
  }
  // QApplication adopts the desktop's C locale. Coordinate parsing must still
  // accept pano_trafo's dot decimals, including its out-of-view nan/inf values.
  {
    const std::string saved_locale = std::setlocale(LC_NUMERIC, nullptr);
    bool comma_locale = false;
    for (const char* locale : {"en_DK.utf8", "de_DE.UTF-8", "fr_FR.UTF-8"}) {
      if (std::setlocale(LC_NUMERIC, locale) && std::string(std::localeconv()->decimal_point) == ",") {
        comma_locale = true;
        break;
      }
    }
    if (comma_locale) {
      auto localized = UpdateRinkMap(*snapshot, document, false, {});
      ok &= expect(
          localized.ok() && localized->fit.state == rink::RinkMapState::kReady,
          "Dot-decimal transforms work with a comma-decimal desktop locale");
    } else {
      std::cout << "SKIP: no comma-decimal locale installed\n";
    }
    ok &= write(
        bin.filePath("pano_trafo"),
        "#!/bin/sh\nif [ \"$1\" = \"-r\" ]; then\n"
        "awk 'NR==1 {print \"nan -nan\"; next} NR==2 {print \"inf -inf\"; next} {print $2, $3}'\n"
        "else\nawk '{print $2, $3}'\nfi\n");
    auto outside = UpdateRinkMap(*snapshot, document, false, {});
    ok &= expect(
        outside.ok() && outside->fit.state == rink::RinkMapState::kReady && !outside->projected_layout[0].empty() &&
            std::isnan(outside->projected_layout[0][0][0].x) && std::isnan(outside->projected_layout[0][0][0].y) &&
            std::isinf(outside->projected_layout[0][0][1].x) && std::isinf(outside->projected_layout[0][0][1].y),
        "Out-of-view projections retain nonfinite coordinates without rejecting the map");
    std::setlocale(LC_NUMERIC, saved_locale.c_str());
    ok &= write(bin.filePath("pano_trafo"), "#!/bin/sh\nawk '{print $2, $3}'\n");
  }
  document = updated->document;
  ok &= expect(SaveRinkMap(*snapshot, document).ok(), "Save a complete map");
  if (access("/usr/bin/pano_trafo", X_OK) == 0) {
    auto playback = rink::LoadPlaybackRinkMap(game.path().toStdString());
    ok &= expect(
        playback.ok(),
        playback.ok() ? "Native playback consumes the editor map" : playback.status().ToString().c_str());
    if (playback.ok())
      ok &= expect(
          cv::norm((*playback)->rays.front() - cv::Vec3d(0, 0.5, std::sqrt(3.0) / 2)) < 1e-6,
          "Capped playback uses the published TIFF origin and extent, not source dimensions or PTO crop origin");
  }
  ok &= expect(!SaveRinkMap(competitor, document).ok(), "A competing editor must not overwrite a newer map");
  ok &= write(game.filePath("config.yaml"), config + "unrelated_preference: true\n");
  ok &= expect(SaveRinkMap(*snapshot, document).ok(), "Unrelated preferences must not invalidate rink evidence");
  {
    RinkMapDialog dialog(game.path());
    dialog.show();
    ok &= expect(idle(dialog), "Reopen completes");
    auto* save = dialog.findChild<QPushButton*>("saveRinkMapButton");
    ok &= expect(
        save->isEnabled() && save->text() == "Save reviewed map",
        "Reopen recomputes a saved map from its source marks");
    ok &= expect(
        dialog.findChild<QLabel*>("rinkMapMinimapReadiness")->text().contains("Ready — saved") &&
            dialog.findChild<QLabel*>("rinkMapFramingReadiness")->text().contains("Ready — saved"),
        "Saved valid geometry enables both uses without requiring every rink feature");
    // A physically implausible fit is still evidence worth retaining.
    auto* rejected_width = dialog.findChild<QDoubleSpinBox*>("rinkMapDimension1");
    rejected_width->setValue(40);
    ok &= expect(
        waitUntil([&] { return save->isEnabled() && save->text() == "Save partial map"; }),
        "Rejected fit keeps partial Save available");
    ok &= expect(
        dialog.findChild<QLabel*>("rinkMapStatus")->text().contains("Cannot fit a reliable rink map"),
        "Rejected fit explains uncertainty without blaming a flat rink");
    save->click();
    ok &= expect(
        !dialog.isWindowModified() && !rink::LoadPlaybackRinkMap(game.path().toStdString()).ok(),
        "Saving rejected evidence succeeds without enabling player locations");
    ok &= expect(
        dialog.findChild<QLabel*>("rinkMapFramingReadiness")->text().contains("marks need correction"),
        "Saving inconsistent evidence never reports it ready for framing");
    {
      RinkMapDialog reopened(game.path());
      reopened.show();
      ok &= expect(
          idle(reopened) && reopened.findChild<QTableWidget*>("rinkMapObservationsTable")->rowCount() == 4 &&
              reopened.findChild<QPushButton*>("saveRinkMapButton")->isEnabled() &&
              reopened.findChild<QPushButton*>("saveRinkMapButton")->text() == "Save partial map",
          "Rejected evidence reopens intact and remains partial");
    }
    dialog.findChild<QPushButton*>("rinkMapUndoButton")->click();
    ok &= expect(idle(dialog) && save->text() == "Save reviewed map", "Undo restores the valid dimensions");
    auto* width = dialog.findChild<QDoubleSpinBox*>("rinkMapDimension1");
    width->setValue(1);
    ok &= expect(!save->isEnabled(), "Changed dimensions disable save immediately, before debounce");
    QTest::qWait(300);
    ok &= expect(!save->isEnabled(), "Invalid dimensions cannot save stale geometry");
    auto* table = dialog.findChild<QTableWidget*>("rinkMapObservationsTable");
    auto* remove = dialog.findChild<QPushButton*>("removeRinkMapObservationButton");
    table->selectRow(0);
    remove->click();
    ok &= expect(
        waitUntil([&] { return table->rowCount() == 3 && table->isEnabled(); }),
        "Removing a mark with invalid dimensions rebuilds the table");
    table->item(2, 0)->setCheckState(Qt::Unchecked);
    ok &= expect(waitUntil([&] { return table->isEnabled(); }), "Invalid-dimension checkbox edits remain safe");
    auto* identity = qobject_cast<QComboBox*>(table->cellWidget(2, 1));
    identity->setCurrentIndex(identity->findData("center"));
    ok &= expect(waitUntil([&] { return table->isEnabled(); }), "Invalid-dimension identity edits remain safe");
    ok &= expect(save->text() == "Save partial map", "Invalid dimensions discard the previous ready fit");
    while (table->rowCount() > 0) {
      const int before = table->rowCount();
      table->selectRow(0);
      remove->click();
      if (!expect(waitUntil([&] { return table->rowCount() == before - 1; }), "Every removal updates row indices"))
        return 1;
    }
    width->setValue(document.layout.width);
    ok &= expect(waitUntil([&] { return save->isEnabled(); }), "Clearing a saved map can be persisted");
    save->click();
  }
  {
    RinkMapDialog dialog(game.path());
    dialog.show();
    ok &= expect(idle(dialog), "Cleared map reopen completes");
    ok &= expect(
        dialog.findChild<QTableWidget*>("rinkMapObservationsTable")->rowCount() == 0 &&
            dialog.findChild<QPushButton*>("saveRinkMapButton")->text() == "Save partial map",
        "Reopening a cleared map cannot resurrect removed marks or goal estimates");
  }
  ok &= write(game.filePath("config.yaml"), "hstream_ui:\n  stitching_calibration:\n    status: pending\n");
  ok &= expect(!rink::LoadPlaybackRinkMap(game.path().toStdString()).ok(), "Playback rejects pending calibration");
  ok &= expect(!SaveRinkMap(*snapshot, document).ok(), "Pending recalibration blocks saving old evidence");
  ok &= write(game.filePath("config.yaml"), config);
  ok &= write(game.filePath("autooptimiser_out.pto"), pto + "# changed\n");
  ok &= expect(!SaveRinkMap(*snapshot, document).ok(), "Changed camera geometry blocks a stale save");
  if (ok)
    std::cout << "RinkMapDialog tests passed\n";
  return ok ? 0 : 1;
}
