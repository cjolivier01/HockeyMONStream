#include <opencv2/imgcodecs.hpp>
#include <sys/stat.h>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <fstream>
#include <iostream>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/WebJobSpec.h"
namespace fs = std::filesystem;
namespace {
using namespace hm::stitching;
void require(const absl::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}
template <typename T>
T value(absl::StatusOr<T> result) {
  require(result.status());
  return std::move(*result);
}
void write(const fs::path& path, const std::string& bytes) {
  std::ofstream(path) << bytes;
}
StitchingBackendChoices choices(const YAML::Node& config) {
  StitchingBackendChoices result;
  result.control_point_matcher = ControlPointMatcherName(value(resolve_control_point_matcher(config)));
  result.mapping_backend = "nona";
  result.projection = config["stitching"]["projection"].as<std::string>();
  result.run_autooptimizer = true;
  result.projection_parameters =
      value(read_stitch_projection_parameters(config, value(ParseStitchProjection(result.projection))));
  result.projection_framing = value(read_stitch_projection_framing(config));
  result.camera = value(read_stitch_camera_selection(config));
  result.control_point_resolution = value(read_control_point_resolution(config));
  result.control_point_filtering = value(read_control_point_filtering(config));
  result.control_point_execution_provider = value(read_control_point_execution_provider(config));
  result.calibration_frame_selection_fingerprint = value(player_frame_selection_fingerprint(config));
  result.manual_control_point_fingerprint = value(manual_control_point_fingerprint(config));
  return result;
}

YAML::Node fixture(const fs::path& root) {
  fs::create_directories(root);
  auto config = YAML::Clone(value(hm::baseline_config::load()).values);
  config["stitching"]["calibration_frame_count"] = 2;
  // Pin the matcher so this fixture keeps exercising a resolution-bearing
  // neural backend rather than following the shipped baseline default.
  config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  config["stitching"]["control_point_resolution"] = "native";
  write_control_point_filtering(config, {false, false});
  config["stitching"]["control_point_execution_provider"] = "cpu";
  config["stitching"]["max_output_width"] = 0;
  config["stitching"]["max_output_dimension"] = 0;
  config["stitching"]["rink_config"] = "";
  StitchProjectionFraming framing;
  framing.rotation_degrees = {0, -15, 3};
  write_stitch_projection_framing(config, framing);
  write_stitch_camera_selection(config, {"gopro-mission-1", 109, 64});
  auto state = config["hstream_ui"]["stitching_calibration"];
  state["status"] = "complete";
  state["invalidation_id"] = "completed-solve";
  state["control_points"] = 1500;
  state["frame_count"] = 2;
  require(reserve_stitching_backend_generation_in_config(config, "completed-solve", choices(config)));
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    write(
        root / name,
        "p f19 w64 h32 v180\ni w512 h256 f0 v109 r0 p0 y0 n\"left.png\"\ni w512 h256 f0 v109 r0 p0 y45 n\"right.png\"\nc n0 N1 x1 y2 X3 Y4 t0\n");
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    if (!cv::imwrite((root / name).string(), cv::Mat(32, 64, CV_16UC1, cv::Scalar(1))))
      throw std::runtime_error("Cannot create TIFF fixture");
  for (const char* name : {"left.png", "right.png", "seam_file.png"})
    if (!cv::imwrite((root / name).string(), cv::Mat(256, 512, CV_8UC3, cv::Scalar(1, 2, 3))))
      throw std::runtime_error("Cannot create PNG fixture");
  return config;
}

void provenance(
    const fs::path& root,
    const std::string& fingerprint = {},
    const std::string& diagnostics = {},
    StitchProjection projection = StitchProjection::kGeneralPanini,
    double horizontal_fov = 180) {
  const auto parameters = FormatStitchProjectionParameters(DefaultStitchProjectionParameters(projection));
  write(
      root / "stitching_canvas_provenance",
      "version=10\nmax-output-width=0\nmax-canvas-dimension=0\nsource-canvas-width=64\nsource-canvas-height=32\n"
      "canvas-width=64\ncanvas-height=32\nmax-output-width-applied=0\nmax-canvas-dimension-applied=0\n"
      "mapping-backend=nona\nprojection=" +
          std::string(StitchProjectionName(projection)) +
          "\nprojection-parameters=" + (parameters.empty() ? "none" : parameters) +
          "\nprojection-auto-fov=0\n"
          "projection-horizontal-fov=" +
          std::to_string(horizontal_fov) +
          "\nprojection-auto-canvas=1\nprojection-auto-crop=0\n"
          "camera-configuration=gopro-mission-1\ncamera-horizontal-fov=109\ncamera-vertical-fov=64\n"
          "control-point-matcher=superpoint-lightglue\nakaze-calibration-fingerprint=not-applicable\n"
          "projection-rotation-0=0\nprojection-rotation-1=-15\nprojection-rotation-2=3\n"
          "projection-crop-0=0\nprojection-crop-1=1\nprojection-crop-2=0\nprojection-crop-3=1\n"
          "control-point-resolution=native\ncalibration-frame-selection=" +
          (fingerprint.empty() ? "none" : fingerprint) +
          "\ncalibration-frame-diagnostics=" + (diagnostics.empty() ? "none" : diagnostics) + "\n");
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
       {"run_id", "saved-view-contract"},
       {"operation", operation},
       {"root", QString::fromStdString(root.string())},
       {"game_dir", "game"},
       {"payload", payload}});
  QProcess child;
  child.start(binary, {"--request", QString::fromStdString((root / "operation-request.json").string())});
  if (!child.waitForFinished(120000)) {
    child.kill();
    throw std::runtime_error("Saved view timed out");
  }
  if ((child.exitCode() == 0) != success) {
    std::cerr << child.readAllStandardOutput().constData() << child.readAllStandardError().constData();
    throw std::runtime_error("Unexpected saved view outcome");
  }
  const auto receipt = hm::web::ReadObject(root / "operation-result.json");
  if (success) {
    for (const auto& item : receipt.value("artifacts").toArray()) {
      const auto artifact = item.toObject();
      struct stat info{};
      const auto path = root / artifact.value("path").toString().toStdString();
      if (::lstat(path.c_str(), &info) != 0 || artifact.value("inode").toString().toULongLong() != info.st_ino ||
          artifact.value("size").toString().toLongLong() != info.st_size ||
          artifact.value("mtime_ns").toString().toLongLong() !=
              qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec)
        throw std::runtime_error("Saved-view receipt must pin final artifact identities");
    }
  }
  return receipt.value("result").toObject();
}
void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    expect(argc == 2, "Pass adapter binary");
    // Uniform synthetic images do not always support Enblend seam optimization.
    // Exercise the explicitly enabled native hard-seam path for these tiny stills.
    qputenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1");
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    auto config = fixture(game);
    QProcess normalize;
    normalize.setWorkingDirectory(QString::fromStdString(game.string()));
    normalize.start("pano_modify", {"--rotate=0,-15,3", "--output=canonical.pto", "autooptimiser_out.pto"});
    expect(normalize.waitForFinished(30000) && normalize.exitCode() == 0, "Cannot prepare canonical real-Hugin source");
    fs::copy_file(game / "canonical.pto", game / "autooptimiser_out.pto", fs::copy_options::overwrite_existing);
    fs::copy_file(game / "canonical.pto", game / "hm_project.pto", fs::copy_options::overwrite_existing);
    provenance(game);
    write(game / "config.yaml", YAML::Dump(config));
    const auto opened = invoke(argv[1], root, "calibration-view-read", {{"kind", "leveling"}});
    const auto snap = opened.value("snapshot");
    expect(opened.value("image_sizes").toArray().size() == 2, "Original images must be exposed");
    QJsonArray cameras;
    for (int camera = 0; camera < 2; ++camera) {
      QJsonArray points;
      for (int x : {128, 256, 384}) {
        points.append(QJsonArray{x, 64});
        points.append(QJsonArray{x, 192});
      }
      cameras.append(points);
    }
    const auto estimate = invoke(
        argv[1],
        root,
        "calibration-view-estimate",
        {{"kind", "leveling"},
         {"snapshot", snap},
         {"angles", QJsonArray{0, 0, 0}},
         {"method", "posts"},
         {"points", cameras}});
    const auto recovered = estimate.value("angles").toArray();
    expect(
        recovered.size() == 3 && std::abs(recovered[1].toDouble()) < 1e-4 && std::abs(recovered[2].toDouble()) < 1e-4,
        "Saved estimator must remove the published rotation before fitting original-image posts");
    invoke(
        argv[1],
        root,
        "calibration-view-landmarks",
        {{"kind", "leveling"}, {"snapshot", snap}, {"angles", QJsonArray{0, 0, 0}}},
        false);
    const QJsonArray angles{0, -20, 4};
    const QJsonObject selected{{"kind", "leveling"}, {"snapshot", snap}, {"angles", angles}};
    auto preview = invoke(argv[1], root, "calibration-view-preview", selected);
    expect(
        preview.value("angles") == angles && fs::is_regular_file(root / "view-preview.png"),
        "Actual Hugin preview must match selected angles");
    invoke(argv[1], root, "calibration-view-save", selected, false);
    auto saving = selected;
    saving.insert("preview_verified", true);
    const auto saved = invoke(argv[1], root, "calibration-view-save", saving);
    expect(
        saved.value("saved").toBool() && saved.value("calibration_complete").toBool() &&
            saved.value("alignment_reused").toBool(),
        "Save must reuse and publish the saved alignment");
    const auto stored = YAML::LoadFile((game / "config.yaml").string());
    expect(
        value(read_stitch_projection_framing(stored)).rotation_degrees == std::array<double, 3>{0, -20, 4},
        "Selected rotation must be published");
    invoke(argv[1], root, "calibration-view-preview", selected, false);
    const auto crop = invoke(argv[1], root, "calibration-view-read", {{"kind", "crop"}});
    const QJsonObject cropped{
        {"kind", "crop"},
        {"snapshot", crop.value("snapshot")},
        {"crop", QJsonArray{0.1, 0.9, 0.1, 0.9}},
        {"automatic", false}};
    const auto final = invoke(argv[1], root, "calibration-view-save", cropped);
    expect(
        final.value("saved").toBool() && final.value("calibration_complete").toBool(),
        "Crop save must publish a new view");
    const auto hugin = [&]() {
      auto lock = value(lock_canvas_constraint_artifacts(game));
      return value(stitch_artifact_generation_id_locked(game));
    }();
    const std::string output = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin.size()) + "\n" +
        hugin + "post-stitch-rotate-degrees:0\noutput-size:64x32\n";
    auto output_config = YAML::LoadFile((game / "config.yaml").string());
    output_config["rink"]["stitched_output_generation"] = output;
    output_config["rink"]["stitched_output_persisted_rotation_degrees"] = 0.0;
    write(game / "config.yaml", YAML::Dump(output_config));
    const auto rotation = invoke(
        argv[1],
        root,
        "calibration-view-rotation-stage",
        {{"source_generation", QString::fromStdString(output)}, {"post_stitch_rotate_degrees", 7.0}});
    expect(
        rotation.value("changed").toBool() && rotation.value("hugin_generation").toString().toStdString() == hugin &&
            YAML::LoadFile((game / "config.yaml").string())["stitching"]["post_stitch_rotate_degrees"].as<double>() ==
                7.0,
        "Private rotation staging must preserve exact Hugin input and publish checked artifact receipts");
    invoke(
        argv[1],
        root,
        "calibration-view-rotation-stage",
        {{"source_generation", QString::fromStdString(output)}, {"post_stitch_rotate_degrees", 8.0}},
        false);
    std::cout << "Saved calibration real-Hugin tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
