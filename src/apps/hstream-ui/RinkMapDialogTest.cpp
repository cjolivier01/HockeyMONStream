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
