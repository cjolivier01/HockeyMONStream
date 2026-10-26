// Thin headless owners of the same native libraries used by the desktop dialogs.
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/playtracker_replay/ReplaySession.h"
#include "hstream/src/libs/recording/Database.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/CalibrationImage.h"
#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include "src/apps/hstream-ui/HighlightScene.h"
#include "src/apps/hstream-ui/HighlightTracking.h"
#include "src/apps/hstream-ui/HighlightWebPreview.h"
#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"
#include "src/apps/hstream-ui/StitchingExperimentBackend.h"
#include "src/apps/hstream-ui/WebExperimentFrames.h"
#include "src/apps/hstream-ui/WebJobSpec.h"
#include "src/apps/hstream-ui/WebRinkEditor.h"
#include "src/apps/hstream-ui/WebRinkMap.h"
#include "src/apps/hstream-ui/WebSavedCalibration.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSaveFile>
#include <QtGui/QGuiApplication>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <iterator>
#include <limits>
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
unsigned preview_port = 0; // Assigned only by the supervisor's command line.
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

hm::ui::ArchiveMediaInfo probe(const fs::path& archive, bool metadata_only = false) {
  QProcess process;
  QStringList arguments{"-v", "error"};
  if (metadata_only)
    arguments << "-protocol_whitelist" << "file" << "-f" << "mov" << "-enable_drefs" << "0"
              << "-use_absolute_path" << "0";
  arguments << "-show_streams" << "-show_format" << "-of" << "json" << qs(archive);
  process.start("ffprobe", arguments);
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
  errors += process.readAllStandardError();
  require(output.size() + errors.size() <= 1024 * 1024, "Archive inspection exceeded its output limit");
  require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "Archive inspection failed");
  hm::ui::ArchiveMediaInfo media;
  QString error;
  if (!hm::ui::ParseArchiveMediaInfo(output, &media, &error))
    throw std::runtime_error(error.toStdString());
  return media;
}

hm::ui::ArchiveMediaInfo card_archive(const Request& request) {
  const auto source = request.runtime.value("archive_probe").toObject();
  const auto name = RequiredString(source, "path", 4096);
  const fs::path path(name.toStdString());
  require(
      path.is_absolute() && path.lexically_normal() == path && request.authorized_media.size() == 1 &&
          request.authorized_media.at(0).isString() && request.authorized_media.at(0).toString() == name,
      "Card preview requires exactly its frozen authorized archive");
  const auto expected = source.value("identity").toObject();
  require(expected.size() == 5, "Card archive identity is incomplete");
  // Open every directory without following aliases, then pin the immutable file
  // while the metadata-only probe reads its descriptor. The archive is neither
  // copied into this run nor retained as a symlink or hardlink after completion.
  QFile pinned;
  int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(descriptor >= 0, "Cannot open the archive filesystem root");
  require(pinned.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle), "Cannot own archive root");
  for (auto component = path.begin(); component != path.end(); ++component) {
    if (*component == path.root_directory())
      continue;
    require(*component != "." && *component != "..", "Invalid card archive path");
    const bool last = std::next(component) == path.end();
    descriptor = ::openat(
        pinned.handle(), component->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY));
    require(descriptor >= 0, "Cannot open the frozen card archive without following links");
    pinned.close();
    require(pinned.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle), "Cannot own archive input");
  }
  const auto matches = [&] {
    struct stat info{};
    require(::fstat(pinned.handle(), &info) == 0 && S_ISREG(info.st_mode), "Card archive is not a regular file");
    const QJsonObject actual{
        {"dev", QString::number(info.st_dev)},
        {"ino", QString::number(info.st_ino)},
        {"size", QString::number(info.st_size)},
        {"mtime_ns", QString::number(qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec)},
        {"ctime_ns", QString::number(qint64(info.st_ctim.tv_sec) * 1000000000 + info.st_ctim.tv_nsec)}};
    require(actual == expected, "Card archive identity changed before or during inspection");
  };
  matches();
  const auto media =
      probe(fs::path("/proc") / std::to_string(::getpid()) / "fd" / std::to_string(pinned.handle()), true);
  matches();
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
  if (request.operation == "highlight-card-preview") {
    require(
        plan.intervals.size() == 1 && plan.intervals.front().is_card && !request.payload.contains("selected_indices"),
        "Card preview requires exactly one authored card");
    require(plan.archive_path.isEmpty(), "Card preview accepts only its frozen archive probe");
    QSize output_size;
    if (request.runtime.contains("archive_probe")) {
      const auto media = card_archive(request);
      output_size = QSize(media.width, media.height);
    }
    QImage image;
    QString error;
    require(!interrupted, "cancelled");
    if (!hm::ui::RasterHighlightCard(
            plan.intervals.front().card, qs(request.root), &image, &error, output_size))
      throw std::runtime_error(error.toStdString());
    // Without an archive, use the same intrinsic card canvas as the Qt editor.
    if (!output_size.isValid())
      output_size = image.size();
    require(!interrupted, "cancelled");
    const auto path = request.root / "highlight-card.png";
    require(!fs::exists(path), "Refusing to replace an existing card preview");
    QSaveFile output(qs(path));
    require(output.open(QIODevice::WriteOnly), "Cannot create the card preview");
    require(
        image.save(&output, "PNG") && output.flush() && ::fsync(output.handle()) == 0 && output.commit(),
        "Cannot publish the card preview");
    artifacts.append(Artifact(request, path, "image"));
    return {
        {"preview", "highlight-card.png"},
        {"width", image.width()},
        {"height", image.height()},
        {"source_width", output_size.width()},
        {"source_height", output_size.height()}};
  }
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
  const bool live = request.operation == "highlight-live-preview";
  if (request.payload.contains("inspect_game_ms")) {
    require(live, "Authored frame inspection requires live preview");
    job.inspect_game_ms = integer(request.payload, "inspect_game_ms", 0, 86400000, -1);
  }
  if (!job.archive_path.isEmpty())
    job.media = probe(job.archive_path.toStdString());
  else {
    job.media.width = 1920;
    job.media.height = 1080;
    job.media.frame_rate = 30;
  }
  if (live) {
    require(request.payload.value("loop").isBool(), "Live highlight preview requires an explicit loop choice");
    require(preview_port, "Live highlight preview requires a supervisor-assigned signaling port");
    auto config = std::make_shared<hm::ui::HighlightWebPreviewConfig>();
    config->options = take(hm::preview::ReadWebPreviewOptions(take(hm::baseline_config::load()).values));
    config->run = request.run_id;
    config->port = preview_port;
    config->stun_server = config->options.stun_server;
    job.web_preview = config;
    job.loop = request.payload.value("loop").toBool();
    const auto& o = config->options;
    const QJsonObject descriptor{
        {"version", 1},
        {"run", text(request.run_id)},
        {"stage", 0},
        {"topology", "1"},
        {"operation", "1"},
        {"channel", "program"},
        {"channels", QJsonArray{"program"}},
        {"width", int(o.width)},
        {"height", int(o.height)},
        {"seek", false},
        {"receiptLimits",
         QJsonObject{{"maxFrames", int(o.browser_max_frames)}, {"maxAgeMs", int(o.browser_max_age_ms)}}}};
    QJsonObject session{
        {"version", 1},
        {"kind", "session"},
        {"run", text(request.run_id)},
        {"stage", 0},
        {"generation", 1},
        {"presentation", descriptor}};
    if (job.inspect_game_ms >= 0) {
      const auto size = hm::ui::HighlightReelPipeline::PreviewSize(job.media);
      session.insert(
          "inspection",
          QJsonObject{{"game_ms", job.inspect_game_ms}, {"width", size.width()}, {"height", size.height()}});
    }
    std::cout << "HSTREAM_HIGHLIGHT_PREVIEW " << QJsonDocument(session).toJson(QJsonDocument::Compact).constData()
              << std::endl;
  } else {
    require(!preview_port, "A signaling port is only valid for live highlight preview");
    job.output_path = qs(request.root / "highlights.mp4");
    require(!fs::exists(job.output_path.toStdString()), "Refusing to replace an existing reel output");
  }
  job.items = selected;
  job.archive_offset_ms = integer(request.payload, "archive_offset_ms", 0, 86400000, 0);
  hm::ui::HighlightReelPipeline pipeline;
  QString error;
  if (!pipeline.Start(std::move(job), &error))
    throw std::runtime_error(error.toStdString());
  while (true) {
    if (interrupted)
      pipeline.Cancel();
    const auto status = pipeline.Poll();
    if (live) {
      std::cout << "HSTREAM_HIGHLIGHT_PREVIEW "
                << QJsonDocument(
                       QJsonObject{
                           {"version", 1},
                           {"kind", "state"},
                           {"run", text(request.run_id)},
                           {"stage", 0},
                           {"generation", 1},
                           {"position_ms", status.position_ms},
                           {"duration_ms", duration},
                           {"item", status.item},
                           {"waiting", status.waiting_for_browser},
                           {"finished", status.finished},
                           {"presented", QString::number(status.presented_frames)}})
                       .toJson(QJsonDocument::Compact)
                       .constData()
                << std::endl;
    }
    if (status.finished) {
      require(!status.cancelled && !interrupted, "cancelled");
      require(status.error.isEmpty(), status.error.toStdString());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!live)
    artifacts.append(Artifact(request, request.root / "highlights.mp4", "video"));
  return {{"duration_ms", duration}, {"items", selected.size()}};
}

hm::stitching::CalibrationMatchSet matches(const Request& request, bool edit) {
  auto payload = request.payload;
  if (!edit && payload.value("fingerprint").toString().isEmpty()) {
    const auto config = YAML::LoadFile((request.game / "config.yaml").string());
    const auto snapshot = config["hstream_ui"]["stitching_calibration"]["match_snapshot"].as<std::string>("");
    require(
        !snapshot.empty(),
        "This calibration has no editable control-point snapshot. Run stitching preparation again with match inspection enabled.");
    payload.insert("fingerprint", text(snapshot));
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
      pair.left_index = integer(point, "left_index", -1, std::numeric_limits<int>::max(), -1);
      pair.right_index = integer(point, "right_index", -1, std::numeric_limits<int>::max(), -1);
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
    QJsonArray points, images, sizes, source_seconds, source_identity, source_names;
    for (int camera = 0; camera < 2; ++camera) {
      images.append(text(Relative(request, frame.images[camera])));
      sizes.append(QJsonArray{frame.sizes[camera].width, frame.sizes[camera].height});
      artifacts.append(Artifact(request, frame.images[camera], "image"));
      source_seconds.append(frame.source_seconds[camera]);
      source_names.append(text(fs::path(frame.source_paths[camera]).filename().string()));
      source_identity.append(
          QString::fromLatin1(
              QCryptographicHash::hash(
                  QByteArray::fromStdString(frame.source_paths[camera]), QCryptographicHash::Sha256)
                  .toHex()));
    }
    for (const auto& pair : frame.matches)
      points.append(
          QJsonObject{
              {"left", QJsonArray{pair.left.x, pair.left.y}},
              {"right", QJsonArray{pair.right.x, pair.right.y}},
              {"score", pair.score},
              {"left_index", pair.left_index},
              {"right_index", pair.right_index}});
    frames.append(
        QJsonObject{
            {"index", int(index)},
            {"images", images},
            {"sizes", sizes},
            {"matches", points},
            {"source_seconds", source_seconds},
            {"source_identity", source_identity},
            {"source_names", source_names}});
  }
  return {
      {"fingerprint", text(saved.fingerprint)},
      {"input_fingerprint", text(saved.input_fingerprint)},
      {"manual", saved.manual},
      {"frames", frames}};
}

QJsonObject editable_matches_result(const Request& request, const Request& source, QJsonArray& artifacts) {
  const auto saved = matches(source, false);
  const auto automatic = saved.manual
      ? take(hm::stitching::LoadCalibrationMatches(source.game, saved.automatic_fingerprint, false))
      : saved;
  bool compatible = !saved.frames.empty() && saved.frames.size() == automatic.frames.size() &&
      saved.input_fingerprint == automatic.input_fingerprint;
  for (size_t i = 0; compatible && i < saved.frames.size(); ++i)
    compatible = saved.frames[i].sizes == automatic.frames[i].sizes &&
        saved.frames[i].source_paths == automatic.frames[i].source_paths &&
        saved.frames[i].source_seconds == automatic.frames[i].source_seconds &&
        (!saved.input_fingerprint.empty() || saved.frames[i].images == automatic.frames[i].images);
  auto result = matches_result(request, saved, artifacts);
  result.insert("match_editor_version", 1);
  result.insert("automatic", matches_result(request, automatic, artifacts));
  result.insert("automatic_compatible", compatible);
  result.insert(
      "editor_limits",
      QJsonObject{
          {"maximum_matches_per_pair", int(hm::stitching::kMaximumEditableMatchesPerPair)},
          {"maximum_image_bytes", qint64(hm::stitching::kMaximumEditableImageBytes)},
          {"maximum_image_pixels", qint64(hm::stitching::kMaximumEditableImagePixels)},
          {"maximum_image_dimension", hm::stitching::kMaximumEditableImageDimension}});
  return result;
}

void merge_settings(YAML::Node destination, const YAML::Node& overlay) {
  for (const auto& entry : overlay) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && destination[key].IsMap())
      merge_settings(destination[key], entry.second);
    else
      destination[key] = YAML::Clone(entry.second);
  }
}

QJsonObject camera_json(const hm::stitching::StitchCameraSelection& camera) {
  YAML::Node saved;
  hm::stitching::write_stitch_camera_selection(saved, camera);
  const auto optics = saved["stitching"], lens = optics["input_lens"];
  QJsonObject input;
  for (const char* key : {"projection", "optimization"})
    input.insert(key, text(lens[key].as<std::string>()));
  for (const char* key : {"distortion", "center_offset"}) {
    QJsonArray components;
    for (const auto& value : lens[key])
      components.append(value.as<double>());
    input.insert(key, components);
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
      {"camera_automation", automation}};
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
    const std::set<QString> allowed{"camera_config", "camera_fov", "input_lens", "camera_automation"};
    for (auto item = camera.begin(); item != camera.end(); ++item)
      require(allowed.count(item.key()), "Unsupported experiment camera setting");
    // A profile-only selection resolves that profile's optics, rather than
    // carrying a previous profile's explicit FOV/lens correction into it.
    if (camera.contains("camera_config")) {
      effective["stitching"].remove("camera_fov");
      effective["stitching"].remove("input_lens");
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
    const auto config = YAML::LoadFile((request.game / "config.yaml").string());
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
  result.settings =
      settings(object.value("settings").toObject(), YAML::LoadFile((result.game_directory / "config.yaml").string()));
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

void run_experiment(const Request& request, const StitchingExperimentWorkspace& saved, bool complete = true) {
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
  if (complete)
    check(CompleteStitchingExperimentWorkspace(saved, require_mask));
}

QJsonObject experiments(const Request& request, QJsonArray& artifacts) {
  if (request.operation == "experiment-create" || request.operation == "experiment-edit" ||
      request.operation == "experiment-prepare-edit" || request.operation == "experiment-save-matches" ||
      request.operation == "experiment-recover") {
    fs::create_directories(request.root / "experiments");
    const int sequence = integer(request.payload, "sequence", 1, 1024, 1);
    StitchingExperimentWorkspace created;
    if (request.operation == "experiment-create" || request.operation == "experiment-recover") {
      std::vector<fs::path> authorized_media;
      require(request.authorized_media.size() <= 10000, "Too many authorized media inputs");
      for (const auto& value : request.authorized_media) {
        require(value.isString() && !value.toString().contains(QChar(0)), "Invalid authorized media path");
        authorized_media.emplace_back(value.toString().toStdString());
      }
      fs::path source_game = request.game;
      if (request.operation == "experiment-create" && request.payload.contains("source_workspace")) {
        auto source = request;
        source.payload["workspace"] = request.payload.value("source_workspace");
        source_game = workspace(source).game_directory;
      }
      created = take(CreateStagedStitchingExperimentWorkspace(
          source_game,
          request.root / "experiments",
          settings(
              request.payload.value("settings").toObject(), YAML::LoadFile((source_game / "config.yaml").string())),
          sequence,
          authorized_media,
          !request.payload.value("settings").toObject().value("camera").toObject().isEmpty()));
    } else if (request.operation == "experiment-prepare-edit") {
      created = take(CreateStitchingExperimentEditableCopy(workspace(request), request.root / "experiments", sequence));
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
    if (request.operation == "experiment-create" || request.operation == "experiment-edit") {
      const auto selection =
          take(ReusableStitchingExperimentSelectionFingerprint(created.game_directory, created.settings));
      return {
          {"workspace", workspace_json(request, created)},
          {"state", "queued"},
          {"selection_available", !selection.empty()},
          {"selection_fingerprint", text(selection)}};
    }
    const bool recovering = request.operation == "experiment-recover";
    bool complete = false;
    try {
      run_experiment(request, created, !recovering);
      complete = !recovering;
    } catch (const std::exception&) {
      // Recovery prepares immutable original images even when automatic
      // matching cannot solve them. It never advertises a solved calibration.
      // Cancellation and ordinary Save & recalibrate failures remain failures.
      if (interrupted || (!recovering && request.operation != "experiment-prepare-edit"))
        throw;
    }
    auto source = request;
    source.game = created.game_directory;
    source.payload.remove("fingerprint");
    auto result = editable_matches_result(request, source, artifacts);
    result.insert("workspace", workspace_json(request, created));
    result.insert("state", complete ? "complete" : "editable");
    result.insert("calibration_complete", complete);
    const auto selection =
        take(ReusableStitchingExperimentSelectionFingerprint(created.game_directory, created.settings));
    result.insert("selection_available", !selection.empty());
    result.insert("selection_fingerprint", text(selection));
    return result;
  }
  const auto saved = workspace(request);
  if (request.operation == "experiment-scan" || request.operation == "experiment-reuse-selection") {
    auto companion_request = request;
    const bool scan = request.operation == "experiment-scan";
    const auto key = scan ? "baseline_workspace" : "selection_workspace";
    require(request.payload.value(key).isObject(), "Expected the owned frame-selection dependency");
    companion_request.payload.insert("workspace", request.payload.value(key));
    const auto companion = workspace(companion_request);
    if (scan) {
      const auto selected = hm::web::ScanExperimentFrames(request, companion, saved, run_process);
      return {
          {"workspace", workspace_json(request, saved)},
          {"state", selected.available ? "queued" : "unavailable"},
          {"selection_available", selected.available},
          {"selection_diagnostic", text(selected.diagnostic)}};
    }
    check(ReuseStitchingExperimentPlayerSelection(companion, saved));
    return {{"workspace", workspace_json(request, saved)}, {"state", "queued"}, {"selection_available", true}};
  }
  if (request.operation == "experiment-run") {
    run_experiment(request, saved);
    return {{"workspace", workspace_json(request, saved)}, {"state", "complete"}};
  }
  if (request.operation == "experiment-complete") {
    check(CompleteStitchingExperimentWorkspace(saved, request.payload.value("require_ice_mask").toBool(false)));
    return {{"workspace", workspace_json(request, saved)}, {"state", "complete"}};
  }
  if (request.operation == "experiment-promote") {
    std::optional<StitchingExperimentBlend> blend;
    if (request.payload.contains("blend")) {
      require(request.payload.value("blend").isObject(), "Experiment blend must be an object");
      const auto object = request.payload.value("blend").toObject();
      const auto mode = RequiredString(object, "mode");
      require(mode == "laplacian" || mode == "alpha" || mode == "gpu-hard-seam", "Invalid experiment blend mode");
      blend = StitchingExperimentBlend{mode.toStdString(), std::nullopt};
      if (object.contains("feather_fraction"))
        blend->feather_fraction = Number(object, "feather_fraction", 0, 1, 0);
    }
    check(PromoteStitchingExperiment(saved, request.game, blend));
    return {
        {"workspace", workspace_json(request, saved)},
        {"promoted", true},
        {"setup_preview", hm::web::ExportSavedCalibrationPreview(request, artifacts)}};
  }
  const auto inspection = take(InspectStitchingExperimentFrames(saved));
  QString editable_snapshot;
  const auto config = YAML::LoadFile((saved.game_directory / "config.yaml").string());
  const auto snapshot = config["hstream_ui"]["stitching_calibration"]["match_snapshot"].as<std::string>("");
  if (!snapshot.empty()) {
    const auto editable = hm::stitching::LoadCalibrationMatches(saved.game_directory, snapshot);
    if (editable.ok())
      editable_snapshot = text(snapshot);
  }
  QJsonArray frames;
  for (const auto& frame : inspection.frames) {
    QJsonArray thumbnails, matches, sources, coverage, bands;
    for (const auto& path : frame.thumbnails) {
      if (!path.empty() && fs::exists(path)) {
        thumbnails.append(text(Relative(request, path)));
        artifacts.append(Artifact(request, path, "image"));
      } else {
        thumbnails.append(QJsonValue::Null);
      }
    }
    for (const auto& path : frame.match_images) {
      if (!path.empty() && fs::exists(path)) {
        matches.append(text(Relative(request, path)));
        artifacts.append(Artifact(request, path, "image"));
      } else {
        matches.append(QJsonValue::Null);
      }
    }
    for (size_t camera = 0; camera < frame.camera_paths.size(); ++camera)
      sources.append(
          QJsonObject{
              {"available", !frame.camera_paths[camera].empty()},
              // Browser users see the original media filename, never a worker path.
              {"name", text(fs::path(frame.camera_paths[camera]).filename().string())},
              {"source_seconds", frame.source_seconds[camera]},
              {"source_pts_ns", QString::number(frame.source_pts_ns[camera])},
              {"decoded_sequence", QString::number(frame.decoded_sequences[camera])}});
    for (const auto cell : frame.coverage)
      coverage.append(int(cell));
    for (const auto count : frame.size_band_counts)
      bands.append(int(count));
    frames.append(
        QJsonObject{
            {"timeline_ns", QString::number(frame.timeline_ns)},
            {"thumbnails", thumbnails},
            {"match_images", matches},
            {"sources", sources},
            {"coverage", coverage},
            {"size_band_counts", bands},
            {"eligible_people", int(frame.eligible_people)},
            {"quality", frame.quality}});
  }
  QJsonArray colors;
  for (const auto* color : kStitchingExperimentCoverageColors)
    colors.append(color);
  return {
      {"inspection_version", 1},
      {"workspace", workspace_json(request, saved)},
      {"editable", !editable_snapshot.isEmpty()},
      {"editable_snapshot", editable_snapshot},
      {"calibration_complete",
       config["hstream_ui"]["stitching_calibration"]["status"].as<std::string>("") == "complete"},
      {"fingerprint", text(inspection.fingerprint)},
      {"player_selected", inspection.player_selected},
      {"expected_pair_count", int(inspection.expected_pair_count)},
      {"anchor_ns", QString::number(inspection.anchor_ns)},
      {"coverage_grid",
       QJsonObject{
           {"columns", int(kStitchingExperimentCoverageColumns)},
           {"rows", int(kStitchingExperimentCoverageRows)},
           {"colors", colors}}},
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
  if (request.operation != "calibration-complete" && request.operation.rfind("matches-", 0) != 0 &&
      request.operation.rfind("experiment-", 0) != 0 && request.operation.rfind("rink-map-", 0) != 0 &&
      request.operation.rfind("calibration-view-", 0) != 0)
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
  if (request.operation.rfind("calibration-view-", 0) == 0)
    return hm::web::RunSavedCalibration(request, artifacts, [] { return interrupted != 0; });
  if (request.operation.rfind("rink-map-", 0) == 0)
    return hm::web::RunRinkMap(request, artifacts, [] { return interrupted != 0; });
  if (request.operation == "calibration-complete") {
    const auto owner = RequiredString(request.payload, "invalidation_id").toStdString();
    require(!owner.empty(), "Calibration completion requires an invalidation owner");
    require(
        !request.payload.contains("require_ice_mask") || request.payload.value("require_ice_mask").isBool(),
        "Expected boolean require_ice_mask");
    StitchingExperimentWorkspace saved;
    saved.root = request.root;
    saved.game_directory = request.game;
    saved.invalidation_id = owner;
    check(CompleteStitchingExperimentWorkspace(saved, request.payload.value("require_ice_mask").toBool(false)));
    // A single bounded proxy of an existing calibration artifact, never a
    // readback from the steady-state GPU video pipeline.
    const auto source = Contained(request.root, text(Relative(request, request.game / "panorama.tif")));
    const auto preview = readCalibrationTiff(text(source.string()), 1920);
    require(!preview.isNull(), "Calibration did not produce a readable stitched panorama");
    const auto destination = Contained(
        request.root,
        text((request.game / "setup-preview.png").lexically_relative(request.root).generic_string()),
        false);
    require(preview.save(text(destination.string()), "PNG"), "Could not publish stitched setup preview");
    return {{"state", "complete"}, {"invalidation_id", text(owner)}, {"preview", text(Relative(request, destination))}};
  }
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
  if (request.operation == "matches-read") {
    auto result = editable_matches_result(request, source, artifacts);
    result.insert("workspace", workspace_json(request, workspace(source)));
    const auto config = YAML::LoadFile((source.game / "config.yaml").string());
    result.insert(
        "calibration_complete",
        config["hstream_ui"]["stitching_calibration"]["status"].as<std::string>("") == "complete");
    return result;
  }
  auto saved = matches(source, true);
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
  if (argc == 3 && std::string(argv[1]) == "--rink-editor") {
    try {
      const auto result = hm::web::RinkEditor(ReadObject(argv[2]));
      std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 2 && std::string(argv[1]) == "--capabilities") {
    // Readiness probes need neither a display server nor a request workspace.
    std::cout << QJsonDocument(hm::web::Capabilities()).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
  }
  if ((argc != 3 && argc != 4) || std::string(argv[1]) != "--request") {
    std::cerr << "usage: hstream-web-job --request JOB_ROOT/operation-request.json | --capabilities\n";
    return 64;
  }
  if (argc == 4) {
    const std::string argument(argv[3]), prefix("--preview-port=");
    if (argument.rfind(prefix, 0) != 0)
      return 64;
    const auto digits = argument.substr(prefix.size());
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), preview_port);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || preview_port < 1024 ||
        preview_port > 65535)
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
    require(
        !preview_port || request->operation == "highlight-live-preview",
        "Signaling is only available for live highlight preview");
    const auto hash = QCryptographicHash::hash(
                          QJsonDocument(ReadObject(argv[2])).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
                          .toHex();
    const auto receipt = request->root / "operation-result.json";
    previous_receipt = fs::exists(receipt);
    if (previous_receipt) {
      const auto previous = ReadObject(receipt, hm::web::kMaximumResultBytes);
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
