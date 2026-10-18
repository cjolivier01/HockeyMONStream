#include "src/apps/hstream-ui/RinkMapDialog.h"

#include <cmath>
#include <fstream>
#include <iostream>

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
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
    detect->click();
    if (!idle(dialog, 240000))
      return 1;
    std::cout << dialog.findChild<QLabel*>("rinkMapStatus")->text().toStdString() << '\n';
    const QString prefix = QString::fromLocal8Bit(argv[3]);
    dialog.grab().save(prefix + "-partial.png");
    if (argc == 5) {
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
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif",
        "seam_file.png"})
    ok &= write(game.filePath(name), "fixture");
  const QByteArray config = "hstream_ui:\n  stitching_calibration:\n    status: complete\n";
  ok &= write(game.filePath("config.yaml"), config);
  ok &= write(
      game.filePath("stitching_canvas_provenance"),
      "version=3\nmax-output-width=0\nmax-canvas-dimension=0\nsource-canvas-width=3600\nsource-canvas-height=1800\ncanvas-width=3600\ncanvas-height=1800\nmax-output-width-applied=0\nmax-canvas-dimension-applied=0\nmapping-backend=nona\nprojection=equirectangular\n");
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
    auto* save = dialog.findChild<QPushButton*>("saveRinkMapButton");
    ok &= expect(
        save->isEnabled() && save->text() == "Save partial map", "A saved single line stays partial after reopening");
  }
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
  document = updated->document;
  ok &= expect(SaveRinkMap(*snapshot, document).ok(), "Save a complete map");
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
    auto* width = dialog.findChild<QDoubleSpinBox*>("rinkMapDimension1");
    width->setValue(1);
    ok &= expect(!save->isEnabled(), "Changed dimensions disable save immediately, before debounce");
    QTest::qWait(300);
    ok &= expect(!save->isEnabled(), "Invalid dimensions cannot save stale geometry");
  }
  ok &= write(game.filePath("config.yaml"), "hstream_ui:\n  stitching_calibration:\n    status: pending\n");
  ok &= expect(!SaveRinkMap(*snapshot, document).ok(), "Pending recalibration blocks saving old evidence");
  ok &= write(game.filePath("config.yaml"), config);
  ok &= write(game.filePath("autooptimiser_out.pto"), pto + "# changed\n");
  ok &= expect(!SaveRinkMap(*snapshot, document).ok(), "Changed camera geometry blocks a stale save");
  if (ok)
    std::cout << "RinkMapDialog tests passed\n";
  return ok ? 0 : 1;
}
