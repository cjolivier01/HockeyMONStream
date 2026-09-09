#include "src/apps/hstream-ui/ProjectionCropDialog.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <cmath>
#include <iostream>

namespace {
bool expect(bool ok, const char* message) {
  if (!ok)
    std::cerr << message << '\n';
  return ok;
}
bool write(const QString& path, const QByteArray& contents, bool executable = false) {
  QFile file(path);
  const bool ok = file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
  if (ok && executable)
    file.setPermissions(file.permissions() | QFile::ExeOwner);
  return ok;
}
bool waitUntil(const std::function<bool()>& condition, int timeout = 5000) {
  for (int elapsed = 0; elapsed < timeout; elapsed += 20) {
    if (condition())
      return true;
    QTest::qWait(20);
  }
  return condition();
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  hm::stitching::StitchProjectionFraming framing;
  framing.crop = {0.05, 0.95, 0.20, 0.90};
  hm::stitching::StitchCameraSelection camera;
  // Real offline integration/visual check: explicit game path and screenshot path.
  if (argc == 3) {
    framing.auto_fov = true;
    framing.rotation_degrees = {0, -32.511, 2.861};
    ProjectionCropDialog dialog(QString::fromLocal8Bit(argv[1]), framing, "general-panini", {100, 0, 0}, camera);
    dialog.show();
    if (!waitUntil([&]() { return !dialog.sourceRevision().isEmpty(); }, 65000)) {
      std::cerr << dialog.findChild<QLabel*>("projectionCropStatus")->text().toStdString() << '\n';
      return 1;
    }
    dialog.findChild<QCheckBox*>("projectionCropKeepWidth")->setChecked(true);
    QTest::qWait(500);
    dialog.grab().save(QString::fromLocal8Bit(argv[2]));
    dialog.findChild<QPushButton*>("acceptProjectionCropButton")->click();
    return dialog.result() == QDialog::Accepted ? 0 : 1;
  }
  bool ok = true;
  {
    QTemporaryDir partial;
    const auto check_unavailable = [&](const QString& expected) {
      ProjectionCropDialog dialog(partial.path(), framing, "general-panini", {100, 0, 0}, camera);
      dialog.show();
      auto* status = dialog.findChild<QLabel*>("projectionCropStatus");
      auto* canvas = dialog.findChild<QWidget*>("projectionCropCanvas");
      auto* mode = dialog.findChild<QComboBox*>("projectionCropMode");
      mode->setCurrentIndex(mode->findData("manual"));
      const bool explained = waitUntil([&]() { return status->text().contains(expected); });
      const bool visible = canvas->accessibleDescription().contains(expected);
      const bool numeric_available = dialog.findChild<QDoubleSpinBox*>("projectionCropTop")->isEnabled();
      dialog.close();
      return expect(
          explained && visible && numeric_available && dialog.sourceRevision().isEmpty(),
          "An unavailable crop preview must explain why in the canvas and keep numeric trim available");
    };
    ok &= check_unavailable("Stitching calibration is not available yet");
    ok &= write(partial.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: pending}}\n");
    ok &= check_unavailable("Stitching calibration is incomplete");
    ok &= write(partial.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: failed}}\n");
    ok &= check_unavailable("Stitching calibration failed");
    ok &= write(partial.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: complete}}\n");
    ok &= check_unavailable("has not produced all the saved camera images and projection");
    ok &= write(partial.filePath("config.yaml"), "{}\n");
    ok &= check_unavailable("has not produced all the saved camera images and projection");
    ok &= write(partial.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: [invalid]}}\n");
    ok &= check_unavailable("Could not read the game's calibration status");
  }
  {
    ProjectionCropDialog dialog({}, framing, "general-panini", {100, 0, 0}, camera, "No saved calibration.");
    dialog.show();
    auto* mode = dialog.findChild<QComboBox*>("projectionCropMode");
    auto* keep = dialog.findChild<QCheckBox*>("projectionCropKeepWidth");
    auto* left = dialog.findChild<QDoubleSpinBox*>("projectionCropLeft");
    auto* right = dialog.findChild<QDoubleSpinBox*>("projectionCropRight");
    ok &= expect(
        mode->currentData() == "manual" && dialog.framing() == framing, "existing manual crop must load exactly");
    keep->setChecked(true);
    ok &= expect(
        dialog.framing().crop == std::array<double, 4>{0, 1, 0.20, 0.90} && !left->isEnabled() && !right->isEnabled(),
        "Keep full width must change only horizontal edges");
    keep->setChecked(false);
    ok &= expect(dialog.framing().crop == framing.crop, "unlocking width restores the previous horizontal trim");
    right->setValue(90);
    left->setValue(99);
    ok &= expect(dialog.framing().crop[0] < dialog.framing().crop[1], "opposite edges cannot cross");
    const auto manual = dialog.framing().crop;
    mode->setCurrentIndex(mode->findData("auto"));
    ok &= expect(
        dialog.framing().auto_crop && dialog.framing().crop == hm::stitching::StitchProjectionFraming{}.crop &&
            !left->isEnabled(),
        "Auto retains backend auto_crop without a conflicting explicit crop");
    mode->setCurrentIndex(mode->findData("full"));
    ok &= expect(
        !dialog.framing().auto_crop && dialog.framing().crop == hm::stitching::StitchProjectionFraming{}.crop,
        "Full canvas disables Auto and removes all edge trims");
    mode->setCurrentIndex(mode->findData("manual"));
    ok &= expect(dialog.framing().crop == manual, "mode changes must preserve the in-dialog manual selection");
    auto* canvas = static_cast<ProjectionCropCanvas*>(dialog.findChild<QWidget*>("projectionCropCanvas"));
    QImage image(800, 400, QImage::Format_RGB32);
    image.fill(Qt::gray);
    canvas->setImage(image);
    left->setValue(10);
    right->setValue(10);
    QTest::qWait(20);
    const auto bounds = canvas->imageRect();
    const QPoint start = (bounds.topLeft() + QPointF(bounds.width() * 0.10, bounds.height() * 0.20)).toPoint();
    const QPoint end = (bounds.topLeft() + QPointF(bounds.width() * 0.03, bounds.height() * 0.12)).toPoint();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(canvas, end);
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
    ok &= expect(
        std::abs(dialog.framing().crop[0] - 0.03) < 0.01 && std::abs(dialog.framing().crop[2] - 0.12) < 0.01 &&
            std::abs(dialog.framing().crop[1] - 0.9) < 0.001,
        "dragging a corner edits its two edges in full-canvas coordinates");
    dialog.close();
    ok &= expect(
        dialog.result() == QDialog::Rejected && framing.crop[0] == 0.05,
        "Cancel must leave the caller's settings unchanged");
  }
  QTemporaryDir game, bin;
  const QByteArray pto =
      "p f19 w800 h400 v180 P\"100 0 0\" S80,720,80,360 n\"PNG\"\n"
      "i w100 h100 f0 v90 y-30 p0 r0 n\"left.png\"\n"
      "i w100 h100 f0 v90 y30 p0 r0 n\"right.png\"\n";
  ok &= write(game.filePath("autooptimiser_out.pto"), pto) && write(game.filePath("config.yaml"), "{}\n");
  ok &= write(
      game.filePath("stitching_canvas_provenance"),
      "version=8\nmapping-backend=nona\nprojection=general-panini\nprojection-parameters=100,0,0\n"
      "projection-auto-fov=0\nprojection-auto-canvas=1\nprojection-horizontal-fov=180\n"
      "projection-rotation-0=0\nprojection-rotation-1=0\nprojection-rotation-2=0\n"
      "camera-configuration=gopro-mission-1\ncamera-horizontal-fov=127.2\ncamera-vertical-fov=95\n");
  QImage source(100, 100, QImage::Format_RGB32);
  source.fill(Qt::gray);
  ok &= source.save(game.filePath("left.png")) && source.save(game.filePath("right.png"));
  ok &= write(
      bin.filePath("pano_modify"),
      "#!/bin/sh\ncase \"$*\" in\n*--crop=AUTO*) cp autooptimiser_out.pto auto.pto;;\n*) cp autooptimiser_out.pto full.pto;;\nesac\n",
      true);
  // Generate the bounded preview fixture with real dimensions; the tool only copies it.
  QImage preview(800, 400, QImage::Format_RGB32);
  preview.fill(QColor(120, 160, 180));
  ok &= preview.save(bin.filePath("fixture.png"));
  ok &= write(bin.filePath("nona"), "#!/bin/sh\ncp \"" + bin.filePath("fixture.png").toUtf8() + "\" full.png\n", true);
  if (!ok)
    return 1;
  const QByteArray path = qgetenv("PATH");
  qputenv("PATH", bin.path().toUtf8() + ":/usr/bin:/bin");
  {
    // Old image artifacts do not make a pending calibration ready.
    ok &= write(game.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: pending}}\n");
    ProjectionCropDialog dialog(game.path(), framing, "general-panini", {100, 0, 0}, camera);
    dialog.show();
    ok &= expect(
        waitUntil([&]() {
          return dialog.findChild<QLabel*>("projectionCropStatus")
              ->text()
              .contains("Stitching calibration is incomplete");
        }) &&
            dialog.sourceRevision().isEmpty(),
        "Pending calibration must be explained even when old preview artifacts exist");
    dialog.close();
    ok &= write(game.filePath("config.yaml"), "{}\n");
  }
  for (bool edit_before_preview : {false, true}) {
    auto automatic = framing;
    automatic.auto_crop = true;
    automatic.crop = {0, 1, 0, 1};
    ProjectionCropDialog dialog(game.path(), automatic, "general-panini", {100, 0, 0}, camera);
    dialog.show();
    auto* mode = dialog.findChild<QComboBox*>("projectionCropMode");
    mode->setCurrentIndex(mode->findData("manual"));
    if (edit_before_preview)
      dialog.findChild<QDoubleSpinBox*>("projectionCropTop")->setValue(30);
    // Loading is asynchronous: changing mode early must still seed Auto's bounds,
    // but a late preview must never overwrite a selection the user already edited.
    ok &= expect(waitUntil([&]() { return !dialog.sourceRevision().isEmpty(); }), "full preview must become available");
    if (edit_before_preview)
      mode->setCurrentIndex(mode->findData("auto"));
    ok &= expect(
        waitUntil([&]() { return dialog.findChild<QLabel*>("projectionCropCoverage")->text().contains("80.0%"); }),
        "automatic bounds must become available");
    mode->setCurrentIndex(mode->findData("manual"));
    ok &= expect(
        dialog.framing().crop ==
            (edit_before_preview ? std::array<double, 4>{0, 1, 0.3, 1} : std::array<double, 4>{0.1, 0.9, 0.2, 0.9}),
        "late Auto bounds must seed only an untouched manual selection");
    dialog.close();
  }
  {
    framing.auto_crop = true;
    framing.crop = {0, 1, 0, 1};
    ProjectionCropDialog dialog(game.path(), framing, "general-panini", {100, 0, 0}, camera);
    dialog.show();
    auto* coverage = dialog.findChild<QLabel*>("projectionCropCoverage");
    ok &= expect(
        waitUntil([&]() { return coverage->text().contains("80.0%"); }),
        "Auto preview must expose the saved Hugin crop bounds");
    if (dialog.sourceRevision().isEmpty())
      std::cerr << dialog.findChild<QLabel*>("projectionCropStatus")->text().toStdString() << '\n';
    auto* mode = dialog.findChild<QComboBox*>("projectionCropMode");
    mode->setCurrentIndex(mode->findData("manual"));
    ok &= expect(
        dialog.framing().crop == std::array<double, 4>{0.1, 0.9, 0.2, 0.9}, "Manual can start from the Auto rectangle");
    write(game.filePath("config.yaml"), "changed: true\n");
    dialog.findChild<QPushButton*>("acceptProjectionCropButton")->click();
    ok &= expect(dialog.result() != QDialog::Accepted, "a stale calibration preview cannot be applied");
    dialog.close();
  }
  {
    ok &= write(game.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: complete}}\n");
    ok &= write(bin.filePath("nona"), "#!/bin/sh\nprintf 'invalid preview' > full.png\n", true);
    ProjectionCropDialog dialog(game.path(), framing, "general-panini", {100, 0, 0}, camera);
    dialog.show();
    auto* status = dialog.findChild<QLabel*>("projectionCropStatus");
    ok &= expect(
        waitUntil([&]() { return status->text().contains("The rendered crop preview could not be loaded:"); }) &&
            !status->text().contains("calibration is incomplete") && dialog.sourceRevision().isEmpty(),
        "An image decode failure after valid calibration must be distinguished from incomplete calibration");
    dialog.close();
  }
  qputenv("PATH", path);
  return ok ? 0 : 1;
}
