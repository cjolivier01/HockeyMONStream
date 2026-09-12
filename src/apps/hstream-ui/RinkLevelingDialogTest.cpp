#include "src/apps/hstream-ui/RinkLevelingDialog.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>

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
void advanceToPreview(RinkLevelingDialog& dialog) {
  auto* tabs = dialog.findChild<QTabWidget*>("rinkLevelingTabs");
  auto* next = dialog.findChild<QPushButton*>("nextRinkLevelingButton");
  if (tabs->currentIndex() == 2)
    dialog.findChild<QPushButton*>("previousRinkLevelingButton")->click();
  if (tabs->currentIndex() == 0)
    next->click();
  next->click();
}
bool estimateComplete(RinkLevelingDialog& dialog, int timeout = 10000) {
  auto* status = dialog.findChild<QLabel*>("rinkLevelingStatus");
  return status && waitUntil([status]() { return status->text().startsWith("Used "); }, timeout);
}
hm::stitching::StitchProjectionFraming previewFraming(const std::array<double, 3>& rotation) {
  hm::stitching::StitchProjectionFraming framing;
  framing.auto_fov = true;
  framing.auto_canvas = true;
  framing.auto_crop = true;
  framing.rotation_degrees = rotation;
  return framing;
}
bool writeInProgressSnapshot(const QTemporaryDir& staging, const QImage& source, const QByteArray& pto) {
  return source.save(staging.filePath("left.png")) && source.save(staging.filePath("right.png")) &&
      write(staging.filePath("autooptimiser_out.pto"), pto) &&
      write(staging.filePath(".autooptimiser_out.aligned.pto"), pto);
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
    if (!estimateComplete(dialog, 65000))
      return 1;
    const auto rotation = dialog.rotationDegrees();
    std::cout << "Real image estimate: " << rotation[0] << ',' << rotation[1] << ',' << rotation[2] << '\n';
    std::cout << dialog.findChild<QLabel*>("rinkLevelingStatus")->text().toStdString() << '\n';
    dialog.grab().save(QString::fromLocal8Bit(argv[2]) + "-selection.png");
    auto* preview = dialog.findChild<QPushButton*>("nextRinkLevelingButton");
    advanceToPreview(dialog);
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
  const QByteArray provenance =
      "version=8\nmapping-backend=nona\nprojection-rotation-0=0\nprojection-rotation-1=0\nprojection-rotation-2=0\n"
      "camera-configuration=test-camera\ncamera-horizontal-fov=90\ncamera-vertical-fov=90\n";
  ok &= write(game.filePath("stitching_canvas_provenance"), provenance);
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
  ok &= script(
      bin.filePath("pano_modify"),
      "if [ -n \"$RINK_PREVIEW_ARGS\" ]; then printf 'BEGIN\\nLC_ALL=%s\\n' \"$LC_ALL\" >>\"$RINK_PREVIEW_ARGS\"; printf '%s\\n' \"$@\" >>\"$RINK_PREVIEW_ARGS\"; fi\n"
      "output=\ninput=\nexpect_output=0\n"
      "for argument do\n"
      "  if [ \"$expect_output\" = 1 ]; then output=$argument; expect_output=0; continue; fi\n"
      "  case \"$argument\" in --output=*) output=${argument#--output=} ;; -o) expect_output=1 ;; -*) ;; *) input=$argument ;; esac\n"
      "done\n"
      "cp \"$input\" \"$output\"\n");
  ok &= script(bin.filePath("nona"), "cp left.png preview.png\n");
  if (!ok)
    return 1;
  const QByteArray old_path = qgetenv("PATH");
  const QByteArray old_pano_trafo = qgetenv("HM_PANO_TRAFO");
  const QByteArray old_pano_modify = qgetenv("HM_PANO_MODIFY");
  const QByteArray old_nona = qgetenv("HM_NONA");
  qputenv("PATH", bin.path().toUtf8() + ":/usr/bin:/bin");
  qputenv("HM_PANO_TRAFO", bin.filePath("pano_trafo").toUtf8());
  qputenv("HM_PANO_MODIFY", bin.filePath("pano_modify").toUtf8());
  qputenv("HM_NONA", bin.filePath("nona").toUtf8());
  qputenv("RINK_PREVIEW_ARGS", bin.filePath("rink-preview-arguments").toUtf8());
  const auto revision = RinkLevelingDialog::sourceRevision(game.path());
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    auto* tabs = dialog.findChild<QTabWidget*>("rinkLevelingTabs");
    auto* next = dialog.findChild<QPushButton*>("nextRinkLevelingButton");
    auto* previous = dialog.findChild<QPushButton*>("previousRinkLevelingButton");
    auto* method = dialog.findChild<QCheckBox*>("selectRinkCornersCheck");
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(
        method && !method->isChecked() && next->text() == "Next" && previous->text() == "Prev" &&
            !dialog.findChild<QPushButton*>("previewRinkLevelingButton") && tabs->currentIndex() == 0,
        "Posts remain the default, with Next and Prev replacing the preview button");
    previous->click();
    ok &= expect(
        waitUntil([&]() { return accept->isEnabled(); }) && tabs->currentIndex() == 2,
        "Prev wraps from Left to Preview and renders the current angles");
    const auto rendered_arguments = read(bin.filePath("rink-preview-arguments"));
    next->click();
    ok &= expect(tabs->currentIndex() == 0, "Next wraps from Preview to Left");
    next->click();
    ok &= expect(tabs->currentIndex() == 1, "Next advances from Left to Right");
    next->click();
    ok &= expect(
        tabs->currentIndex() == 2 && read(bin.filePath("rink-preview-arguments")) == rendered_arguments,
        "Navigation reuses an unchanged preview");
    dialog.findChild<QDoubleSpinBox*>("rinkLevelingPitch")->setValue(-30);
    ok &= expect(!accept->isEnabled(), "Edited angles invalidate preview acceptance");
    previous->click();
    next->click();
    ok &= expect(
        waitUntil([&]() { return accept->isEnabled(); }) &&
            read(bin.filePath("rink-preview-arguments")) != rendered_arguments,
        "Returning to Preview renders changed angles");
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    markPosts(dialog);
    ok &= expect(estimateComplete(dialog), "Post selections are ready before switching methods");
    auto* left = static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera0"));
    auto* right = static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera1"));
    const auto saved_posts = left->points();
    auto* method = dialog.findChild<QCheckBox*>("selectRinkCornersCheck");
    auto* tabs = dialog.findChild<QTabWidget*>("rinkLevelingTabs");
    auto* next = dialog.findChild<QPushButton*>("nextRinkLevelingButton");
    auto* previous = dialog.findChild<QPushButton*>("previousRinkLevelingButton");
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    method->setChecked(true);
    next->click();
    next->click();
    ok &= expect(
        left->points().isEmpty() && right->points().isEmpty() && tabs->currentIndex() == 1 && !accept->isEnabled(),
        "Corner mode has separate marks and cannot preview an incomplete rectangle");
    QByteArray corner_rays;
    for (const auto& point :
         std::vector<std::array<double, 3>>{{8, -12, -5}, {8, 12, -5}, {28, -12, -5}, {28, 12, -5}}) {
      const double radius = std::hypot(std::hypot(point[0], point[1]), point[2]);
      corner_rays += QByteArray::number(1799.5 - std::atan2(point[1], point[0]) * 1800 / pi, 'g', 16) + " " +
          QByteArray::number(899.5 - std::asin(point[2] / radius) * 1800 / pi, 'g', 16) + "\n";
    }
    const auto corner_tool = "cat >/dev/null\nsleep 0.1\ncat <<'RAYS'\n" + corner_rays + "RAYS\n";
    ok &= script(bin.filePath("pano_trafo"), corner_tool);
    left->setPoints({{10, 20}, {70, 80}, {50, 50}});
    right->setPoints({{20, 30}, {80, 70}});
    // Click before the debounce expires: navigation must estimate before rendering.
    next->click();
    ok &= expect(
        left->points().size() == 2 && !next->isEnabled() && !previous->isEnabled() && !method->isEnabled(),
        "Corner mode bounds selections to four points and locks navigation during estimation");
    ok &= expect(
        waitUntil([&]() { return accept->isEnabled(); }) && tabs->currentIndex() == 2 &&
            std::abs(dialog.rotationDegrees()[1]) < 0.001 && std::abs(dialog.rotationDegrees()[2]) < 0.001,
        "Next finishes the pending corner estimate and renders its angles, not the previous post result");
    previous->click();
    ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\nprintf 'invalid rays\\n'\n");
    right->setPoints({{21, 30}, {80, 70}});
    next->click();
    auto* status = dialog.findChild<QLabel*>("rinkLevelingStatus");
    ok &= expect(
        waitUntil([&]() { return next->isEnabled() && status->text().contains("could not be transformed"); }) &&
            !accept->isEnabled() && tabs->currentIndex() == 1,
        "Failed corner estimation must leave stale preview angles unavailable for acceptance");
    ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\ncat <<'RAYS'\n" + transformed + "RAYS\n");
    method->setChecked(false);
    ok &= expect(
        left->points() == saved_posts && estimateComplete(dialog),
        "Switching back restores post marks and estimates with the post method");
    ok &= script(bin.filePath("pano_trafo"), corner_tool);
    method->setChecked(true);
    ok &= expect(
        left->points().size() == 2 && right->points().front() == QPoint(21, 30) && estimateComplete(dialog),
        "Switching again restores the independent corner marks");
    advanceToPreview(dialog);
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "Restored corners can render again");
    dialog.findChild<QPushButton*>("cancelRinkLevelingButton")->click();
    ok &= expect(
        dialog.result() == QDialog::Rejected && RinkLevelingDialog::sourceRevision(game.path()) == revision,
        "Cancel discards corner leveling and preserves the game snapshot");
    ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\ncat <<'RAYS'\n" + transformed + "RAYS\n");
  }
  {
    const auto producer_lock = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
    if (!expect(producer_lock.ok() && *producer_lock, "producer holds the artifact lock before opening"))
      return 1;
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    ok &= expect(
        !dialog.loadError().isEmpty() && !dialog.findChild<QPushButton*>("nextRinkLevelingButton")->isEnabled(),
        "opening must reject artifact-lock contention even before any source files change");
  }
  {
    ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\nsleep 0.3\ncat <<'RAYS'\n" + transformed + "RAYS\n");
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    markPosts(dialog);
    auto* clear = dialog.findChild<QPushButton*>("rinkLevelingCamera0Clear");
    auto* undo = dialog.findChild<QPushButton*>("rinkLevelingCamera0Undopoint");
    ok &= expect(waitUntil([&]() { return !clear->isEnabled(); }), "automatic estimate starts after its debounce");
    ok &= expect(
        clear && undo && !clear->isEnabled() && !undo->isEnabled(), "point actions are disabled while estimating");
    clear->click();
    undo->click();
    ok &= expect(estimateComplete(dialog), "delayed automatic estimate completes");
    auto* canvas = static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera0"));
    ok &= expect(
        canvas->points().size() == 4 && clear->isEnabled(),
        "disabled mutation buttons preserve the estimated selection");
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    dialog.show();
    markPosts(dialog);
    QApplication::processEvents();
    auto* canvas = static_cast<ScoreboardSelectionCanvas*>(dialog.findChild<QWidget*>("rinkLevelingCamera0"));
    canvas->fitImage();
    const double scale = canvas->viewScale();
    const QPointF offset(
        (canvas->width() - canvas->imageSize().width() * scale) / 2.0,
        (canvas->height() - canvas->imageSize().height() * scale) / 2.0);
    const QPoint press = ((QPointF(10.5, 10.5) * scale) + offset).toPoint();
    const QPoint moved = press + QPoint(20, 0);
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(canvas, moved, 10);
    QTest::qWait(250);
    ok &= expect(
        canvas->isEnabled() && canvas->pointerInteractionActive(),
        "automatic estimate waits for a paused point drag to finish");
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, moved);
    const QVector<QPoint> released_points = canvas->points();
    ok &= expect(
        waitUntil([canvas]() { return !canvas->isEnabled(); }),
        "automatic estimate starts after the dragged point is released");
    ok &= expect(estimateComplete(dialog), "post-drag automatic estimate completes");
    QTest::mouseMove(canvas, moved + QPoint(20, 0), 10);
    ok &= expect(
        !canvas->pointerInteractionActive() && canvas->points() == released_points,
        "hover after automatic estimation does not continue the completed drag");
  }
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
    ok &= expect(estimateComplete(dialog), "automatic estimate completes");
    ok &= expect(
        std::abs(dialog.rotationDegrees()[1]) < 0.001 && std::abs(dialog.rotationDegrees()[2]) < 0.001,
        "selected poles set absolute level");
    dialog.findChild<QDoubleSpinBox*>("rinkLevelingPitch")->setValue(-31.5);
    advanceToPreview(dialog);
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
    advanceToPreview(dialog);
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "manual preview completes");
    write(game.filePath("autooptimiser_out.pto"), pto + "# changed generation\n");
    accept->click();
    ok &= expect(dialog.result() != QDialog::Accepted && !accept->isEnabled(), "reject stale selection");
    write(game.filePath("autooptimiser_out.pto"), pto);
  }
  {
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    advanceToPreview(dialog);
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "repeat preview completes");
    {
      const auto producer_lock = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
      if (!expect(producer_lock.ok() && *producer_lock, "producer holds the artifact lock before acceptance"))
        return 1;
      accept->click();
      ok &= expect(
          dialog.result() != QDialog::Accepted && RinkLevelingDialog::sourceRevision(game.path()) == revision,
          "acceptance must reject artifact-lock contention even when snapshot hashes still match");
    }
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
    advanceToPreview(dialog);
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
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    advanceToPreview(dialog);
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "preview before config change completes");
    write(game.filePath("config.yaml"), config + "hstream_ui:\n  stitching_calibration:\n    status: pending\n");
    accept->click();
    ok &= expect(dialog.result() != QDialog::Accepted, "changed calibration settings invalidate an open estimate");
    RinkLevelingDialog pending(game.path(), {0, -33, 2});
    ok &= expect(!pending.loadError().isEmpty(), "saved but stale calibration cannot be used for selecting posts");
    write(game.filePath("config.yaml"), config);
    hm::stitching::StitchCameraSelection changed_camera;
    changed_camera.configuration = "test-camera";
    changed_camera.horizontal_fov = 90;
    changed_camera.vertical_fov = 90;
    RinkLevelingDialog matching(game.path(), {0, -33, 2}, nullptr, changed_camera);
    ok &= expect(matching.loadError().isEmpty(), "matching camera metadata allows leveling");
    changed_camera.configuration = "different-camera";
    RinkLevelingDialog mismatched(game.path(), {0, -33, 2}, nullptr, changed_camera);
    ok &= expect(!mismatched.loadError().isEmpty(), "published camera metadata must match the selected camera model");
    QByteArray legacy = provenance;
    legacy.replace("version=8", "version=6");
    write(game.filePath("stitching_canvas_provenance"), legacy);
    RinkLevelingDialog unverifiable(game.path(), {0, -33, 2}, nullptr, changed_camera);
    ok &= expect(!unverifiable.loadError().isEmpty(), "desktop selection requires verifiable camera metadata");
    write(game.filePath("stitching_canvas_provenance"), provenance);
  }
  {
    QTemporaryDir staging;
    ok &= staging.isValid() && writeInProgressSnapshot(staging, source, pto);
    const std::array<double, 3> rotation{7, -33, 2};
    RinkLevelingDialog dialog(
        staging.path(),
        rotation,
        nullptr,
        std::nullopt,
        true,
        hm::stitching::StitchProjection::kRectilinear,
        {},
        previewFraming(rotation));
    auto* skip = dialog.findChild<QPushButton*>("skipRinkLevelingButton");
    auto* cancel_calibration = dialog.findChild<QPushButton*>("cancelRinkCalibrationButton");
    auto* preview = dialog.findChild<QPushButton*>("nextRinkLevelingButton");
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(
        dialog.loadError().isEmpty() && skip && cancel_calibration && preview && accept &&
            !dialog.findChild<QPushButton*>("cancelRinkLevelingButton") &&
            !dialog.findChild<QPushButton*>("estimateRinkLevelingButton"),
        "in-progress selection loads the aligned snapshot and offers Skip and whole-calibration cancellation");
    dialog.show();
    markPosts(dialog);
    ok &= expect(estimateComplete(dialog), "in-progress post edits automatically update the estimated angles");
    ok &= expect(!accept->isEnabled(), "in-progress angles cannot be used before an explicit preview");
    advanceToPreview(dialog);
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "in-progress preview enables Use angles");
    const QByteArray preview_arguments = read(bin.filePath("rink-preview-arguments"));
    ok &= expect(
        preview_arguments.contains("--projection=0\n") && preview_arguments.contains("--fov=AUTO\n") &&
            preview_arguments.contains("--canvas=AUTO\n") && preview_arguments.contains("--crop=AUTO\n") &&
            preview_arguments.contains(".autooptimiser_out.aligned.pto\n") && preview_arguments.contains("LC_ALL=C\n"),
        "in-progress preview shares final projection framing, executable overrides, and C locale before downscaling");
    accept->click();
    ok &= expect(
        dialog.result() == QDialog::Accepted && dialog.rotationDegrees()[0] == 7,
        "in-progress Use angles returns absolute pitch and roll while preserving yaw");
  }
  {
    QTemporaryDir staging;
    ok &= staging.isValid() && writeInProgressSnapshot(staging, source, pto) &&
        script(bin.filePath("pano_trafo"), "cat >/dev/null\nsleep 30\n");
    const std::array<double, 3> rotation{0, -33, 2};
    RinkLevelingDialog dialog(
        staging.path(),
        rotation,
        nullptr,
        std::nullopt,
        true,
        hm::stitching::StitchProjection::kRectilinear,
        {},
        previewFraming(rotation));
    dialog.show();
    markPosts(dialog);
    auto* skip = dialog.findChild<QPushButton*>("skipRinkLevelingButton");
    ok &= expect(
        waitUntil([&]() {
          return skip && skip->isEnabled() && !dialog.findChild<QWidget*>("rinkLevelingCamera0")->isEnabled();
        }),
        "Skip leveling stays available while an automatic estimate is running");
    QElapsedTimer elapsed;
    elapsed.start();
    skip->click();
    ok &= expect(
        elapsed.elapsed() < 4000 && dialog.result() == QDialog::Rejected,
        "Skip leveling promptly cancels an active estimator");
    ok &= script(bin.filePath("pano_trafo"), "cat >/dev/null\ncat <<'RAYS'\n" + transformed + "RAYS\n");
  }
  {
    QTemporaryDir staging;
    ok &= staging.isValid() && writeInProgressSnapshot(staging, source, pto);
    const std::array<double, 3> rotation{0, -33, 2};
    RinkLevelingDialog dialog(
        staging.path(),
        rotation,
        nullptr,
        std::nullopt,
        true,
        hm::stitching::StitchProjection::kRectilinear,
        {},
        previewFraming(rotation));
    dialog.show();
    advanceToPreview(dialog);
    auto* accept = dialog.findChild<QPushButton*>("acceptRinkLevelingButton");
    ok &= expect(waitUntil([&]() { return accept->isEnabled(); }), "in-progress manual-angle preview completes");
    write(staging.filePath("autooptimiser_out.pto"), pto + "# changed pending generation\n");
    accept->click();
    ok &= expect(
        dialog.result() != QDialog::Accepted && !accept->isEnabled(),
        "Use angles rejects an in-progress calibration generation that changed after preview");
  }
  {
    QTemporaryDir staging;
    ok &= staging.isValid() && writeInProgressSnapshot(staging, source, pto);
    const std::array<double, 3> rotation{0, -33, 2};
    RinkLevelingDialog dialog(
        staging.path(),
        rotation,
        nullptr,
        std::nullopt,
        true,
        hm::stitching::StitchProjection::kRectilinear,
        {},
        previewFraming(rotation));
    dialog.findChild<QPushButton*>("cancelRinkCalibrationButton")->click();
    ok &= expect(
        dialog.result() == QDialog::Rejected && dialog.calibrationCancellationRequested(),
        "Cancel calibration is distinct from Skip leveling");
  }
  {
    QTemporaryDir staging;
    ok &= staging.isValid() && writeInProgressSnapshot(staging, source, pto);
    const std::array<double, 3> rotation{0, -33, 2};
    RinkLevelingDialog dialog(
        staging.path(),
        rotation,
        nullptr,
        std::nullopt,
        true,
        hm::stitching::StitchProjection::kRectilinear,
        {},
        previewFraming(rotation));
    dialog.closeAfterBackendCompletion();
    ok &= expect(
        dialog.result() == QDialog::Rejected && dialog.closedAfterBackendCompletion() &&
            !dialog.calibrationCancellationRequested(),
        "backend completion closes an open selector without turning it into Skip or user cancellation");
  }
  {
    QImage mismatch(20, 20, QImage::Format_RGB32);
    mismatch.fill(Qt::black);
    mismatch.save(game.filePath("right.png"));
    RinkLevelingDialog dialog(game.path(), {0, -33, 2});
    ok &= expect(
        !dialog.loadError().isEmpty() && !dialog.findChild<QPushButton*>("nextRinkLevelingButton")->isEnabled(),
        "mismatched camera dimensions fail closed");
  }
  qunsetenv("RINK_PREVIEW_ARGS");
  if (old_pano_trafo.isNull())
    qunsetenv("HM_PANO_TRAFO");
  else
    qputenv("HM_PANO_TRAFO", old_pano_trafo);
  if (old_pano_modify.isNull())
    qunsetenv("HM_PANO_MODIFY");
  else
    qputenv("HM_PANO_MODIFY", old_pano_modify);
  if (old_nona.isNull())
    qunsetenv("HM_NONA");
  else
    qputenv("HM_NONA", old_nona);
  qputenv("PATH", old_path);
  return ok ? 0 : 1;
}
