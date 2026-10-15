// Thin headless owners of the same native libraries used by the desktop dialogs.
#include "hstream/src/libs/playtracker_replay/ReplaySession.h"
#include "hstream/src/libs/recording/Database.h"
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include "src/apps/hstream-ui/HighlightTracking.h"
#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"
#include "src/apps/hstream-ui/StitchingExperimentBackend.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

#include <yaml-cpp/yaml.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtGui/QGuiApplication>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
using hm::web::Artifact;
using hm::web::Contained;
using hm::web::Number;
using hm::web::ReadObject;
using hm::web::Relative;
using hm::web::Request;
using hm::web::RequiredString;
using hm::web::WriteObject;
volatile std::sig_atomic_t interrupted = 0;
void cancel(int) {
  interrupted = 1;
}
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
QString fingerprint(const QJsonObject& payload) {
  const auto result = RequiredString(payload, "fingerprint", 64);
  require(result.size() == 64 && result.toLatin1().toHex().size() == 128, "Invalid calibration fingerprint");
  for (const auto c : result)
    require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "Invalid calibration fingerprint");
  return result;
}

hm::ui::ArchiveMediaInfo probe(const fs::path& archive) {
  QProcess process;
  process.start("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", qs(archive)});
  require(process.waitForStarted(5000), "Could not start archive inspection");
  QByteArray output, errors;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (process.state() != QProcess::NotRunning) {
    process.waitForReadyRead(50);
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if (interrupted || output.size() + errors.size() > 1024 * 1024 || std::chrono::steady_clock::now() > deadline) {
      process.kill();
      process.waitForFinished(5000);
      throw std::runtime_error("Archive inspection cancelled or exceeded its limits");
    }
  }
  output += process.readAllStandardOutput();
  require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "Archive inspection failed");
  hm::ui::ArchiveMediaInfo media;
  QString error;
  if (!hm::ui::ParseArchiveMediaInfo(output, &media, &error))
    throw std::runtime_error(error.toStdString());
  return media;
}

hm::ui::HighlightPlan load_plan(const Request& request) {
  require(request.payload.value("plan").isObject(), "Expected a native highlight plan");
  auto json = request.payload.value("plan").toObject();
  // The worker resolves artifact IDs. No saved desktop absolute paths can leak
  // into this run's media/assets, including recorded annotation bindings.
  json.insert("archive_path", request.payload.value("archive_path").toString());
  const auto path = request.root / "highlight-plan.json";
  WriteObject(path, json);
  hm::ui::HighlightPlan plan;
  QString error;
  if (!hm::ui::LoadHighlightPlan(qs(path), &plan, &error))
    throw std::runtime_error(error.toStdString());
  if (!plan.archive_path.isEmpty())
    plan.archive_path = qs(Contained(request.root, plan.archive_path));
  for (auto& item : plan.intervals) {
    for (auto* logo : {&item.card.logo_a, &item.card.logo_b})
      if (!logo->isEmpty())
        *logo = qs(Contained(request.root, *logo));
    for (auto& annotation : item.annotations) {
      if (!annotation.database.isEmpty())
        annotation.database = qs(Contained(request.root, annotation.database));
      if (!annotation.archive_path.isEmpty())
        annotation.archive_path = qs(Contained(request.root, annotation.archive_path));
    }
  }
  return plan;
}

QJsonObject highlights(const Request& request, QJsonArray& artifacts) {
  auto plan = load_plan(request);
  QVector<hm::ui::HighlightInterval> selected;
  if (request.payload.contains("selected_indices")) {
    require(request.payload.value("selected_indices").isArray(), "Expected selected item indices");
    std::set<int> seen;
    for (const auto item : request.payload.value("selected_indices").toArray()) {
      require(item.isDouble() && item.toDouble() == item.toInt(-1), "Invalid selected item");
      const int index = item.toInt(-1);
      require(
          index >= 0 && index < plan.intervals.size() && seen.insert(index).second,
          "Invalid or duplicate selected item");
      selected.push_back(plan.intervals[index]);
    }
  } else {
    selected = plan.intervals;
  }
  qint64 duration = 0;
  for (const auto& item : selected)
    duration += hm::ui::HighlightItemDuration(item);
  const qint64 limit = request.operation == "highlight-preview" ? 30000 : 86400000;
  require(duration > 0 && duration <= limit, "Reel duration exceeds this operation's limit");
  hm::ui::HighlightReelPipeline::Request job;
  job.archive_path = plan.archive_path;
  job.asset_root = qs(request.root);
  job.output_path = qs(request.root / "highlights.mp4");
  require(!fs::exists(job.output_path.toStdString()), "Refusing to replace an existing reel output");
  job.items = selected;
  job.archive_offset_ms = integer(request.payload, "archive_offset_ms", 0, 86400000, 0);
  if (!job.archive_path.isEmpty())
    job.media = probe(job.archive_path.toStdString());
  else {
    job.media.width = 1920;
    job.media.height = 1080;
    job.media.frame_rate = 30;
  }
  hm::ui::HighlightReelPipeline pipeline;
  QString error;
  if (!pipeline.Start(std::move(job), &error))
    throw std::runtime_error(error.toStdString());
  while (true) {
    if (interrupted)
      pipeline.Cancel();
    const auto status = pipeline.Poll();
    if (status.finished) {
      require(!status.cancelled && !interrupted, "cancelled");
      require(status.error.isEmpty(), status.error.toStdString());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  artifacts.append(Artifact(request, request.root / "highlights.mp4", "video"));
  return {{"duration_ms", duration}, {"items", selected.size()}};
}

hm::stitching::CalibrationMatchSet matches(const Request& request, bool edit) {
  auto payload = request.payload;
  if (!edit && payload.value("fingerprint").toString().isEmpty()) {
    const auto config = YAML::LoadFile((request.game / "config.yaml").string());
    payload.insert(
        "fingerprint", text(config["hstream_ui"]["stitching_calibration"]["match_snapshot"].as<std::string>("")));
  }
  auto saved = take(hm::stitching::LoadCalibrationMatches(request.game, fingerprint(payload).toStdString()));
  for (const auto& frame : saved.frames)
    for (const auto& image : frame.images)
      Relative(request, image);
  if (!edit)
    return saved;
  require(request.payload.value("frames").isArray(), "Expected edited match frames");
  std::set<int> seen;
  for (const auto raw : request.payload.value("frames").toArray()) {
    require(raw.isObject(), "Expected edited frame object");
    const auto frame = raw.toObject();
    const int index = integer(frame, "index", 0, saved.frames.size() - 1, -1);
    require(index >= 0 && seen.insert(index).second, "Invalid or duplicated frame index");
    require(frame.value("matches").isArray(), "Expected point correspondences");
    const auto points = frame.value("matches").toArray();
    require(std::size_t(points.size()) <= hm::stitching::kMaximumEditableMatchesPerPair, "Too many control points");
    auto& destination = saved.frames[index];
    destination.matches.clear();
    for (const auto raw_point : points) {
      require(raw_point.isObject(), "Expected point correspondence object");
      const auto point = raw_point.toObject();
      hm::stitching::FeatureMatch pair;
      int camera = 0;
      for (const auto key : {"left", "right"}) {
        require(point.value(key).isArray(), "Expected original-camera pixel coordinates");
        const auto xy = point.value(key).toArray();
        require(xy.size() == 2 && xy[0].isDouble() && xy[1].isDouble(), "Expected two pixel coordinates");
        const double x = xy[0].toDouble(), y = xy[1].toDouble();
        require(
            std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 && x < destination.sizes[camera].width &&
                y < destination.sizes[camera].height,
            "Point is outside original camera image");
        (camera++ ? pair.right : pair.left) = cv::Point2f(x, y);
      }
      pair.score = Number(point, "score", 0, 1, 1);
      destination.matches.push_back(pair);
    }
  }
  return saved;
}
QJsonObject matches_result(
    const Request& request,
    const hm::stitching::CalibrationMatchSet& saved,
    QJsonArray& artifacts) {
  QJsonArray frames;
  for (std::size_t index = 0; index < saved.frames.size(); ++index) {
    const auto& frame = saved.frames[index];
    QJsonArray points, images, sizes;
    for (int camera = 0; camera < 2; ++camera) {
      images.append(text(Relative(request, frame.images[camera])));
      sizes.append(QJsonArray{frame.sizes[camera].width, frame.sizes[camera].height});
      artifacts.append(Artifact(request, frame.images[camera], "image"));
    }
    for (const auto& pair : frame.matches)
      points.append(
          QJsonObject{
              {"left", QJsonArray{pair.left.x, pair.left.y}},
              {"right", QJsonArray{pair.right.x, pair.right.y}},
              {"score", pair.score}});
    frames.append(QJsonObject{{"index", int(index)}, {"images", images}, {"sizes", sizes}, {"matches", points}});
  }
  return {
      {"fingerprint", text(saved.fingerprint)},
      {"input_fingerprint", text(saved.input_fingerprint)},
      {"manual", saved.manual},
      {"frames", frames}};
}

StitchingExperimentSettings settings(const QJsonObject& object) {
  StitchingExperimentSettings result;
  result.control_points = integer(object, "control_points", 10, 5000, 100);
  result.frame_count = integer(object, "frame_count", 1, 16, 1);
  result.stitch_frame_time = RequiredString(object, "stitch_frame_time", 32).toStdString();
  qint64 milliseconds;
  require(hm::ui::ParseHighlightTime(text(result.stitch_frame_time), &milliseconds), "Invalid calibration timestamp");
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
  require(request.payload.value("workspace").isObject(), "Expected the saved native workspace descriptor");
  const auto object = request.payload.value("workspace").toObject();
  StitchingExperimentWorkspace result;
  result.root = Contained(request.root, RequiredString(object, "root"));
  result.game_directory = Contained(request.root, RequiredString(object, "game_directory"));
  result.game_id = RequiredString(object, "game_id", 255).toStdString();
  result.invalidation_id = RequiredString(object, "invalidation_id", 128).toStdString();
  require(result.game_directory.filename() == result.game_id, "Workspace game identity mismatch");
  result.settings = settings(object.value("settings").toObject());
  result.settings.camera = take(
      hm::stitching::read_stitch_camera_selection(YAML::LoadFile((result.game_directory / "config.yaml").string())));
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

void run_process(
    const QString& program,
    const QStringList& arguments,
    const QString& directory,
    const QProcessEnvironment& environment) {
  QProcess process;
  process.setWorkingDirectory(directory);
  process.setProcessEnvironment(environment);
  // The outer supervisor bounds logs and owns every descendant, including
  // plugin helpers. Forwarding prevents QProcess accumulating pipeline logs.
  process.setProcessChannelMode(QProcess::ForwardedChannels);
  process.start(program, arguments);
  require(process.waitForStarted(5000), "Could not start native experiment process");
  auto stopped_at = std::chrono::steady_clock::time_point::max();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(6);
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(100);
    const auto now = std::chrono::steady_clock::now();
    if ((interrupted || now > deadline) && stopped_at == std::chrono::steady_clock::time_point::max()) {
      process.terminate();
      stopped_at = now;
    }
    if (stopped_at != std::chrono::steady_clock::time_point::max() && now - stopped_at > std::chrono::seconds(5)) {
      process.kill();
      process.waitForFinished(5000);
      throw std::runtime_error("Native experiment cancelled or exceeded its limit");
    }
  }
  require(!interrupted, "cancelled");
  require(
      process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
      "Native experiment process failed; inspect the run log");
}

void run_experiment(const Request& request, const StitchingExperimentWorkspace& saved) {
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
    auto config = YAML::LoadFile((saved.game_directory / "config.yaml").string());
    check(hm::stitching::validate_pending_stitching_invalidation(config, saved.invalidation_id));
    YAML::Node arguments(YAML::NodeType::Sequence);
    for (const auto& argument : std::vector<std::string>{
             "--stitching-calibration-only",
             "--enable-sinks=FAKE",
             "--clean-expected-invalidation-id=" + saved.invalidation_id,
             "--options=pipeline.hmstitcher.calibration-frame-count=" + std::to_string(saved.settings.frame_count),
             "--stitch-frame-time=" + saved.settings.stitch_frame_time,
             "-t=1"})
      arguments.push_back(argument);
    if (require_mask)
      arguments.push_back("--stitching-calibration-with-ice-mask");
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
  environment.insert("HSTREAM_CALIBRATION_START_STAGE", "features");
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
  check(CompleteStitchingExperimentWorkspace(saved, require_mask));
}

QJsonObject experiments(const Request& request, QJsonArray& artifacts) {
  if (request.operation == "experiment-create" || request.operation == "experiment-edit") {
    fs::create_directories(request.root / "experiments");
    const int sequence = integer(request.payload, "sequence", 1, 1024, 1);
    StitchingExperimentWorkspace created;
    if (request.operation == "experiment-create") {
      std::vector<fs::path> authorized_media;
      require(request.authorized_media.size() <= 10000, "Too many authorized media inputs");
      for (const auto& value : request.authorized_media) {
        require(value.isString() && !value.toString().contains(QChar(0)), "Invalid authorized media path");
        authorized_media.emplace_back(value.toString().toStdString());
      }
      created = take(CreateStagedStitchingExperimentWorkspace(
          request.game,
          request.root / "experiments",
          settings(request.payload.value("settings").toObject()),
          sequence,
          authorized_media));
    } else {
      const auto saved = workspace(request);
      auto source = request;
      source.game = saved.game_directory;
      created = take(CreateEditedStitchingExperimentWorkspace(
          saved,
          request.root / "experiments",
          fingerprint(request.payload).toStdString(),
          matches(source, true),
          sequence));
    }
    return {{"workspace", workspace_json(request, created)}, {"state", "queued"}};
  }
  const auto saved = workspace(request);
  if (request.operation == "experiment-run") {
    run_experiment(request, saved);
    return {{"workspace", workspace_json(request, saved)}, {"state", "complete"}};
  }
  if (request.operation == "experiment-complete") {
    check(CompleteStitchingExperimentWorkspace(saved, request.payload.value("require_ice_mask").toBool(false)));
    return {{"workspace", workspace_json(request, saved)}, {"state", "complete"}};
  }
  if (request.operation == "experiment-promote") {
    check(PromoteStitchingExperiment(saved, request.game));
    return {{"workspace", workspace_json(request, saved)}, {"promoted", true}};
  }
  const auto inspection = take(InspectStitchingExperimentFrames(saved));
  QJsonArray frames;
  for (const auto& frame : inspection.frames) {
    QJsonArray thumbnails, matches;
    for (const auto& path : frame.thumbnails)
      if (!path.empty()) {
        thumbnails.append(text(Relative(request, path)));
        artifacts.append(Artifact(request, path, "image"));
      }
    for (const auto& path : frame.match_images)
      if (!path.empty() && fs::exists(path)) {
        matches.append(text(Relative(request, path)));
        artifacts.append(Artifact(request, path, "image"));
      }
    frames.append(
        QJsonObject{
            {"timeline_ns", QString::number(frame.timeline_ns)},
            {"thumbnails", thumbnails},
            {"match_images", matches},
            {"eligible_people", int(frame.eligible_people)},
            {"quality", frame.quality}});
  }
  return {
      {"workspace", workspace_json(request, saved)},
      {"fingerprint", text(inspection.fingerprint)},
      {"player_selected", inspection.player_selected},
      {"source_validation", text(inspection.source_validation)},
      {"frames", frames}};
}

QJsonObject tracks(const Request& request, QJsonArray& artifacts) {
  const auto database = qs(Contained(request.root, RequiredString(request.payload, "database")));
  QString error;
  if (request.operation == "track-runs") {
    QStringList games;
    if (!request.payload.value("game").toString().isEmpty()) {
      games.append(RequiredString(request.payload, "game", 255));
    } else {
      hm::recording::Database db(database.toStdString());
      db.Validate();
      hm::recording::Statement query(db.get(), "SELECT DISTINCT game_id FROM runs ORDER BY game_id LIMIT 1025");
      while (query.Next())
        games.append(text(query.Text(0)));
      require(games.size() <= 1024, "Telemetry database exceeds its game count bound");
    }
    QJsonArray result;
    for (const auto& game : games) {
      const auto runs = hm::ui::HighlightTrackingRuns(database, game, &error);
      require(error.isEmpty(), error.toStdString());
      for (const auto& run : runs) {
        require(result.size() < 10000, "Telemetry database exceeds its run count bound");
        result.append(run);
      }
    }
    return {{"runs", result}};
  }
  const auto route = RequiredString(request.payload, "route", 32);
  require(route == "program" || route == "stitched" || route == "program-full", "Unsupported archive geometry route");
  const auto run_id = RequiredString(request.payload, "run_id", 128);
  const auto offset = integer(request.payload, "pts_offset_ms", -86400000, 86400000, 0);
  if (request.operation == "track-choices") {
    QVector<hm::ui::HighlightTrackChoice> choices;
    if (!hm::ui::HighlightTrackingChoices(
            database, run_id, route, integer(request.payload, "game_ms", 0, 86400000, 0), offset, &choices, &error))
      throw std::runtime_error(error.toStdString());
    QJsonArray result;
    for (const auto& choice : choices)
      result.append(
          QJsonObject{
              {"id", choice.id},
              {"geometry", choice.geometry},
              {"seek", choice.seek},
              {"reset", choice.reset},
              {"source", choice.source},
              {"box", QJsonArray{choice.box.x(), choice.box.y(), choice.box.width(), choice.box.height()}}});
    return {{"choices", result}};
  }
  auto plan = load_plan(request);
  const int item = integer(request.payload, "item_index", 0, plan.intervals.size() - 1, -1);
  require(item >= 0, "Select a reel item");
  const int cue = integer(request.payload, "cue_index", 0, plan.intervals[item].annotations.size() - 1, -1);
  require(cue >= 0, "Select an annotation");
  auto& annotation = plan.intervals[item].annotations[cue];
  const auto selection = request.payload.value("choice").toObject();
  hm::ui::HighlightTrackChoice choice;
  choice.id = RequiredString(selection, "id", 128);
  choice.geometry = integer(selection, "geometry", 0, 9007199254740991LL, 0);
  choice.seek = integer(selection, "seek", 0, 9007199254740991LL, 0);
  choice.reset = integer(selection, "reset", 0, 9007199254740991LL, 0);
  choice.source = integer(selection, "source", 0, 9007199254740991LL, 0);
  annotation.database = database;
  annotation.run_id = run_id;
  annotation.telemetry_offset_ms = offset;
  annotation.archive_path = plan.archive_path;
  annotation.archive_origin_ms = integer(request.payload, "archive_offset_ms", 0, 86400000, 0);
  const QFileInfo archive(plan.archive_path);
  require(archive.isFile(), "The bound archive is missing");
  annotation.archive_size = archive.size();
  annotation.archive_mtime_ms = archive.lastModified().toMSecsSinceEpoch();
  if (!hm::ui::CaptureHighlightTrack(&annotation, choice, route, &error))
    throw std::runtime_error(error.toStdString());
  // Return portable run-relative paths rather than exposing worker locations.
  plan.archive_path = text(Relative(request, plan.archive_path.toStdString()));
  for (auto& interval : plan.intervals) {
    for (auto* logo : {&interval.card.logo_a, &interval.card.logo_b})
      if (!logo->isEmpty())
        *logo = text(Relative(request, logo->toStdString()));
    for (auto& mark : interval.annotations) {
      if (!mark.database.isEmpty())
        mark.database = text(Relative(request, mark.database.toStdString()));
      if (!mark.archive_path.isEmpty())
        mark.archive_path = text(Relative(request, mark.archive_path.toStdString()));
    }
  }
  const auto path = request.root / "tracked-plan.json";
  if (!hm::ui::SaveHighlightPlan(qs(path), plan, &error))
    throw std::runtime_error(error.toStdString());
  artifacts.append(Artifact(request, path, "metadata"));
  return {{"plan", ReadObject(path)}};
}

QJsonArray trajectory(const std::vector<hm::playtracker_replay::Frame>& frames) {
  require(frames.size() <= 50000, "Replay exceeds its sample bound");
  QJsonArray result;
  for (const auto& frame : frames) {
    const auto box = [](const auto& value) -> QJsonValue {
      return value ? QJsonValue(QJsonArray{value->left, value->top, value->right, value->bottom}) : QJsonValue();
    };
    result.append(
        QJsonObject{
            {"sample_id", QString::number(frame.sample_id)},
            {"pts_ns", QString::number(frame.pts_ns)},
            {"fast", box(frame.fast)},
            {"follower", box(frame.follower)},
            {"left_rotation", frame.edge_rotation_left},
            {"right_rotation", frame.edge_rotation_right}});
  }
  return result;
}
QJsonObject replay(const Request& request) {
  hm::playtracker_replay::PrepareOptions options;
  options.manifest_path = Contained(request.root, RequiredString(request.payload, "database")).string();
  options.run_id = RequiredString(request.payload, "run_id", 128).toStdString();
  options.start_seconds = Number(request.payload, "start_seconds", 0, 86400, 0);
  options.duration_seconds = Number(request.payload, "duration_seconds", 0.001, 120, 10);
  auto session = take(hm::playtracker_replay::ReplaySession::Prepare(options));
  YAML::Node config;
  auto tracker = config["play-tracker"];
  tracker["live-boxes"].push_back(YAML::Node(YAML::NodeType::Map));
  tracker["hstream-apply-to-fast-box"] = request.payload.value("apply_to_fast_box").toBool(false);
  tracker["hstream-apply-to-follower-box"] = request.payload.value("apply_to_follower_box").toBool(true);
  tracker["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
  const auto tuning = request.payload.value("tuning").toObject();
  auto specs = tracking_control_specs([](const char*) { return 0; });
  const auto motion = motion_control_specs([](const char*) { return 0; });
  specs.insert(specs.end(), motion.begin(), motion.end());
  std::set<std::string> allowed;
  for (const auto& spec : specs) {
    if (!spec.runtime_key)
      continue;
    allowed.insert(spec.runtime_key);
    if (!tuning.contains(spec.runtime_key))
      continue;
    if (std::string(spec.runtime_key) == "cancel-stop-on-opposite-dir") {
      require(tuning.value(spec.runtime_key).isBool(), "Expected boolean camera tuning field");
      tracker["hstream-runtime-tuning"][spec.runtime_key] = tuning.value(spec.runtime_key).toBool();
    } else {
      const double value =
          Number(tuning, spec.runtime_key, double(spec.minimum) / spec.divisor, double(spec.maximum) / spec.divisor, 0);
      require(spec.divisor != 1 || std::floor(value) == value, "Expected integer camera tuning field");
      tracker["hstream-runtime-tuning"][spec.runtime_key] = value;
    }
  }
  for (const auto* key : {"ignore-largest-bbox-count", "ignore-oversized-bboxes", "oversized-bbox-percent"}) {
    allowed.insert(key);
    if (!tuning.contains(key))
      continue;
    if (std::string(key) == "ignore-oversized-bboxes") {
      require(tuning.value(key).isBool(), "Expected boolean detection tuning field");
      tracker["hstream-runtime-tuning"][key] = tuning.value(key).toBool();
    } else if (std::string(key) == "ignore-largest-bbox-count") {
      tracker["hstream-runtime-tuning"][key] = int(integer(tuning, key, 0, 2147483647, 0));
    } else {
      tracker["hstream-runtime-tuning"][key] = Number(tuning, key, 0, 1000000, 0);
    }
  }
  for (auto it = tuning.begin(); it != tuning.end(); ++it)
    require(allowed.count(it.key().toStdString()), "Unsupported native camera tuning field");
  const auto parameters = take(DsPlayTrackerLoadRuntimeTuningContents(YAML::Dump(config)));
  const auto trial = take(session->RunTrial("Web experiment", parameters));
  return {
      {"canvas", QJsonArray{int(session->width()), int(session->height())}},
      {"provenance", text(session->provenance())},
      {"end_pts_ns", QString::number(session->end_pts_ns())},
      {"original", trajectory(session->original())},
      {"baseline", trajectory(session->baseline())},
      {"trial", trajectory(trial.frames)}};
}

void retain_calibration_artifacts(const Request& request, QJsonArray& artifacts) {
  if (request.operation.rfind("matches-", 0) != 0 && request.operation.rfind("experiment-", 0) != 0)
    return;
  std::size_t count = 0;
  for (const auto& directory : {request.game, request.root / "experiments"}) {
    if (!fs::exists(directory))
      continue;
    for (fs::recursive_directory_iterator iterator(directory), end; iterator != end; ++iterator) {
      const auto& path = iterator->path();
      if (iterator->is_symlink()) {
        iterator.disable_recursion_pending();
        continue;
      }
      const auto name = path.filename().string();
      if (iterator->is_directory() && name.size() > 3 && name.rfind("cam", 0) == 0 &&
          std::all_of(name.begin() + 3, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
        iterator.disable_recursion_pending();
        continue;
      }
      if (!iterator->is_regular_file() || iterator->file_size() == 0 || path.extension() == ".mp4" ||
          path.extension() == ".MP4" || name == ".lock" || name.back() == '~')
        continue;
      require(++count <= 10000, "Calibration output exceeds its file count bound");
      const auto extension = path.extension().string();
      const QString kind = extension == ".png" || extension == ".jpg" || extension == ".tif" ? "image" : "metadata";
      artifacts.append(Artifact(request, path, kind));
    }
  }
}

QJsonObject execute(const Request& request, QJsonArray& artifacts) {
  if (request.operation.rfind("highlight-", 0) == 0)
    return highlights(request, artifacts);
  if (request.operation.rfind("experiment-", 0) == 0)
    return experiments(request, artifacts);
  if (request.operation.rfind("track-", 0) == 0)
    return tracks(request, artifacts);
  if (request.operation == "camera-replay")
    return replay(request);
  auto source = request;
  if (request.payload.contains("workspace"))
    source.game = workspace(request).game_directory;
  auto saved = matches(source, request.operation == "matches-publish");
  if (request.operation == "matches-publish") {
    saved.automatic_fingerprint = saved.manual ? saved.automatic_fingerprint : saved.fingerprint;
    saved.fingerprint.clear();
    saved.manual = true;
    saved.fingerprint = take(hm::stitching::PublishCalibrationMatches(source.game, saved));
  }
  auto result = matches_result(request, saved, artifacts);
  if (request.payload.contains("workspace"))
    result.insert("workspace", request.payload.value("workspace"));
  return result;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string(argv[1]) != "--request") {
    std::cerr << "usage: hstream-web-job --request JOB_ROOT/operation-request.json\n";
    return 64;
  }
  std::optional<Request> request;
  bool previous_receipt = false;
  std::signal(SIGTERM, cancel);
  std::signal(SIGINT, cancel);
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  try {
    request = hm::web::LoadRequest(argv[2]);
    const auto hash = QCryptographicHash::hash(
                          QJsonDocument(ReadObject(argv[2])).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
                          .toHex();
    const auto receipt = request->root / "operation-result.json";
    previous_receipt = fs::exists(receipt);
    if (previous_receipt) {
      const auto previous = ReadObject(receipt);
      require(
          previous.value("request_sha256").toString() == QString::fromLatin1(hash),
          "A different operation already used this run");
      require(
          previous.value("state").toString() == "complete", "This operation already stopped; queue a new run to retry");
      return 0;
    }
    QJsonArray artifacts;
    const auto result = execute(*request, artifacts);
    retain_calibration_artifacts(*request, artifacts);
    require(!interrupted, "cancelled");
    const auto data = request->root / "operation-data.json";
    WriteObject(data, result);
    artifacts.append(Artifact(*request, data, "metadata"));
    // Some native inspection bundles reference the same immutable image twice.
    // Deduplicate receipt membership; files are never double-published.
    QJsonArray unique;
    std::set<QString> paths;
    for (const auto artifact : artifacts)
      if (paths.insert(artifact.toObject().value("path").toString()).second)
        unique.append(artifact);
    WriteObject(
        receipt,
        {{"schema", 1},
         {"state", "complete"},
         {"runner_succeeded", true},
         {"run_id", text(request->run_id)},
         {"operation", text(request->operation)},
         {"request_sha256", QString::fromLatin1(hash)},
         {"artifacts", unique},
         {"result", result}});
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (request && !previous_receipt) {
      try {
        WriteObject(
            request->root / "operation-result.json",
            {{"schema", 1},
             {"state", interrupted ? "cancelled" : "failed"},
             {"runner_succeeded", false},
             {"run_id", text(request->run_id)},
             {"operation", text(request->operation)},
             {"error", QString::fromUtf8(error.what()).left(4000)},
             {"artifacts", QJsonArray{}}});
      } catch (const std::exception& receipt_error) {
        std::cerr << "Could not persist failure receipt: " << receipt_error.what() << '\n';
      }
    }
    return interrupted ? 130 : 1;
  }
}
