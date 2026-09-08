#include "src/apps/hstream-ui/RinkLevelingDialog.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <cmath>
#include <functional>
#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
bool waitUntil(const std::function<bool()>& ready, int timeout = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < timeout) {
    QApplication::processEvents();
    QTest::qWait(5);
  }
  return ready();
}
bool write(const QString& path, const QByteArray& data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}
bool script(const QString& path, const QByteArray& data) {
  return write(path, "#!/bin/sh\n" + data) &&
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
QByteArray read(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void markPosts(RinkLevelingDialog& dialog) {
  static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera0"))
      ->setPoints({{10, 10}, {12, 70}, {70, 12}, {70, 75}});
  static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera1"))
      ->setPoints({{10, 10}, {12, 70}, {70, 12}, {70, 75}});
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // Manual integration/visual check uses real saved Sabercats images and the
  // installed Hugin tools; no production config/artifacts are modified.
  if (argc == 3) {
    RinkLevelingDialog dialog(QString::fromLocal8Bit(argv[1]), {0, -35, 3});
    if (!expect(dialog.loadError().isEmpty(), qPrintable(dialog.loadError())))
      return 1;
    dialog.show();
    static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera0"))
        ->setPoints(
            {{728, 914}, {808, 1099}, {1693, 549}, {1746, 704}, {2576, 248}, {2638, 475}, {3189, 107}, {3242, 355}});
    static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera1"))
        ->setPoints(
            {{589, 32}, {560, 294}, {1218, 109}, {1176, 346}, {1757, 222}, {1714, 422}, {2166, 338}, {2118, 485}});
    auto* estimate = dialog.findChild<QPushButton*>("estimateRinkLevelingButton");
    estimate->click();
    if (!waitUntil([&]() { return estimate->isEnabled(); }, 65000))
      return 1;
    const auto rotation = dialog.rotationDegrees();
    std::cout << "Real image estimate: " << rotation[0] << ',' << rotation[1] << ',' << rotation[2] << '\n';
    std::cout << dialog.findChild<QLabel*>("rinkLevelingStatus")->text().toStdString() << '\n';
    dialog.grab().save(QString::fromLocal8Bit(argv[2]) + "-selection.png");
    auto* preview = dialog.findChild<QPushButton*>("previewRinkLevelingButton");
    preview->click();
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    if (!waitUntil([&]() { return preview->isEnabled(); }, 65000) || !accept->isEnabled()) {
      std::cerr << dialog.findChild<QLabel*>("rinkLevelingStatus")->text().toStdString() << '\n';
      return 1;
    }
    dialog.grab().save(QString::fromLocal8Bit(argv[2]) + "-preview.png");
    const QImage preview_image = dialog.findChild<QWidget*>("rinkLevelingPreview")->grab().toImage();
    const QColor ice = preview_image.pixelColor(preview_image.width() / 2, preview_image.height() / 2);
    if (!expect(ice.lightness() > 30, "real 16-bit camera preview must contain visible rink pixels"))
      return 1;
    accept->click();
    return dialog.result() == QDialog::Accepted ? 0 : 1;
  }
  QTemporaryDir game, bin;
  bool ok = game.isValid() && bin.isValid();
  QImage source(100, 100, QImage::Format_RGB32);
  source.fill(Qt::gray);
  ok &= source.save(game.filePath("left.png")) && source.save(game.filePath("right.png"));
  const QByteArray pto =
      "p f2 w400 h200 v180 n\"PNG\"\ni w100 h100 f0 v90 y-30 p0 r0 n\"left.png\"\n"
      "i w100 h100 f0 v90 y30 p0 r0 n\"right.png\"\n";
  ok &= write(game.filePath("autooptimiser_out.pto"), pto);
  ok &= write(
      game.filePath("stitching_canvas_provenance"),
      "version=8\nmapping-backend=nona\nprojection-rotation-0=0\nprojection-rotation-1=0\nprojection-rotation-2=0\n");
  const QByteArray config =
      "stitching:\n  rink_config: vallco\n  projection_framing:\n    rotation_degrees: [0, -33, 2]\n";
  ok &= write(game.filePath("config.yaml"), config);
  // Plane intersections are exactly vertical, with well-separated azimuths.
  QByteArray transformed;
  constexpr double pi = 3.14159265358979323846;
  for (double azimuth : {-60.0, -20.0, 20.0, 60.0}) {
    for (double z : {0.8, 0.1}) {
      transformed += QByteArray::number(1799.5 - azimuth * 10, 'g', 16) + " " +
          QByteArray::number(899.5 - std::atan(z) * 1800 / pi, 'g', 16) + "\n";
    }
  }
  ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\ncat <<'RAYS'\n" + transformed + "RAYS\n");
  ok &= script(bin.filePath("pano_modify"), "cp autooptimiser_out.pto preview.pto\n");
  ok &= script(bin.filePath("nona"), "cp left.png preview.png\n");
  if (!ok)
    return 1;
  const QByteArray old_path = qgetenv("PATH");
  qputenv("PATH", bin.path().toUtf8() + ":/usr/bin:/bin");
  const auto revision = RinkLevelingDialog::sourceRevision(game.path());
  {
    ScoreboardSelectionCanvas canvas;
    canvas.setLineSelectionMode();
    canvas.setImage(game.filePath("left.png"));
    canvas.resize(600, 600);
    canvas.show();
    QApplication::processEvents();
    canvas.fitImage();
    for (int index = 0; index < 10; ++index) {
      canvas.zoomBy(2);
      canvas.zoomBy(0.5);
    }
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(123, 183));
    ok &= expect(canvas.points() == QVector<QPoint>{{20, 30}}, "zoom cycles preserve source pixel-center coordinates");
    canvas.clearPoints();
    canvas.resize(800, 700);
    QApplication::processEvents();
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(223, 233));
    ok &= expect(
        canvas.points() == QVector<QPoint>{{20, 30}}, "resize preserves pixel centers at the pinned view center");
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    ok &= expect(dialog.loadError().isEmpty(), "snapshot loads");
    dialog.show();
    markPosts(dialog);
    dialog.findChild<QPushButton*>("estimateRinkLevelingButton")->click();
    ok &= expect(
        waitUntil([&]() { return dialog.findChild<QPushButton*>("estimateRinkLevelingButton")->isEnabled(); }),
        "estimate completes");
    ok &= expect(
        std::abs(dialog.rotationDegrees()[1]) < 0.001 && std::abs(dialog.rotationDegrees()[2]) < 0.001,
        "selected poles set absolute level");
    dialog.findChild<QDoubleSpinBox*>("rinkLevelingPitch")->setValue(-31.5);
    dialog.findChild<QPushButton*>("previewRinkLevelingButton")->click();
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "preview required before acceptance");
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    ok &= expect(
        dialog.result() == QDialog::Rejected && read(game.filePath("config.yaml")) == config &&
            RinkLevelingDialog::sourceRevision(game.path()) == revision,
        "cancel after estimate and preview changes nothing");
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    dialog.findChild<QPushButton*>("previewRinkLevelingButton")->click();
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "manual preview completes");
    write(game.filePath("autooptimiser_out.pto"), pto + "# changed generation\n");
    accept->click();
    ok &= expect(dialog.result() != QDialog::Accepted && !accept->isEnabled(), "reject stale selection");
    write(game.filePath("autooptimiser_out.pto"), pto);
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.findChild<QPushButton*>("previewRinkLevelingButton")->click();
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "repeat preview completes");
    accept->click();
    ok &= expect(
        dialog.result() == QDialog::Accepted && dialog.rotationDegrees() == std::array<double, 3>{0, -33, 2},
        "accept preserves displayed absolute values");
    ok &= expect(
        read(game.filePath("config.yaml")) == config, "dialog acceptance stages values without publishing config");
  }
  {
    ok &= script(bin.filePath("nona"), "exec sleep 30\n");
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    dialog.findChild<QPushButton*>("previewRinkLevelingButton")->click();
    QTest::qWait(100);
    QElapsedTimer elapsed;
    elapsed.start();
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    ok &= expect(
        elapsed.elapsed() < 4000 && dialog.result() == QDialog::Rejected &&
            read(game.filePath("config.yaml")) == config && RinkLevelingDialog::sourceRevision(game.path()) == revision,
        "cancel stops an active renderer promptly without changing game artifacts");
    ok &= script(bin.filePath("nona"), "cp left.png preview.png\n");
  }
  {
    QImage mismatch(20, 20, QImage::Format_RGB32);
    mismatch.fill(Qt::black);
    mismatch.save(game.filePath("right.png"));
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    ok &= expect(
        !dialog.loadError().isEmpty() && !dialog.findChild<QPushButton*>("previewRinkLevelingButton")->isEnabled(),
        "mismatched camera dimensions fail closed");
  }
  qputenv("PATH", old_path);
  return ok ? 0 : 1;
}
