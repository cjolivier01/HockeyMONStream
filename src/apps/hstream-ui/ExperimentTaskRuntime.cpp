#include "src/apps/hstream-ui/ExperimentTaskRuntime.h"

#include <QtCore/QJsonDocument>
#include <cmath>
#include <set>
#include <stdexcept>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

namespace hm::experiment {
namespace fs = std::filesystem;
using namespace hm::web;
namespace {
void require(bool condition, const std::string& error) {
  if (!condition)
    throw std::runtime_error(error);
}
void check(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <typename T>
T take(absl::StatusOr<T> value) {
  check(value.status());
  return std::move(*value);
}
qint64 integer(const QJsonObject& value, const QString& key, qint64 lower, qint64 upper, qint64 fallback) {
  const double result = Number(value, key, double(lower), double(upper), double(fallback));
  require(std::floor(result) == result, "Expected integer field " + key.toStdString());
  return qint64(result);
}
QString qs(const fs::path& path) {
  return QString::fromStdString(path.string());
}
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
} // namespace
void merge_settings(YAML::Node destination, const YAML::Node& overlay) {
  for (const auto& entry : overlay) {
    const auto key = entry.first.as<std::string>();
    if (key != "input_cameras" && key != "input_start_times" && entry.second.IsMap() && destination[key].IsMap())
      merge_settings(destination[key], entry.second);
    else
      destination[key] = YAML::Clone(entry.second);
  }
}

QJsonObject input_lens_json(const hm::stitching::StitchInputLens& lens) {
  QJsonArray distortion, center;
  for (double value : lens.distortion)
    distortion.append(value);
  for (double value : lens.center_offset)
    center.append(value);
  return {
      {"projection", text(lens.projection)},
      {"optimization", text(lens.optimization)},
      {"distortion", distortion},
      {"center_offset", center}};
}

QJsonObject camera_json(const hm::stitching::StitchCameraSelection& camera) {
  YAML::Node saved;
  hm::stitching::write_stitch_camera_selection(saved, camera);
  const auto optics = saved["stitching"];
  const auto input = input_lens_json(camera.input_lens);
  QJsonObject starts;
  for (const auto& [source, seconds] : camera.input_start_times)
    starts.insert(text(source), seconds);
  QJsonObject inputs;
  for (const auto& [source, values] : camera.input_cameras) {
    QJsonObject prescription{
        {"horizontal_fov", values.horizontal_fov},
        {"vertical_fov", values.vertical_fov},
        {"input_lens", input_lens_json(values.input_lens)}};
    if (values.source_size)
      prescription.insert("source_size", QJsonArray{(*values.source_size)[0], (*values.source_size)[1]});
    if (!values.configuration.empty())
      prescription.insert("camera_config", text(values.configuration));
    QJsonObject policies;
    if (values.auto_fov.has_value())
      policies.insert("fov", *values.auto_fov);
    if (values.auto_lens.has_value())
      policies.insert("lens", *values.auto_lens);
    if (values.frozen.has_value())
      prescription.insert("auto_input_lens_frozen", *values.frozen);
    if (!policies.empty())
      prescription.insert("camera_automation", policies);
    inputs.insert(text(source), prescription);
  }
  QJsonObject automation;
  for (const char* key : {"fov", "lens"}) {
    const auto value = optics["camera_automation"][key];
    automation.insert(key, value.IsNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(value.as<bool>()));
  }
  return {
      {"camera_config", text(optics["camera_config"].as<std::string>())},
      {"camera_fov", QJsonObject{{"horizontal_fov", camera.horizontal_fov}, {"vertical_fov", camera.vertical_fov}}},
      {"input_lens", input},
      {"camera_automation", automation},
      {"input_cameras", inputs},
      {"input_start_times", starts}};
}

QJsonObject filtering_json(const hm::stitching::ControlPointFiltering& filtering) {
  YAML::Node saved;
  hm::stitching::write_control_point_filtering(saved, filtering);
  const auto value = saved["stitching"]["control_point_filtering"];
  return {
      {"epipolar_ransac", value["epipolar_ransac"].as<bool>()},
      {"homography_consistency", value["homography_consistency"].as<bool>()}};
}

StitchingExperimentSettings settings(const QJsonObject& object, const YAML::Node& source) {
  StitchingExperimentSettings result;
  auto effective = YAML::Clone(take(hm::baseline_config::load()).values);
  merge_settings(effective, source);
  hm::stitching::PreserveLegacyInputLensSelection(effective, source);
  if (object.contains("camera")) {
    require(object.value("camera").isObject(), "Expected camera optics settings");
    const auto camera = object.value("camera").toObject();
    const std::set<QString> allowed{
        "camera_config", "camera_fov", "input_lens", "camera_automation", "input_cameras", "input_start_times"};
    for (auto item = camera.begin(); item != camera.end(); ++item)
      require(allowed.count(item.key()), "Unsupported experiment camera setting");
    // A profile-only selection resolves that profile's optics, rather than
    // carrying a previous profile's explicit FOV/lens correction into it.
    if (camera.contains("camera_config")) {
      effective["stitching"].remove("camera_fov");
      effective["stitching"].remove("input_lens");
      effective["stitching"].remove("input_cameras");
      effective["stitching"].remove("camera_automation");
    }
    YAML::Node override;
    override["stitching"] = YAML::Load(QJsonDocument(camera).toJson().toStdString());
    merge_settings(effective, override);
    hm::stitching::PreserveLegacyInputLensSelection(effective, override);
  }
  result.camera = take(hm::stitching::read_stitch_camera_selection(effective));
  if (object.contains("control_point_filtering")) {
    require(object.value("control_point_filtering").isObject(), "Expected geometric filtering settings");
    merge_settings(
        effective["stitching"]["control_point_filtering"],
        YAML::Load(QJsonDocument(object.value("control_point_filtering").toObject()).toJson().toStdString()));
  }
  result.control_point_filtering = take(hm::stitching::read_control_point_filtering(effective));
  const auto ui = source["hstream_ui"];
  const auto calibration = ui && ui.IsMap() ? ui["stitching_calibration"] : YAML::Node(YAML::NodeType::Undefined);
  result.control_points =
      integer(object, "control_points", 10, 5000, take(hm::stitching::read_control_point_limit(effective)));
  result.frame_count = integer(
      object,
      "frame_count",
      1,
      16,
      calibration && calibration["frame_count"] ? calibration["frame_count"].as<int>()
                                                : effective["stitching"]["calibration_frame_count"].as<int>());
  result.stitch_frame_time = object.contains("stitch_frame_time")
      ? RequiredString(object, "stitch_frame_time", 32).toStdString()
      : effective["stitching"]["stitch_frame_time"].as<std::string>();
  (void)hm::stitch_frame_time_to_nanoseconds(result.stitch_frame_time);
  if (object.contains("player_scan_playback_start")) {
    result.player_scan_playback_start = RequiredString(object, "player_scan_playback_start", 13).toStdString();
    require(
        hm::stitch_frame_time_to_nanoseconds(*result.player_scan_playback_start) >= 0,
        "Player scan Playback start must be nonnegative");
  }
  if (object.contains("control_point_matcher"))
    result.control_point_matcher = RequiredString(object, "control_point_matcher", 64).toStdString();
  if (object.contains("control_point_resolution"))
    result.control_point_resolution = RequiredString(object, "control_point_resolution", 64).toStdString();
  if (object.contains("rink_rotation_degrees")) {
    const auto angles = object.value("rink_rotation_degrees").toArray();
    require(angles.size() == 3, "Expected yaw/pitch/roll");
    std::array<double, 3> rotation;
    for (int index = 0; index < 3; ++index) {
      rotation[index] = angles[index].toDouble(1000);
      require(
          angles[index].isDouble() && std::isfinite(rotation[index]) && std::abs(rotation[index]) <= 180,
          "Invalid rink rotation");
    }
    result.rink_rotation_degrees = rotation;
  }
  return result;
}
QJsonObject settings_json(const StitchingExperimentSettings& value) {
  QJsonObject result{
      {"control_points", value.control_points},
      {"frame_count", value.frame_count},
      {"stitch_frame_time", text(value.stitch_frame_time)}};
  if (value.player_scan_playback_start)
    result.insert("player_scan_playback_start", text(*value.player_scan_playback_start));
  if (value.camera)
    result.insert("camera", camera_json(*value.camera));
  if (value.control_point_filtering)
    result.insert("control_point_filtering", filtering_json(*value.control_point_filtering));
  if (value.control_point_matcher)
    result.insert("control_point_matcher", text(*value.control_point_matcher));
  if (value.control_point_resolution)
    result.insert("control_point_resolution", text(*value.control_point_resolution));
  if (value.manual_control_points)
    result.insert("manual_control_points", text(*value.manual_control_points));
  if (value.rink_rotation_degrees) {
    QJsonArray rotation;
    for (double angle : *value.rink_rotation_degrees)
      rotation.append(angle);
    result.insert("rink_rotation_degrees", rotation);
  }
  return result;
}
StitchingExperimentWorkspace workspace(const Request& request) {
  if (!request.payload.contains("workspace")) {
    const auto config = YAML::LoadFile((request.game / "_hockeymon/config.yaml").string());
    const auto saved = take(MainStitchingExperimentWorkspace(request.game, settings({}, config)));
    require(
        saved.has_value(), "The selected run has no saved calibration workspace; recover its original frames first");
    return *saved;
  }
  require(request.payload.value("workspace").isObject(), "Expected the saved native workspace descriptor");
  const auto object = request.payload.value("workspace").toObject();
  StitchingExperimentWorkspace result;
  result.root = Contained(request.root, RequiredString(object, "root"));
  result.game_directory = Contained(request.root, RequiredString(object, "game_directory"));
  result.game_id = RequiredString(object, "game_id", 255).toStdString();
  result.invalidation_id = RequiredString(object, "invalidation_id", 128).toStdString();
  require(result.game_directory.filename() == result.game_id, "Workspace game identity mismatch");
  result.settings = settings(
      object.value("settings").toObject(), YAML::LoadFile((result.game_directory / "_hockeymon/config.yaml").string()));
  const auto manual = object.value("settings").toObject().value("manual_control_points").toString();
  if (!manual.isEmpty())
    result.settings.manual_control_points = manual.toStdString();
  return result;
}
QJsonObject workspace_json(const Request& request, const StitchingExperimentWorkspace& value) {
  return {
      {"root", text(Relative(request, value.root))},
      {"game_directory", text(Relative(request, value.game_directory))},
      {"game_id", text(value.game_id)},
      {"invalidation_id", text(value.invalidation_id)},
      {"settings", settings_json(value.settings)}};
}

bool materialized(const StitchingExperimentWorkspace& saved) {
  const auto config = YAML::LoadFile((saved.game_directory / "_hockeymon/config.yaml").string());
  const auto fingerprint = take(hm::stitching::player_frame_selection_fingerprint(config));
  if (fingerprint.empty())
    return false;
  const auto plan =
      take(hm::stitching::ParsePlayerFrameSelectionPlan(config["stitching"]["calibration_frame_selection"]));
  const auto inputs = take(hm::stitching::LoadPlayerFrameInputs(saved.game_directory, plan));
  return inputs.has_value() && inputs->images.size() == static_cast<size_t>(saved.settings.frame_count) &&
      config["stitching"]["calibration_frame_inputs_fingerprint"].as<std::string>("") == fingerprint;
}

void run_experiment(
    const Request& request,
    const StitchingExperimentWorkspace& saved,
    const ExperimentProcessRunner& run_process,
    bool complete,
    bool materialize) {
  const auto runtime_path = [&](const char* key, bool directory = false) {
    const fs::path path(RequiredString(request.runtime, key, 2048).toStdString());
    require(path.is_absolute(), "Runtime paths must be absolute operator settings");
    require(directory ? fs::is_directory(path) : fs::is_regular_file(path), "Native runtime path is unavailable");
    return qs(fs::canonical(path));
  };
  const auto exporter = runtime_path("exporter"), cli = runtime_path("cli"), config = runtime_path("config");
  const auto directory = runtime_path("root", true);
  const bool require_mask = request.payload.value("require_ice_mask").toBool(false);
  {
    const auto lock = take(hm::stitching::GameConfigTransactionLock::Acquire(saved.game_directory));
    auto config = YAML::LoadFile((saved.game_directory / "_hockeymon/config.yaml").string());
    check(hm::stitching::validate_pending_stitching_invalidation(config, saved.invalidation_id));
    YAML::Node arguments(YAML::NodeType::Sequence);
    for (const auto& argument : std::vector<std::string>{
             materialize ? "--stitching-materialize-selected-frames" : "--stitching-calibration-only",
             "--enable-sinks=FAKE",
             "--clean-expected-invalidation-id=" + saved.invalidation_id,
             "--options=pipeline.hmstitcher.calibration-frame-count=" + std::to_string(saved.settings.frame_count),
             "--stitch-frame-time=" + saved.settings.stitch_frame_time,
             "-t=1"})
      arguments.push_back(argument);
    if (saved.settings.player_scan_playback_start)
      arguments.push_back("--start-time=" + *saved.settings.player_scan_playback_start);
    if (require_mask && !materialize)
      arguments.push_back("--stitching-calibration-with-ice-mask");
    if (!request.workflow.isEmpty())
      arguments.push_back("--gpu-id=0");
    config["hstream_ui"]["job"]["arguments"] = arguments;
    check(hm::stitching::publish_game_config(saved.game_directory, YAML::Dump(config)));
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  const auto work = request.root / "work";
  fs::create_directories(work / "experiment-output");
  fs::create_directories(work / "experiment-runtime");
  environment.insert("HM_GAME_DIR", qs(saved.root));
  environment.insert("HM_OUTPUT_WORK_DIR", qs(work / "experiment-output"));
  environment.insert("HSTREAM_RUNTIME_CACHE_DIR", qs(work / "experiment-runtime"));
  environment.insert("HM_MAX_CONTROL_POINTS", QString::number(saved.settings.control_points));
  environment.insert("HM_STITCH_CALIBRATION_FRAME_COUNT", QString::number(saved.settings.frame_count));
  environment.insert("HSTREAM_CALIBRATION_PENDING", "1");
  environment.insert("HSTREAM_CALIBRATION_START_STAGE", materialize ? "input" : "features");
  environment.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", text(saved.invalidation_id));
  environment.insert("HSTREAM_STITCH_CALIBRATION_INSPECTION", "1");
  environment.insert("HSTREAM_RENDER_AUDIO_MUTED", "1");
  environment.insert("USE_NEW_NVSTREAMMUX", "yes");
  const auto script = qs(request.root / "experiment.sh");
  run_process(
      exporter,
      {"--interactive",
       "--game-dir",
       qs(saved.game_directory),
       "--output",
       script,
       "--runner",
       cli,
       "--config",
       config,
       "--working-directory",
       directory},
      directory,
      environment);
  run_process("/bin/bash", {script}, directory, environment);
  if (materialize)
    require(materialized(saved), "Materialization exited without verified selected images");
  if (complete && !materialize)
    check(CompleteStitchingExperimentWorkspace(saved, require_mask));
}

} // namespace hm::experiment
