#include <tiffio.h>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <fstream>
#include <iostream>
#include "hstream/src/libs/stitching/RinkMap.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void placement(const fs::path& path, float left) {
  TIFF* file = TIFFOpen(path.c_str(), "w");
  require(file, "Cannot create TIFF fixture");
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
  for (uint32_t y = 0; y < 1200; ++y)
    require(TIFFWriteScanline(file, row.data(), y, 0) >= 0, "Cannot write TIFF fixture");
  TIFFClose(file);
}
QJsonObject invoke(
    const QString& binary,
    const fs::path& root,
    const char* operation,
    const QJsonObject& payload,
    bool success = true) {
  fs::remove(root / "operation-result.json");
  hm::web::WriteObject(
      root / "operation-request.json",
      {{"schema", 1},
       {"run_id", "rink-map-contract"},
       {"operation", operation},
       {"root", QString::fromStdString(root.string())},
       {"game_dir", "game"},
       {"payload", payload}});
  QProcess process;
  process.start(binary, {"--request", QString::fromStdString((root / "operation-request.json").string())});
  require(process.waitForFinished(30000), "Rink adapter timed out");
  if ((process.exitCode() == 0) != success) {
    std::cerr << process.readAllStandardError().constData();
    throw std::runtime_error("Unexpected rink-map adapter outcome");
  }
  return hm::web::ReadObject(root / "operation-result.json").value("result").toObject();
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    require(argc == 2, "Pass native adapter binary");
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::create_directories(game);
    QImage image(3600, 1800, QImage::Format_RGB32);
    image.fill(Qt::gray);
    for (const char* name : {"left.png", "right.png"})
      require(image.save(QString::fromStdString((game / name).string())), "Cannot save image");
    const std::string pto =
        "p f2 w3600 h1800 v360 n\"PNG\"\n"
        "i w3600 h1800 f4 v360 y0 p0 r0 n\"left.png\"\n"
        "i w3600 h1800 f4 v360 y0 p0 r0 n\"right.png\"\n";
    for (const char* name : {"autooptimiser_out.pto", "hm_project.pto"})
      std::ofstream(game / name) << pto;
    for (const char* name : {"mapping_0000.tif", "mapping_0000_x.tif", "mapping_0000_y.tif"})
      placement(game / name, 900);
    for (const char* name : {"mapping_0001.tif", "mapping_0001_x.tif", "mapping_0001_y.tif"})
      placement(game / name, 1800);
    std::ofstream(game / "seam_file.png") << "fixture";
    std::ofstream(game / "config.yaml") << "hstream_ui:\n  stitching_calibration:\n    status: complete\n"
                                           "stitching:\n  projection_framing:\n    rotation_degrees: [0, 0, 0]\n";
    std::ofstream(game / "stitching_canvas_provenance")
        << "version=3\nmax-output-width=1800\nmax-canvas-dimension=0\nsource-canvas-width=7200\nsource-canvas-height=3600\n"
           "canvas-width=1800\ncanvas-height=1200\nmax-output-width-applied=1\nmax-canvas-dimension-applied=0\n"
           "mapping-backend=nona\nprojection=equirectangular\n";
    const auto opened = invoke(argv[1], root, "rink-map-read", {});
    require(opened.value("fit").toObject().value("state") == "partial", "Empty map must stay partial");
    auto document = opened.value("document").toObject();
    require(
        document.value("dimensions_m").toArray()[3].toDouble() == 3.3528,
        "Rink defaults must preserve baseline precision");
    QJsonArray observations;
    const auto dims = document.value("dimensions_m").toArray();
    const hm::stitching::RinkLayout layout{
        dims[0].toDouble(), dims[1].toDouble(), dims[2].toDouble(), dims[3].toDouble(), dims[4].toDouble()};
    for (const char* feature : {"goal-left-near", "goal-left-far", "goal-right-near", "goal-right-far"}) {
      const auto point = hm::stitching::RinkFeaturePoint(layout, feature);
      require(point.ok(), "Expected shared rink feature");
      cv::Vec3d ray(point->x - 3, point->y + 19, -7);
      ray /= cv::norm(ray);
      constexpr double pi = 3.14159265358979323846;
      observations.append(
          QJsonObject{
              {"feature", feature},
              {"label", feature},
              {"camera", 0},
              {"line", false},
              {"arc", false},
              {"automatic", false},
              {"enabled", true},
              {"confidence", 1},
              {"mask_revision", ""},
              {"pixels",
               QJsonArray{QJsonArray{
                   1799.5 - std::atan2(ray[1], ray[0]) * 1800 / pi, 899.5 - std::asin(ray[2]) * 1800 / pi}}}});
    }
    document.insert("observations", observations);
    const QJsonObject draft{{"snapshot", opened.value("snapshot")}, {"document", document}};
    const auto fitted = invoke(argv[1], root, "rink-map-update", draft);
    require(
        fitted.value("fit").toObject().value("state") == "ready",
        "Four exact corners must fit through actual pano_trafo");
    require(!fs::exists(game / "rink-map.yaml"), "Updating must not save a draft");
    const auto saved = invoke(argv[1], root, "rink-map-save", draft);
    require(
        saved.value("saved").toBool() && fs::exists(game / "rink-map.yaml"),
        "Save must publish reviewed native observations");
    invoke(argv[1], root, "rink-map-save", draft, false);
    auto reopened = invoke(argv[1], root, "rink-map-read", {});
    require(
        reopened.value("document").toObject().value("observations").toArray().size() == 4,
        "Read must retain exact saved marks");
    auto wrong = reopened.value("document").toObject();
    wrong.insert("rays", QJsonArray{});
    invoke(argv[1], root, "rink-map-update", {{"snapshot", reopened.value("snapshot")}, {"document", wrong}}, false);
    std::ofstream(game / "autooptimiser_out.pto", std::ios::app) << "# changed\n";
    invoke(
        argv[1],
        root,
        "rink-map-save",
        {{"snapshot", reopened.value("snapshot")}, {"document", reopened.value("document")}},
        false);
    std::cout << "Web rink map real-Hugin tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
