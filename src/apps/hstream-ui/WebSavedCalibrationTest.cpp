#include <opencv2/imgcodecs.hpp>
#include <sys/stat.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <fstream>
#include <iostream>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/StitchingReframe.h"
#include "src/apps/hstream-ui/WebCalibrationControls.h"
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
  result.mapping_backend = config["stitching"]["mapping_backend"].as<std::string>();
  result.projection = config["stitching"]["projection"].as<std::string>();
  result.run_autooptimizer = config["stitching"]["run_autooptimizer"].as<bool>();
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
    bool success = true,
    const QJsonArray& authorized_media = {}) {
  fs::remove(root / "operation-result.json");
  hm::web::WriteObject(
      root / "operation-request.json",
      {{"schema", 1},
       {"run_id", "saved-view-contract"},
       {"operation", operation},
       {"root", QString::fromStdString(root.string())},
       {"game_dir", "game"},
       {"authorized_media", authorized_media},
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

std::string read(const fs::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
QString digest(const std::string& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex());
}
QJsonObject controls_payload(const fs::path& game, const QJsonObject& before, const QJsonObject& desired) {
  const auto hugin = [&] {
    auto lock = value(lock_canvas_constraint_artifacts(game));
    return value(stitch_artifact_preflight_generation_id_locked(game));
  }();
  const std::string generation = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin.size()) + "\n" +
      hugin + "post-stitch-rotate-degrees:0\noutput-size:64x32\n";
  auto config = YAML::LoadFile((game / "config.yaml").string());
  config["rink"]["stitched_output_generation"] = generation;
  config["rink"]["stitched_output_persisted_rotation_degrees"] = 0;
  write(game / "config.yaml", YAML::Dump(config));
  return {
      {"version", 1},
      {"source",
       QJsonObject{
           {"generation", QString::fromStdString(generation)}, {"config_sha256", digest(read(game / "config.yaml"))}}},
      {"before_controls", before},
      {"desired_controls", desired},
      {"defaults", QJsonObject{}},
      {"sources", QJsonArray{}}};
}

void controls_staging(const QString& binary, const fs::path& source) {
  const auto original = read(source / "config.yaml");
  for (const char* kind :
       {"unchanged",
        "width-alias",
        "budget",
        "camera",
        "view",
        "time",
        "count",
        "hash",
        "unknown",
        "cancel",
        "cancel-after-commit",
        "cancel-after-cleanup",
        "roles"}) {
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::copy(source, game, fs::copy_options::recursive);
    // The form's hidden default differs from the actual accepted saved view.
    // An unchanged leaf must not replace those accepted leveling angles.
    QJsonObject before{
        {"stitching",
         QJsonObject{
             {"control_point_limit", 1500},
             {"calibration_frame_count", 2},
             {"stitch_frame_time", "00:00:00"},
             {"camera_fov", QJsonObject{{"horizontal_fov", 109}, {"vertical_fov", 64}}},
             {"camera_automation", QJsonObject{{"fov", false}, {"lens", false}}},
             {"projection_framing",
              QJsonObject{{"rotation_degrees", QJsonArray{0, 0, 0}}, {"crop", QJsonArray{0, 1, 0, 1}}}}}}};
    auto desired = before;
    auto stitching = desired.value("stitching").toObject();
    const std::string name(kind);
    if (name == "budget" || name == "cancel-after-commit" || name == "cancel-after-cleanup")
      stitching["control_point_limit"] = 1600;
    if (name == "camera")
      stitching["camera_fov"] = QJsonObject{{"horizontal_fov", 105}, {"vertical_fov", 64}};
    if (name == "view") {
      auto framing = stitching.value("projection_framing").toObject();
      framing["crop"] = QJsonArray{0.1, 0.9, 0.1, 0.9};
      stitching["projection_framing"] = framing;
    }
    if (name == "time")
      stitching["stitch_frame_time"] = "00:00:01";
    if (name == "count")
      stitching["calibration_frame_count"] = 3;
    desired["stitching"] = stitching;
    if (name == "width-alias") {
      auto config = YAML::LoadFile((game / "config.yaml").string());
      config["stitching"].remove("max_output_width");
      config["pipeline"]["hmstitcher"]["properties"]["max-output-width"] = 1280;
      write(game / "config.yaml", YAML::Dump(config));
      auto canvas_provenance = read(game / "stitching_canvas_provenance");
      const std::string saved_limit = "max-output-width=0\n";
      const auto limit = canvas_provenance.find(saved_limit);
      expect(limit != std::string::npos, "Width-alias fixture needs published canvas provenance");
      canvas_provenance.replace(limit, saved_limit.size(), "max-output-width=1280\n");
      write(game / "stitching_canvas_provenance", canvas_provenance);
    }
    QJsonArray authorized;
    if (name == "roles") {
      auto config = YAML::LoadFile((game / "config.yaml").string());
      for (const char* role : {"left", "right"}) {
        const auto media = root / (std::string(role) + ".mp4");
        write(media, "immutable media fixture");
        fs::create_directories(game / "media");
        const auto alias = "media/" + std::string(role) + ".mp4";
        fs::create_symlink(media, game / alias);
        config["game"]["videos"][role] = std::vector<std::string>{alias};
        config["hstream_ui"]["video_roles"][role] = std::vector<std::string>{alias};
        authorized.append(QString::fromStdString(media.string()));
      }
      config["stitching"]["camera_automation"]["fov"] = false;
      config["stitching"]["camera_automation"]["lens"] = false;
      config["rink"]["mask_frame"]["input_id"] = std::string(64, 'a');
      write(game / "config.yaml", YAML::Dump(config));
      fs::create_directories(game / "rink-mask-input" / "historical");
      write(game / "rink-mask-input" / "historical" / "manifest.yaml", "historical source provenance");
    }
    auto payload = controls_payload(game, before, desired);
    if (name == "roles") {
      QJsonArray roles;
      for (const char* role : {"left", "right"}) {
        const QString alias = "media/" + QString::fromLatin1(role) + ".mp4";
        roles.append(
            QJsonObject{
                {"id", QString::fromLatin1(role)},
                {"before_role", QString::fromLatin1(role)},
                {"after_role", std::string(role) == "left" ? "right" : "left"},
                {"before_alias", alias},
                {"after_alias", alias}});
      }
      payload["sources"] = roles;
    }
    const auto initial = read(game / "config.yaml");
    if (name == "hash") {
      auto identity = payload.value("source").toObject();
      identity["config_sha256"] = QString(64, '0');
      payload["source"] = identity;
    }
    if (name == "unknown")
      payload["desired_controls"] = QJsonObject{{"hstream_ui", QJsonObject{{"job", "untrusted recipe"}}}};
    if (name == "cancel") {
      hm::web::Request request{root, game, "staging-cancellation", "calibration-controls-stage", payload, {}, {}};
      for (const int cancel_at : {1, 3}) {
        int checks = 0;
        bool cancelled = false;
        try {
          hm::web::RunCalibrationControlsStage(request, [&] { return ++checks >= cancel_at; });
        } catch (const std::exception& error) {
          cancelled = std::string(error.what()).find("cancelled") != std::string::npos;
        }
        expect(
            cancelled && read(game / "config.yaml") == initial && fs::exists(game / "mapping_0000.tif"),
            "Cancellation before native commit must preserve the exact private source");
      }
      continue;
    }
    if (name == "cancel-after-commit" || name == "cancel-after-cleanup") {
      hm::web::Request request{root, game, "staging-cancellation", "calibration-controls-stage", payload, {}, {}};
      const bool cleaned = name == "cancel-after-cleanup";
      int checks = 0;
      bool cancelled = false;
      try {
        hm::web::RunCalibrationControlsStage(request, [&] { return ++checks >= (cleaned ? 5 : 4); });
      } catch (const std::exception& error) {
        cancelled = std::string(error.what()).find("cancelled") != std::string::npos;
      }
      const auto state = YAML::LoadFile((game / "config.yaml").string())["hstream_ui"]["stitching_calibration"];
      expect(
          cancelled && state["status"].as<std::string>() == "pending" &&
              state["stale_from"].as<std::string>() == "features" && !state["artifacts_invalidated"].as<bool>() &&
              fs::exists(game / "mapping_0000.tif") != cleaned && fs::exists(game / "left.png"),
          "Postcommit cancellation must leave an incomplete private workspace without a completion receipt");
      continue;
    }
    const bool success = name != "hash" && name != "unknown";
    const auto result = invoke(binary, root, "calibration-controls-stage", payload, success, authorized);
    if (!success) {
      expect(
          read(game / "config.yaml") == initial && fs::exists(game / "mapping_0000.tif"),
          "Rejected staging must preserve config and calibration artifacts");
      continue;
    }
    const auto stored = YAML::LoadFile((game / "config.yaml").string());
    expect(
        result.value("staged").toBool() && result.value("cleanup_completed").toBool() &&
            result.value("source") == payload.value("source") &&
            result.value("config_sha256").toString() == digest(read(game / "config.yaml")) &&
            result.value("invalidation_id").toString().toStdString() ==
                stored["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>(),
        "Native controls receipt must bind original source, new owner and exact final config bytes");
    expect(
        value(read_stitch_projection_framing(stored)).rotation_degrees == std::array<double, 3>{0, -15, 3},
        "Ordinary controls staging must preserve accepted hidden leveling");
    const bool unchanged = name == "unchanged" || name == "width-alias", reframe = name == "view";
    expect(
        result.value("pending").toBool() != unchanged && result.value("alignment_reused").toBool() == reframe,
        "Native controls staging confused unchanged, reframe and fresh solve");
    const QString stage = unchanged   ? ""
        : reframe || name == "camera" ? "canvas"
        : name == "budget"            ? "features"
                                      : "input";
    expect(
        result.value("start_stage").toString() == stage, "Native controls adapter chose a different dependency stage");
    expect(
        fs::exists(game / "mapping_0000.tif") == (unchanged || reframe),
        "Native cleanup retained the wrong mapping generation");
    expect(
        fs::exists(game / "left.png") == (unchanged || reframe || name == "budget"),
        "Native cleanup lost or retained the wrong original frames");
    if (reframe) {
      auto launch = YAML::Clone(stored);
      launch["hstream_ui"]["stitching_calibration"]["reframe"]["desired_view"]["projection_framing"]["crop"] =
          YAML::Load("[0.1, 0.9, 0.1, 0.9]");
      write(game / "config.yaml", YAML::Dump(launch));
      auto artifact_lock = value(lock_canvas_constraint_artifacts(game));
      auto config_lock = value(GameConfigTransactionLock::Acquire(game));
      const auto playback = ValidateStitchingReframeIntentLocked(game, YAML::LoadFile((game / "config.yaml").string()));
      expect(
          playback.ok(), "Native playback must consume staged intent after outer launch YAML shortens numeric scalars");
    }
    if (name == "roles")
      expect(
          stored["game"]["videos"]["left"][0].as<std::string>() == "media/right.mp4" &&
              (!stored["rink"]["mask_frame"] || !stored["rink"]["mask_frame"]["input_id"]) &&
              read(game / "rink-mask-input" / "historical" / "manifest.yaml") == "historical source provenance",
          "Role staging must rebind private playlists without rewriting historical source provenance");
  }
  expect(read(source / "config.yaml") == original, "Draft staging changed the immutable selected calibration");
}

void controls_staging_regressions(const QString& binary, const fs::path& source) {
  for (const std::string name :
       {"legacy-count",
        "mask-time",
        "generated-mask-time",
        "angles",
        "angles-null",
        "angles-null-unchanged",
        "angles-null-default",
        "split-angles",
        "unsupported-mask-time",
        "blend-mode",
        "blend-feather",
        "added-group",
        "removed-group",
        "sync-view"}) {
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::copy(source, game, fs::copy_options::recursive);
    auto config = YAML::LoadFile((game / "config.yaml").string());
    QJsonObject before, desired;
    if (name == "legacy-count") {
      config["stitching"].remove("calibration_frame_count");
      before["stitching"] = QJsonObject{{"calibration_frame_count", 2}};
      desired = before;
    } else if (name == "mask-time") {
      config["stitching"]["rink_mask_frame_time"] = "00:00:30";
      before["stitching"] = QJsonObject{{"rink_mask_frame_time", "00:00:30"}};
      desired["stitching"] = QJsonObject{{"rink_mask_frame_time", "00:01:00"}};
    } else if (name == "generated-mask-time") {
      config["stitching"]["rink_config"] = "vallco";
      config["stitching"]["rink_configs"]["vallco"]["rink_mask_frame_time"] = "00:00:30";
      config["stitching"]["rink_configs"]["sharks-ice"]["rink_mask_frame_time"] = "00:01:00";
      config["stitching"]["rink_mask_frame_time"] = "00:00:30";
      config["hstream_ui"]["generated_rink_mask_frame_time"]["generated"] = "00:00:30";
      config["hstream_ui"]["generated_rink_mask_frame_time"]["previous"] = YAML::Node(YAML::NodeType::Null);
      before["stitching"] = QJsonObject{{"rink_config", "vallco"}, {"rink_mask_frame_time", QJsonValue::Null}};
      desired["stitching"] = QJsonObject{{"rink_config", "sharks-ice"}, {"rink_mask_frame_time", QJsonValue::Null}};
    } else if (name == "angles") {
      config["pipeline"]["hmplaycropper"]["fixed-edge-rotation-angle-left"] = 7;
      before["rink"] = QJsonObject{{"camera", QJsonObject{{"fixed_edge_rotation_angle", 10}}}};
      desired["rink"] = QJsonObject{{"camera", QJsonObject{{"fixed_edge_rotation_angle", QJsonArray{1, 2}}}}};
    } else if (name == "angles-null" || name == "angles-null-unchanged") {
      config["rink"]["camera"]["fixed_edge_rotation_angle"] =
          name == "angles-null" ? YAML::Node(10) : YAML::Node(YAML::NodeType::Null);
      before["rink"] = QJsonObject{
          {"camera",
           QJsonObject{
               {"fixed_edge_rotation_angle", name == "angles-null" ? QJsonValue(10) : QJsonValue(QJsonValue::Null)}}}};
      desired["rink"] = QJsonObject{{"camera", QJsonObject{{"fixed_edge_rotation_angle", QJsonValue::Null}}}};
    } else if (name == "split-angles") {
      desired["rink"] = QJsonObject{
          {"camera", QJsonObject{{"fixed_edge_rotation_angle_left", 1}, {"fixed_edge_rotation_angle_right", 2}}}};
    } else if (name == "unsupported-mask-time") {
      desired["rink"] = QJsonObject{{"mask_frame", QJsonObject{{"time", "00:01:00"}}}};
    } else if (name == "blend-mode" || name == "blend-feather") {
      config["pipeline"]["hmstitcher"]["blend-mode"] = "alpha";
      config["pipeline"]["hmstitcher"]["private-properties"]["blend-feather-fraction"] = 0.15;
      before["stitching"] = QJsonObject{{"blend_mode", "alpha"}, {"blend_feather_fraction", 0.15}};
      desired["stitching"] = QJsonObject{
          {"blend_mode", name == "blend-mode" ? "laplacian" : "alpha"},
          {"blend_feather_fraction", name == "blend-feather" ? 0.2 : 0.15}};
    } else if (name == "added-group") {
      desired["stitching"] = QJsonObject{{"blend_mode", "alpha"}};
    } else if (name == "removed-group") {
      config["stitching"]["blend_mode"] = "alpha";
      before["stitching"] = QJsonObject{{"blend_mode", "alpha"}};
    } else if (name == "sync-view") {
      before["stitching"] =
          QJsonObject{{"sync_method", "audio"}, {"projection_framing", QJsonObject{{"crop", QJsonArray{0, 1, 0, 1}}}}};
      desired["stitching"] = QJsonObject{
          {"sync_method", "imu"}, {"projection_framing", QJsonObject{{"crop", QJsonArray{0.1, 0.9, 0.1, 0.9}}}}};
    }
    write(game / "config.yaml", YAML::Dump(config));
    write(game / "rink_mask_0.png", "old mask artifact");
    auto payload = controls_payload(game, before, desired);
    if (name == "angles-null-default")
      payload["defaults"] =
          QJsonObject{{"rink", QJsonObject{{"camera", QJsonObject{{"fixed_edge_rotation_angle", QJsonValue::Null}}}}}};
    const auto original = read(game / "config.yaml");
    const bool rejected = name == "split-angles" || name == "unsupported-mask-time";
    const auto result = invoke(binary, root, "calibration-controls-stage", payload, !rejected);
    if (rejected) {
      expect(
          read(game / "config.yaml") == original && fs::exists(game / "rink_mask_0.png"),
          "Unsupported calibration spellings must fail before changing private config or artifacts");
      continue;
    }
    const auto stored = YAML::LoadFile((game / "config.yaml").string());
    expect(
        result.value("pending").toBool() == (name == "sync-view") && !result.value("alignment_reused").toBool(),
        "Only synchronization plus view edits in this matrix should restart calibration");
    if (name == "legacy-count")
      expect(
          stored["stitching"]["calibration_frame_count"].as<int>() == 2 &&
              stored["hstream_ui"]["stitching_calibration"]["frame_count"].as<int>() == 2 &&
              fs::exists(game / "left.png") && fs::exists(game / "mapping_0000.tif"),
          "Unchanged legacy completed count must retain its actual two inputs and maps");
    if (name == "mask-time" || name == "generated-mask-time")
      expect(
          value(read_rink_mask_frame_time(stored)) == "00:01:00" && result.value("output_changed").toBool() &&
              fs::exists(game / "mapping_0000.tif") && !fs::exists(game / "rink_mask_0.png"),
          "Canonical mask time and inherited rink edits must select the new time and retire only output artifacts");
    if (name == "angles")
      expect(
          stored["rink"]["camera"]["fixed_edge_rotation_angle"].as<std::vector<double>>() ==
                  std::vector<double>{1, 2} &&
              !stored["pipeline"]["hmplaycropper"]["fixed-edge-rotation-angle-left"],
          "Fixed-edge edits must persist the scalar-or-pair key consumed by the native runner");
    if (name == "angles-null" || name == "angles-null-unchanged")
      expect(
          stored["rink"]["camera"]["fixed_edge_rotation_angle"].IsNull(),
          "Explicit and unchanged null fixed-edge angles must retain the native disabled mapping");
    if (name == "blend-mode" || name == "blend-feather") {
      const auto blend = value(ResolveBlendSettings(value(hm::baseline_config::load()).values, YAML::Node(), stored));
      expect(
          blend.mode == (name == "blend-mode" ? "laplacian" : "alpha") &&
              blend.feather_fraction == (name == "blend-feather" ? 0.2 : 0.15),
          "Changing one blend control must preserve the other control's effective native alias");
    }
    if (name == "added-group" || name == "removed-group") {
      const auto blend = value(ResolveBlendSettings(value(hm::baseline_config::load()).values, YAML::Node(), stored));
      expect(
          blend.mode == (name == "added-group" ? "alpha" : "laplacian") &&
              YAML::Dump(stored["stitching"]["camera_fov"]) == YAML::Dump(config["stitching"]["camera_fov"]) &&
              YAML::Dump(stored["stitching"]["projection_framing"]) ==
                  YAML::Dump(config["stitching"]["projection_framing"]) &&
              stored["hstream_ui"]["stitching_calibration"]["frame_count"].as<int>() == 2 &&
              fs::exists(game / "mapping_0000.tif") && fs::exists(game / "left.png"),
          "Sparse control group additions and removals must preserve unmentioned saved geometry and inputs");
    }
    if (name == "sync-view")
      expect(
          result.value("start_stage").toString() == "input" && result.value("cleanup").toString() == "all" &&
              !fs::exists(game / "mapping_0000.tif") && !fs::exists(game / "left.png"),
          "Changed synchronization cannot reuse the saved alignment for a simultaneous crop edit");
  }
}
void saved_view_controls_staging(const QString& binary, const fs::path& source) {
  const auto original = read(source / "config.yaml");
  const auto saved = YAML::Load(original);
  expect(
      saved["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "complete" &&
          !saved["rink"]["stitched_output_generation"],
      "Real saved-crop output must exercise the absent rendered-output identity");
  for (const std::string name :
       {"absent",
        "null",
        "legacy-no-claim",
        "present",
        "null-for-present",
        "string-for-absent",
        "omitted",
        "wrong-type",
        "empty",
        "nonscalar",
        "hash",
        "pending",
        "stale",
        "reframe",
        "live-authorization",
        "claim-controls",
        "claim-owner",
        "claim-malformed",
        "stale-output",
        "missing-map",
        "malformed-hugin"}) {
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::copy(source, game, fs::copy_options::recursive);
    auto config = YAML::Load(original);
    QJsonObject payload{
        {"version", 1},
        {"source", QJsonObject{{"generation", QJsonValue::Null}}},
        {"before_controls", QJsonObject{}},
        {"desired_controls", QJsonObject{}},
        {"defaults", QJsonObject{}},
        {"sources", QJsonArray{}}};
    if (name == "present" || name == "null-for-present") {
      payload = controls_payload(game, {}, {});
      config = YAML::LoadFile((game / "config.yaml").string());
    }
    auto identity = payload.value("source").toObject();
    if (name == "null-for-present")
      identity["generation"] = QJsonValue::Null;
    if (name == "null")
      config["rink"]["stitched_output_generation"] = YAML::Node(YAML::NodeType::Null);
    if (name == "legacy-no-claim")
      config["hstream_ui"]["stitching_calibration"].remove("backend_generation");
    if (name == "string-for-absent")
      identity["generation"] = "unrelated-generation";
    if (name == "omitted")
      identity.remove("generation");
    if (name == "wrong-type")
      identity["generation"] = false;
    if (name == "empty")
      config["rink"]["stitched_output_generation"] = "";
    if (name == "nonscalar")
      config["rink"]["stitched_output_generation"]["invalid"] = true;
    if (name == "pending")
      config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    if (name == "stale")
      config["hstream_ui"]["stitching_calibration"]["stale_from"] = "canvas";
    if (name == "reframe")
      config["hstream_ui"]["stitching_calibration"]["reframe"] = YAML::Node(YAML::NodeType::Map);
    if (name == "live-authorization")
      config["rink"]["stitched_output_pending_authorization_id"] = "unfinished-live-owner";
    if (name == "claim-controls")
      config["stitching"]["projection_framing"]["crop"][0] = 0.2;
    if (name == "claim-owner")
      config["hstream_ui"]["stitching_calibration"]["backend_generation"]["invalidation_id"] = "other-owner";
    if (name == "claim-malformed")
      config["hstream_ui"]["stitching_calibration"]["backend_generation"] = YAML::Node(YAML::NodeType::Map);
    if (name == "stale-output") {
      const std::string stale =
          "hstream-stitched-output-v1\nhugin-bytes:6\nstale\n"
          "post-stitch-rotate-degrees:0\noutput-size:64x32\n";
      config["rink"]["stitched_output_generation"] = stale;
      identity["generation"] = QString::fromStdString(stale);
    }
    if (name == "missing-map")
      fs::remove(game / "mapping_0000.tif");
    if (name == "malformed-hugin")
      write(game / "stitching_generation_id", "not-a-generation");
    write(game / "config.yaml", YAML::Dump(config));
    const auto initial = read(game / "config.yaml");
    identity["config_sha256"] = name == "hash" ? QString(64, '0') : digest(initial);
    payload["source"] = identity;
    const bool success = name == "absent" || name == "null" || name == "legacy-no-claim" || name == "present";
    const auto result = invoke(binary, root, "calibration-controls-stage", payload, success);
    if (!success) {
      expect(
          read(game / "config.yaml") == initial, "Rejected saved-view staging must preserve the exact source config");
      continue;
    }
    const auto staged = YAML::LoadFile((game / "config.yaml").string());
    expect(
        result.value("source").toObject() == identity && !result.value("pending").toBool() &&
            result.value("cleanup").toString() == "none" && result.value("cleanup_completed").toBool() &&
            result.value("config_sha256").toString() == digest(read(game / "config.yaml")) &&
            !result.value("invalidation_id").toString().isEmpty() &&
            result.value("invalidation_id").toString().toStdString() !=
                config["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() &&
            result.value("invalidation_id").toString().toStdString() ==
                staged["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() &&
            read(game / "mapping_0000.tif") == read(source / "mapping_0000.tif"),
        "Saved-view no-op staging must bind exact nullable identity and a fresh owner without discarding alignment");
    expect(
        YAML::Dump(staged["stitching"]["projection_framing"]) == YAML::Dump(config["stitching"]["projection_framing"]),
        "Saved-view staging must preserve accepted crop/leveling");
    if (name != "present")
      expect(
          !staged["rink"]["stitched_output_generation"] || staged["rink"]["stitched_output_generation"].IsNull(),
          "Staging cannot fabricate a rendered-output identity for a saved view");
  }
  expect(read(source / "config.yaml") == original, "Staging checks changed the saved-crop source");
}
void sparse_controls_changed_defaults(const QString& binary, const fs::path& source) {
  const auto original = read(source / "config.yaml");
  const auto source_config = YAML::Load(original);
  expect(
      !source_config["stitching"]["camera_fov"] && !source_config["stitching"]["input_lens"] &&
          !source_config["stitching"]["camera_configs"] &&
          source_config["hstream_ui"]["stitching_calibration"]["backend_generation"].IsMap(),
      "The completed sparse source must use a modern profile-derived optics claim");
  for (const std::string name : {"changed-defaults", "wrong-source-controls", "wrong-private-optics", "wrong-owner"}) {
    QTemporaryDir temporary;
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::copy(source, game, fs::copy_options::recursive);
    auto config = YAML::Load(original);
    auto state = config["hstream_ui"]["stitching_calibration"];
    state["control_points"] = YAML::Clone(config["stitching"]["control_point_limit"]);
    state["frame_count"] = YAML::Clone(config["stitching"]["calibration_frame_count"]);
    QJsonObject before_stitching{
        {"camera_config", "gopro-mission-1"},
        {"projection_framing", QJsonObject{{"rotation_degrees", QJsonArray{0, 0, 0}}}}};
    QJsonObject current_stitching{
        {"camera_config", "gopro-mission-1"},
        {"camera_fov", QJsonObject{{"horizontal_fov", 110}, {"vertical_fov", 80}}},
        {"camera_automation", QJsonObject{{"fov", false}, {"lens", false}}},
        {"input_lens", QJsonObject{{"projection", "rectilinear"}, {"optimization", "refine"}}}};
    auto desired_stitching = before_stitching;
    for (auto item = current_stitching.begin(); item != current_stitching.end(); ++item)
      desired_stitching[item.key()] = item.value();
    if (name == "wrong-source-controls")
      before_stitching["camera_fov"] = QJsonObject{{"horizontal_fov", 112}, {"vertical_fov", 70}};
    if (name == "wrong-private-optics")
      config["stitching"]["camera_fov"]["horizontal_fov"] = 112;
    if (name == "wrong-owner")
      state["backend_generation"]["invalidation_id"] = "unrelated-owner";
    write(game / "config.yaml", YAML::Dump(config));
    const auto initial = read(game / "config.yaml");
    const QJsonObject payload{
        {"version", 1},
        {"source", QJsonObject{{"generation", QJsonValue::Null}, {"config_sha256", digest(initial)}}},
        {"before_controls", QJsonObject{{"stitching", before_stitching}}},
        {"desired_controls", QJsonObject{{"stitching", desired_stitching}}},
        {"defaults", QJsonObject{{"stitching", current_stitching}}},
        {"sources", QJsonArray{}}};
    const bool success = name == "changed-defaults";
    const auto result = invoke(binary, root, "calibration-controls-stage", payload, success);
    if (!success) {
      expect(read(game / "config.yaml") == initial, "Conflicting old optics or claims must fail before staging");
      continue;
    }
    const auto stored = YAML::LoadFile((game / "config.yaml").string());
    const auto camera = value(read_stitch_camera_selection(stored));
    expect(
        result.value("source") == payload.value("source") && result.value("pending").toBool() &&
            result.value("start_stage").toString() == "canvas" && result.value("cleanup_completed").toBool() &&
            camera.horizontal_fov == 110 && camera.vertical_fov == 80 &&
            stored["stitching"]["input_lens"]["optimization"].as<std::string>() == "refine" &&
            YAML::Dump(stored["stitching"]["projection_framing"]) ==
                YAML::Dump(config["stitching"]["projection_framing"]),
        "New Defaults must change desired optics without corrupting the source claim or replacing saved framing");
  }
  expect(read(source / "config.yaml") == original, "Defaults staging changed the completed sparse source");
}
void sparse_still_defaults(const QString& binary) {
  QTemporaryDir temporary;
  const fs::path root(temporary.path().toStdString()), game = root / "game", home = root / "home";
  fs::create_directories(game);
  fs::create_directories(home);
  const auto original_home = qgetenv("HOME");
  qputenv("HOME", QByteArray::fromStdString(home.string()));
  fixture(game); // Native completion still verifies the ordinary map/project artifacts.
  auto effective = YAML::Clone(value(hm::baseline_config::load()).values);
  effective["stitching"]["camera_config"] = "gopro-mission-1";
  PreserveLegacyInputLensSelection(effective, effective);
  auto sparse = YAML::Clone(effective);
  for (const char* key : {"camera_configs", "camera_fov", "input_lens"})
    sparse["stitching"].remove(key);
  sparse["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  sparse["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "sparse-still";
  sparse["hstream_ui"]["stitching_calibration"].remove("stale_from");
  require(reserve_stitching_backend_generation_in_config(sparse, "sparse-still", choices(effective)));
  write(game / "config.yaml", YAML::Dump(sparse));
  expect(
      cv::imwrite((game / "s.png").string(), cv::Mat(32, 64, CV_8UC3, cv::Scalar(3, 2, 1))),
      "Cannot write sparse still");
  // With no embedded catalog or optics this resolves 108/90 and the fixed
  // fisheye profile, rather than the historical 127.2/95/no-lens fallbacks.
  const auto complete = [&] {
    return invoke(binary, root, "calibration-complete", {{"invalidation_id", "sparse-still"}});
  };
  expect(
      complete().value("preview").toString() == "game/setup-preview.png",
      "Sparse modern completion must inherit baseline camera optics");
  expect(fs::is_empty(home), "Read-only completion must not create user preferences");
  sparse_controls_changed_defaults(binary, game);
  auto user = YAML::Load(
      "stitching: {camera_fov: {horizontal_fov: 113, vertical_fov: 71}, input_lens: {optimization: fixed}}\n");
  effective["stitching"]["camera_fov"] = YAML::Clone(user["stitching"]["camera_fov"]);
  effective["stitching"]["input_lens"] = YAML::Clone(user["stitching"]["input_lens"]);
  PreserveLegacyInputLensSelection(effective, user);
  PreserveLegacyInputLensSelection(effective, sparse);
  // The native worker freezes nondefault resolved optics in its game config;
  // a custom claim with no corresponding worker-visible settings is invalid.
  write_stitch_camera_selection(sparse, value(read_stitch_camera_selection(effective)));
  sparse["hstream_ui"]["stitching_calibration"].remove("backend_generation");
  require(reserve_stitching_backend_generation_in_config(sparse, "sparse-still", choices(effective)));
  write(game / "config.yaml", YAML::Dump(sparse));
  const auto defaults = home / ".config/hstream-ui/defaults.yaml";
  fs::create_directories(defaults.parent_path());
  write(defaults, YAML::Dump(user));
  const auto before = read(defaults);
  expect(
      complete().value("preview").toString() == "game/setup-preview.png",
      "Completion must honor frozen private user camera defaults");
  expect(
      read(defaults) == before && !fs::exists(home / ".hstream"),
      "Still export must leave private user files unchanged");
  user["stitching"]["camera_fov"]["horizontal_fov"] = 114;
  user["stitching"]["input_lens"]["optimization"] = "refine";
  write(defaults, YAML::Dump(user));
  expect(
      complete().value("preview").toString() == "game/setup-preview.png",
      "Later Defaults changes must not override explicit frozen game optics");
  sparse["stitching"]["camera_fov"]["horizontal_fov"] = 114;
  write(game / "config.yaml", YAML::Dump(sparse));
  invoke(binary, root, "calibration-complete", {{"invalidation_id", "sparse-still"}}, false);
  qputenv("HOME", original_home);
}
} // namespace
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    expect(argc == 2, "Pass adapter binary");
    sparse_still_defaults(argv[1]);
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
    controls_staging(argv[1], game);
    controls_staging_regressions(argv[1], game);
    const auto snap = opened.value("snapshot");
    expect(opened.value("image_sizes").toArray().size() == 2, "Original images must be exposed");
    // Reset only stages configuration. The selected source and its optimized
    // camera alignment remain unchanged until Save Preset and the next run.
    const auto source_bytes = [&] {
      std::ifstream input(game / "config.yaml");
      return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    const QJsonObject optics{
        {"camera_config", "gopro-mission-1"},
        {"camera_fov", QJsonObject{{"horizontal_fov", 109}, {"vertical_fov", 64}}},
        {"input_lens",
         QJsonObject{
             {"projection", "rectilinear"},
             {"optimization", "auto"},
             {"distortion", QJsonArray{0, 0, 0}},
             {"center_offset", QJsonArray{0, 0}}}},
        {"camera_automation", QJsonObject{{"fov", false}, {"lens", false}}},
        {"rink_config", "sharks-ice"}};
    for (const char* backend : {"opencv-magsac", "opencv-affine-ransac"}) {
      QTemporaryDir reset_temporary;
      const fs::path reset_root(reset_temporary.path().toStdString()), reset_game = reset_root / "game";
      auto opencv = fixture(reset_game);
      opencv["stitching"]["mapping_backend"] = backend;
      opencv["stitching"]["projection"] = "rectilinear";
      opencv["stitching"]["run_autooptimizer"] = false;
      opencv["hstream_ui"]["stitching_calibration"].remove("backend_generation");
      require(reserve_stitching_backend_generation_in_config(opencv, "completed-solve", choices(opencv)));
      write(reset_game / "config.yaml", YAML::Dump(opencv));
      // OpenCV's completed maps do not provide a NONA optimized project or
      // Hugin canvas provenance. Its saved image editors must still reject it.
      for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
        fs::remove(reset_game / name);
      for (const char* kind : {"leveling", "crop"})
        invoke(argv[1], reset_root, "calibration-view-read", {{"kind", kind}}, false);
      auto controls = optics;
      controls.insert("mapping_backend", "nona");
      controls.insert("projection", "general-panini");
      controls.insert("run_autooptimizer", true);
      const QJsonObject payload{
          {"config", QJsonObject{{"stitching", controls}}},
          {"defaults", QJsonObject{{"stitching", QJsonObject{{"rink_config", "sharks-ice"}}}}},
          {"source_changed", false},
          {"settings_changed", true},
          {"output_changed", false}};
      const auto reset = invoke(argv[1], reset_root, "calibration-view-reset", payload);
      const auto stored = YAML::LoadFile((reset_game / "config.yaml").string());
      const auto state = stored["hstream_ui"]["stitching_calibration"];
      expect(
          reset.value("changed").toBool() && reset.value("pending").toBool() &&
              !reset.value("alignment_reused").toBool() && state["status"].as<std::string>() == "pending" &&
              state["stale_from"].as<std::string>() == "input" && !state["reframe"] && !state["backend_generation"] &&
              stored["stitching"]["mapping_backend"].as<std::string>() == "nona" &&
              stored["stitching"]["projection"].as<std::string>() == "general-panini",
          "Reset from either completed OpenCV backend must stage a full default NONA solve without Hugin artifacts");
      expect(
          reset.value("rotation_inherited").toBool() && reset.value("angles").toArray() == QJsonArray{0, -15, 0} &&
              stored["stitching"]["projection_framing"]["rotation_degrees"].IsNull(),
          "An OpenCV Reset must preserve inherited default framing");
      const auto pending_bytes = YAML::Dump(stored);
      invoke(argv[1], reset_root, "calibration-view-reset", payload, false);
      expect(
          YAML::Dump(YAML::LoadFile((reset_game / "config.yaml").string())) == pending_bytes,
          "Config-only Reset inspection must still reject an unfinished calibration without changing it");
    }
    for (bool solver_changed : {false, true}) {
      QTemporaryDir reset_temporary;
      const fs::path reset_root(reset_temporary.path().toStdString());
      fs::copy(game, reset_root / "game", fs::copy_options::recursive);
      auto controls = optics;
      if (solver_changed)
        controls.insert("calibration_frame_count", 3);
      const auto reset = invoke(
          argv[1],
          reset_root,
          "calibration-view-reset",
          {{"config", QJsonObject{{"stitching", controls}}},
           {"defaults", QJsonObject{{"stitching", QJsonObject{{"rink_config", "sharks-ice"}}}}},
           {"source_changed", false},
           {"settings_changed", true},
           {"output_changed", false}});
      if (reset.value("alignment_reused").toBool() != !solver_changed) {
        std::cerr << QJsonDocument(reset).toJson().constData();
      }
      expect(
          reset.value("staged").toBool() && reset.value("pending").toBool() &&
              reset.value("alignment_reused").toBool() == !solver_changed,
          "Reset must distinguish preserved alignment from changed solve inputs");
      expect(
          reset.value("rotation_inherited").toBool() && reset.value("angles").toArray() == QJsonArray{0, -15, 0},
          "Reset inherits the resolved nonzero rink default instead of inventing zero angles");
      expect(
          reset.value("rink_config").toString() == "sharks-ice" &&
              reset.value("rink_profiles").toObject().value("sharks-ice").toBool(),
          "Reset carries native rink ownership for dormant browser fixed-edge controls");
      const auto reset_config = YAML::LoadFile((reset_root / "game/config.yaml").string());
      const auto state = reset_config["hstream_ui"]["stitching_calibration"];
      expect(
          reset_config["stitching"]["projection_framing"]["rotation_degrees"].IsNull(),
          "Reset preserves null/inherited leveling in private YAML");
      expect(
          state["status"].as<std::string>() == "pending" &&
              state["stale_from"].as<std::string>() == (solver_changed ? "input" : "canvas") &&
              reset_config["rink"]["scoreboard"]["perspective_polygon"].IsNull(),
          "Reset owns pending regeneration and invalidates dependent scoreboard geometry");
      expect(reset.value("config_sha256").toString().size() == 64, "Reset receipt must bind the exact staged config");
      invoke(argv[1], reset_root, "calibration-view-read", {{"kind", "leveling"}}, false);
    }
    {
      QTemporaryDir reset_temporary;
      const fs::path reset_root(reset_temporary.path().toStdString());
      fs::copy(game, reset_root / "game", fs::copy_options::recursive);
      auto controls = optics;
      controls.insert("rink_config", "");
      const auto reset = invoke(
          argv[1],
          reset_root,
          "calibration-view-reset",
          {{"config", QJsonObject{{"stitching", controls}}},
           {"defaults",
            QJsonObject{
                {"stitching",
                 QJsonObject{
                     {"projection_framing",
                      QJsonObject{
                          {"auto_fov", false},
                          {"horizontal_fov", 180},
                          {"rotation_degrees", QJsonArray{0, -15, 3}}}}}}}},
           {"source_changed", false},
           {"settings_changed", false},
           {"output_changed", false}});
      expect(
          !reset.value("changed").toBool() && !reset.value("pending").toBool(),
          "Identical default framing does not invalidate or adopt a new calibration");
    }
    const auto unchanged_bytes = [&] {
      std::ifstream input(game / "config.yaml");
      return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    expect(source_bytes == unchanged_bytes, "Staging or cancelling Reset must preserve the selected source bytes");
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
    saved_view_controls_staging(argv[1], game);
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
