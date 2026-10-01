#include "src/apps/hstream-ui/StitchingStillPreview.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

#include <tiffio.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
bool wait_for(const std::function<bool()>& ready) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 10000) {
    QApplication::processEvents();
    if (ready())
      return true;
    QThread::msleep(10);
  }
  return false;
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot write fixture");
}
void tiff_fixture(const QString& path) {
  TIFF* file = TIFFOpen(QFile::encodeName(path).constData(), "w");
  require(file, "Cannot write TIFF fixture");
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 4096);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 64);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 16);
  std::vector<uint16_t> row(4096 * 3, 0);
  for (int x = 0; x < 4096; ++x)
    row[3 * x + 2] = 65535;
  for (int y = 0; y < 64; ++y)
    require(TIFFWriteScanline(file, row.data(), y) >= 0, "Cannot write TIFF row");
  TIFFClose(file);
}
void exercise() {
  QTemporaryDir game;
  const QByteArray complete = "hstream_ui:\n  stitching_calibration:\n    status: complete\n";
  write(game.filePath("config.yaml"), complete);
  QImage png(4096, 64, QImage::Format_RGB32);
  png.fill(Qt::red);
  require(png.save(game.filePath("s.png")), "Cannot save PNG");
  tiff_fixture(game.filePath("panorama.tif"));
  const auto proxy = readCalibrationTiff(game.filePath("panorama.tif"), 2048);
  require(
      proxy.size() == QSize(2048, 32) && proxy.pixelColor(0, 0) == QColor(Qt::blue),
      "16-bit TIFF must become a bounded color-correct preview");
  const auto png_proxy = readCalibrationPng(game.filePath("s.png"), QSize(2048, 32));
  require(
      png_proxy.size() == QSize(2048, 32) && png_proxy.pixelColor(0, 0) == QColor(Qt::red),
      "PNG must become a bounded color-correct preview");
  QWidget host;
  host.resize(800, 400);
  host.show();
  StitchingStillPreview preview(&host);
  preview.setSource(game.path());
  require(wait_for([&] { return preview.isVisible(); }), "Complete calibration must display a still");
  require(preview.property("calibrationImage").toString() == "s.png", "Prefer PNG over TIFF");
  host.resize(900, 500);
  QApplication::processEvents();
  require(preview.size() == host.size(), "Still must follow preview area resizing");
  write(game.filePath("config.yaml"), complete + "    stale_from: canvas\n");
  require(wait_for([&] { return preview.isHidden(); }), "Invalidated calibration must clear the still");
  write(game.filePath("config.yaml"), complete);
  require(wait_for([&] { return preview.isVisible(); }), "New complete calibration must restore the still");
  QFile::remove(game.filePath("s.png"));
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "Missing PNG must fall back to TIFF");
  write(game.filePath("s.png"), "broken image");
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "Corrupt PNG must fall back to TIFF");
  preview.setSource({});
  require(preview.isHidden() && !preview.property("calibrationImage").isValid(), "Play must clear immediately");
  // Clear a request before its completion callback can paint anything.
  preview.setSource(game.path());
  preview.setSource({});
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Late decode must not reappear after Play");
  preview.setSource(game.path(), "a different artifact generation");
  timer.restart();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Changed candidate generation must not display a still");
  preview.setSource(game.path());
  require(wait_for([&] { return preview.isVisible(); }), "Stop must restore the selected still");
  write(game.filePath("config.yaml"), "invalid: [yaml");
  require(wait_for([&] { return preview.isHidden(); }), "Malformed settings must fail closed");
  auto config = YAML::Load(complete.toStdString());
  hm::stitching::StitchingBackendChoices choices;
  choices.control_point_matcher = "superpoint-lightglue";
  choices.mapping_backend = "nona";
  choices.projection = "cylindrical";
  choices.run_autooptimizer = true;
  config["stitching"]["control_point_matcher"] = choices.control_point_matcher;
  config["stitching"]["mapping_backend"] = choices.mapping_backend;
  config["stitching"]["projection"] = choices.projection;
  config["stitching"]["run_autooptimizer"] = choices.run_autooptimizer;
  hm::stitching::write_stitch_camera_selection(config, choices.camera);
  hm::stitching::write_stitch_projection_framing(config, choices.projection_framing);
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "preview-generation";
  require(
      hm::stitching::reserve_stitching_backend_generation_in_config(config, "preview-generation", choices).ok(),
      "Cannot prepare generation claim");
  const auto claimed = QByteArray::fromStdString(YAML::Dump(config));
  write(game.filePath("config.yaml"), claimed);
  require(wait_for([&] { return preview.isVisible(); }), "Matching generation claim must allow the still");
  config["stitching"]["projection"] = "rectilinear";
  write(game.filePath("config.yaml"), QByteArray::fromStdString(YAML::Dump(config)));
  require(wait_for([&] { return preview.isHidden(); }), "Editing settings must invalidate an old generation claim");
  write(game.filePath("config.yaml"), claimed);
  preview.setSource(game.path(), {}, "stitching: {projection: rectilinear}");
  timer.restart();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Changed effective UI defaults must reject an old saved calibration");
  preview.setSource(game.path(), {}, "stitching: {projection: cylindrical}");
  require(wait_for([&] { return preview.isVisible(); }), "Matching effective UI choices must restore the still");
  // A completed canvas is usable before the downstream PNG/mask has been rebuilt.
  config = YAML::Load(claimed.toStdString());
  config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  require(png.save(game.filePath("s.png")), "Cannot restore PNG fixture");
  write(game.filePath("config.yaml"), QByteArray::fromStdString(YAML::Dump(config)));
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "A completed solve must skip its invalidated PNG and use the new panorama");
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    exercise();
    if (argc == 2) {
      QWidget host;
      host.resize(1200, 500);
      host.show();
      StitchingStillPreview preview(&host);
      preview.setSource(QString::fromLocal8Bit(argv[1]));
      require(wait_for([&] { return preview.isVisible(); }), "Real saved calibration must load");
      require(preview.grab().save("/tmp/hstream-stitching-still.png"), "Cannot save preview capture");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
