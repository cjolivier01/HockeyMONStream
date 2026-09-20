#include "src/apps/hstream-ui/ProjectionCropDialog.h"
#include "src/apps/hstream-ui/RinkLevelingDialog.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

QByteArray read(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& bytes, bool executable = false) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
      (!executable || file.setPermissions(file.permissions() | QFileDevice::ExeOwner));
}

bool wait_for_preview(ProjectionCropDialog* dialog) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 30000) {
    QApplication::processEvents();
    if (!dialog->sourceRevision().isEmpty())
      return true;
    const auto* canvas = dialog->findChild<QWidget*>("projectionCropCanvas");
    if (canvas && canvas->accessibleDescription().contains("unavailable"))
      break;
    QThread::msleep(10);
  }
  const auto* status = dialog->findChild<QLabel*>("projectionCropStatus");
  std::cerr << "Crop preview failed: " << (status ? status->text().toStdString() : "missing status") << '\n';
  return false;
}

bool test_snapshot_paths() {
  QTemporaryDir game(QDir::tempPath() + "/hstream preview images-XXXXXX");
  QTemporaryDir tools;
  QImage image(800, 400, QImage::Format_RGB32);
  image.fill(Qt::gray);
  if (!expect(
          game.isValid() && tools.isValid() && image.save(game.filePath("left.png")) &&
              image.save(game.filePath("right.png")),
          "Cannot create camera image fixtures"))
    return false;
  const QByteArray metadata =
      "version=9\ncontrol-point-resolution=native\nmapping-backend=nona\nprojection=cylindrical\nprojection-parameters=none\n"
      "projection-auto-fov=0\nprojection-auto-canvas=1\nprojection-horizontal-fov=180\n"
      "projection-rotation-0=0\nprojection-rotation-1=0\nprojection-rotation-2=0\n"
      "camera-configuration=gopro-hero-11\ncamera-horizontal-fov=108\ncamera-vertical-fov=90\n";
  if (!expect(
          write(game.filePath("config.yaml"), "hstream_ui:\n  stitching_calibration:\n    status: complete\n") &&
              write(game.filePath("stitching_canvas_provenance"), metadata) &&
              write(
                  tools.filePath("pano_modify"),
                  "#!/bin/sh\ncase \"$*\" in\n*--crop=AUTO*) cp full.pto auto.pto;;\n"
                  "*) cp autooptimiser_out.pto full.pto;;\nesac\n",
                  true) &&
              write(
                  tools.filePath("nona"),
                  "#!/bin/sh\nif grep -q 'n\"/' full.pto; then exit 9; fi\ncp left.png full.png\n",
                  true),
          "Cannot create preview tool fixtures"))
    return false;
  const QByteArray original_path = qgetenv("PATH");
  qputenv("PATH", tools.path().toUtf8() + ":/usr/bin:/bin");
  bool ok = true;
  const hm::stitching::StitchCameraSelection camera{"gopro-hero-11", 108, 90};
  for (bool absolute : {false, true}) {
    const QByteArray prefix = absolute ? game.path().toUtf8() + '/' : QByteArray();
    const QByteArray project =
        "p f1 w800 h400 v180 S0,800,120,360 n\"PNG\"\n"
        "i w800 h400 f0 v90 y-30 p0 r0 n\"" +
        prefix +
        "left.png\"\n"
        "i w800 h400 f0 v90 y30 p0 r0 n\"" +
        prefix + "right.png\"\n";
    ok &= write(game.filePath("autooptimiser_out.pto"), project);
    ok &= write(game.filePath(".autooptimiser_out.aligned.pto"), project);
    for (bool in_progress : {false, true}) {
      ProjectionCropDialog crop(game.path(), {}, "cylindrical", {}, camera, {}, nullptr, in_progress);
      ok &= expect(wait_for_preview(&crop), "Relative and absolute projects must render saved and in-progress crops");
      RinkLevelingDialog leveling(
          game.path(), {}, nullptr, camera, in_progress, hm::stitching::StitchProjection::kCylindrical);
      ok &= expect(leveling.loadError().isEmpty(), "Relative and absolute projects must load for rink leveling");
      ok &= expect(
          read(game.filePath("autooptimiser_out.pto")) == project &&
              read(game.filePath(".autooptimiser_out.aligned.pto")) == project,
          "Preview preparation must preserve the published calibration projects");
    }
  }
  qputenv("PATH", original_path);
  return ok;
}

// Optional read-only smoke against a real game, using installed Hugin tools.
bool test_saved_game(const QString& directory) {
  const auto config = YAML::Load(read(QDir(directory).filePath("config.yaml")).toStdString());
  const auto framing = hm::stitching::read_stitch_projection_framing(config);
  const auto camera = hm::stitching::read_stitch_camera_selection(config);
  const std::string projection_name = config["stitching"]["projection"].as<std::string>();
  const auto projection = hm::stitching::ParseStitchProjection(projection_name);
  if (!framing.ok() || !camera.ok() || !projection.ok())
    return false;
  const auto parameters = hm::stitching::read_stitch_projection_parameters(config, *projection);
  if (!parameters.ok())
    return false;
  const auto revision = RinkLevelingDialog::sourceRevision(directory);
  ProjectionCropDialog crop(directory, *framing, QString::fromStdString(projection_name), *parameters, *camera);
  const bool ready = wait_for_preview(&crop);
  RinkLevelingDialog leveling(directory, framing->rotation_degrees, nullptr, *camera);
  if (!leveling.loadError().isEmpty())
    std::cerr << "Leveling preview failed: " << leveling.loadError().toStdString() << '\n';
  if (RinkLevelingDialog::sourceRevision(directory) != revision)
    std::cerr << "Source calibration changed while opening previews\n";
  return expect(
      ready && leveling.loadError().isEmpty() && RinkLevelingDialog::sourceRevision(directory) == revision,
      "Real-game crop and leveling previews must load without changing source artifacts");
}

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  return (argc == 2 ? test_saved_game(QString::fromLocal8Bit(argv[1])) : test_snapshot_paths()) ? 0 : 1;
}
