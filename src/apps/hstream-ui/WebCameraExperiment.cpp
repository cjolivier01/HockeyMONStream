#include "src/apps/hstream-ui/WebCameraExperiment.h"

#include "hstream/src/libs/common/BaselineConfig.h"
#include "src/apps/hstream-ui/ArchiveCatalog.h"
#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"
#include "src/apps/hstream-ui/CameraExperimentSource.h"
#include "src/apps/hstream-ui/CameraExperimentWebPreview.h"

#include <poll.h>
#include <unistd.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace hm::web {
namespace {
namespace replay = hm::playtracker_replay;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename T>
T take(absl::StatusOr<T> value) {
  require(value.ok(), value.status().ToString());
  return std::move(*value);
}
bool boolean(const QJsonObject& value, const char* key) {
  require(value.value(key).isBool(), std::string("Expected boolean camera field ") + key);
  return value.value(key).toBool();
}
double number(const QJsonObject& value, const char* key, double minimum, double maximum) {
  require(value.contains(key), std::string("Missing camera field ") + key);
  return Number(value, key, minimum, maximum, minimum);
}
std::size_t index(const QJsonObject& value, const char* key, std::size_t maximum) {
  const auto parsed = number(value, key, 0, maximum);
  require(std::floor(parsed) == parsed, std::string("Expected integer camera field ") + key);
  return static_cast<std::size_t>(parsed);
}
void command_fields(const QJsonObject& command, const QString& action) {
  std::set<QString> allowed{"generation", "request", "run", "action"};
  const auto add = [&](std::initializer_list<const char*> keys) {
    for (const auto* key : keys)
      allowed.insert(key);
  };
  if (action == "seek")
    add({"frame"});
  else if (action == "view")
    add({"view"});
  else if (action == "source-confirm")
    add({"confirmed"});
  else if (action == "loop")
    add({"loop"});
  else if (action == "realtime")
    add({"realtime"});
  else if (action == "overlays")
    add({"players", "play", "rink"});
  else if (action == "trial")
    add(
        {"name",
         "tuning",
         "apply_to_fast_box",
         "apply_to_follower_box",
         "rink_aware",
         "anticipate",
         "frame_goals",
         "adaptive_zoom"});
  else
    require(
        action == "play" || action == "pause" || action == "stop" || action == "close" || action == "save",
        "Unsupported camera experiment command");
  require(std::size_t(command.size()) == allowed.size(), "Camera command has missing or unsupported fields");
  for (auto field = command.begin(); field != command.end(); ++field)
    require(allowed.count(field.key()), "Unsupported native camera command field");
}
DsPlayTrackerRuntimeTuning tuning(const QJsonObject& payload) {
  require(payload.value("tuning").isObject(), "Expected camera tuning overrides");
  YAML::Node config;
  auto tracker = config["play-tracker"];
  tracker["live-boxes"].push_back(YAML::Node(YAML::NodeType::Map));
  tracker["hstream-apply-to-fast-box"] = boolean(payload, "apply_to_fast_box");
  tracker["hstream-apply-to-follower-box"] = boolean(payload, "apply_to_follower_box");
  auto values = tracker["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
  auto specs = tracking_control_specs([](const char*) { return 0; });
  const auto motion = motion_control_specs([](const char*) { return 0; });
  specs.insert(specs.end(), motion.begin(), motion.end());
  const auto overrides = payload.value("tuning").toObject();
  std::set<QString> allowed;
  for (const auto& spec : specs) {
    if (!spec.runtime_key)
      continue;
    allowed.insert(spec.runtime_key);
    if (!overrides.contains(spec.runtime_key))
      continue;
    if (std::string(spec.runtime_key) == "cancel-stop-on-opposite-dir")
      values[spec.runtime_key] = boolean(overrides, spec.runtime_key);
    else {
      const auto parsed =
          number(overrides, spec.runtime_key, double(spec.minimum) / spec.divisor, double(spec.maximum) / spec.divisor);
      require(spec.divisor != 1 || std::floor(parsed) == parsed, "Expected integer camera tuning value");
      values[spec.runtime_key] = parsed;
    }
  }
  for (const auto* key : {"ignore-largest-bbox-count", "ignore-oversized-bboxes", "oversized-bbox-percent"}) {
    allowed.insert(key);
    if (!overrides.contains(key))
      continue;
    if (std::string(key) == "ignore-oversized-bboxes")
      values[key] = boolean(overrides, key);
    else if (std::string(key) == "ignore-largest-bbox-count")
      values[key] = static_cast<int>(index(overrides, key, std::numeric_limits<int>::max()));
    else
      values[key] = number(overrides, key, 0, std::numeric_limits<double>::max());
  }
  for (auto it = overrides.begin(); it != overrides.end(); ++it)
    require(allowed.count(it.key()), "Unsupported native camera tuning field");
  return take(DsPlayTrackerLoadRuntimeTuningContents(YAML::Dump(config)));
}
QJsonValue box(const std::optional<hm::BBox>& value) {
  return value ? QJsonValue(QJsonArray{value->left, value->top, value->right, value->bottom}) : QJsonValue();
}
QJsonArray frames(const std::vector<replay::Frame>& input) {
  require(input.size() <= 50000, "Camera trajectory exceeds its sample limit");
  QJsonArray result;
  for (const auto& frame : input) {
    QJsonArray players;
    if (frame.overlays)
      for (const auto& player : frame.overlays->player_rects)
        players.append(QJsonArray{player.rect.left + player.rect.width / 2, player.ignored});
    result.append(
        QJsonObject{
            {"sample", QString::number(frame.sample_id)},
            {"pts", QString::number(frame.pts_ns)},
            {"fast", box(frame.fast)},
            {"follower", box(frame.follower)},
            {"cluster", box(frame.tracking.cluster)},
            {"breakaway_target", box(frame.tracking.breakaway_target)},
            {"breakaway_edge",
             frame.tracking.breakaway_edge ? QJsonValue(frame.tracking.breakaway_edge->x) : QJsonValue()},
            {"players", players},
            {"rink_camera",
             frame.rink_camera ? QJsonValue(
                                     QJsonObject{
                                         {"active", frame.rink_camera->active},
                                         {"speed_mps", frame.rink_camera->speed_mps},
                                         {"lead_pixels", frame.rink_camera->lead_pixels},
                                         {"zoom_ratio", frame.rink_camera->zoom_ratio},
                                         {"goal", frame.rink_camera->goal}})
                               : QJsonValue()}});
  }
  return result;
}
void PublishEvent(const Request& request, QJsonObject value) {
  value.insert("version", 1);
  value.insert("run", QString::fromStdString(request.run_id));
  value.insert("stage", 0);
  value.insert("generation", 1);
  std::cout << "HSTREAM_CAMERA_EXPERIMENT " << QJsonDocument(value).toJson(QJsonDocument::Compact).constData()
            << std::endl;
}

class Commands {
 public:
  std::vector<QJsonObject> Read() {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    if (::poll(&descriptor, 1, 0) <= 0)
      return {};
    char bytes[8192];
    const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
    if (count == 0) {
      closed_ = true;
      return {};
    }
    require(count > 0, "Could not read camera commands");
    pending_.append(bytes, count);
    require(pending_.size() <= 16384, "Camera command exceeds its byte limit");
    std::vector<QJsonObject> result;
    int boundary = 0;
    while ((boundary = pending_.indexOf('\n')) >= 0) {
      const auto line = pending_.left(boundary);
      pending_.remove(0, boundary + 1);
      if (line.trimmed().isEmpty())
        continue;
      QJsonParseError error;
      const auto document = QJsonDocument::fromJson(line, &error);
      require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid camera command JSON");
      result.push_back(document.object());
      require(result.size() <= 32, "Camera command batch exceeds its limit");
    }
    return result;
  }
  bool closed() const {
    return closed_;
  }

 private:
  QByteArray pending_;
  bool closed_{false};
};
} // namespace

QJsonObject RunCameraExperiment(
    const Request& request,
    QJsonArray& artifacts,
    unsigned preview_port,
    const std::function<bool()>& interrupted) {
  require(preview_port >= 1024 && preview_port <= 65535, "Camera experiments require a supervised signaling port");
  replay::PrepareOptions options;
  options.manifest_path = Contained(request.root, RequiredString(request.payload, "database")).string();
  options.run_id = RequiredString(request.payload, "run_id", 128).toStdString();
  options.start_seconds = number(request.payload, "start_seconds", 0, 1e8);
  options.duration_seconds = number(request.payload, "duration_seconds", .1, 120);
  const auto playback_start = number(request.payload, "playback_start_seconds", 0, 1e8);
  const auto recorded_origin =
      CameraExperimentRecordingSourceOrigin(options).value_or(std::llround(playback_start * 1000));
  options.start_seconds += playback_start - recorded_origin / 1000.0;
  require(options.start_seconds >= 0, "Playback start plus Start offset precedes the selected recording");
  options.prepare_overlays = true;
  std::atomic<bool> cancelled{false};
  auto preparation =
      std::async(std::launch::async, [&] { return replay::ReplaySession::Prepare(options, &cancelled); });
  while (preparation.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready)
    if (interrupted())
      cancelled = true;
  auto session = take(preparation.get());
  require(!interrupted(), "cancelled");
  replay::MediaBinding media;
  const auto source = RequiredString(request.payload, "media_kind", 32);
  if (source == "original-cameras") {
    media.stitching = take(PrepareExperimentSources(request.game.string(), session->width(), session->height()));
    media.path = (request.game / "config.yaml").string();
    media.width = session->width();
    media.height = session->height();
  } else {
    require(source == "panorama", "Unsupported camera video source kind");
    media.path = Contained(request.root, RequiredString(request.payload, "archive_path")).string();
    media.width = index(request.payload, "media_width", 65536);
    media.height = index(request.payload, "media_height", 65536);
    require(media.width && media.height, "Camera panorama dimensions are missing");
    // The worker freezes ffprobe-verified dimensions alongside the immutable
    // media reference. Open validates their relation to the recorded canvas.
  }
  media.telemetry_origin_pts_ns = session->original().front().pts_ns;
  auto video_origin_ms = std::llround(number(request.payload, "video_origin_seconds", 0, 1e8) * 1000);
  hm::ui::ArchiveEntry archive;
  if (!media.stitching && hm::ui::LoadArchiveSidecar(QString::fromStdString(media.path), &archive) &&
      archive.start_time_known)
    video_origin_ms -= archive.start_time_ms;
  require(video_origin_ms >= 0, "Playback start plus Video start offset precedes the selected archive");
  media.video_origin_pts_ns = video_origin_ms * GST_MSECOND;
  bool confirmed = boolean(request.payload, "source_confirmed");
  bool loop = boolean(request.payload, "loop"), realtime = boolean(request.payload, "realtime");
  bool players = boolean(request.payload, "players"), play_overlay = boolean(request.payload, "play"),
       rink = boolean(request.payload, "rink");
  std::shared_ptr<const hm::gpu_preview::RinkMaskImage> mask;
  std::shared_ptr<const hm::stitching::PlaybackRinkMap> rink_map;
  std::string rink_reason = "Select Original cameras to bind a reviewed rink map to this recording.";
  if (media.stitching) {
    const auto loaded = hm::stitching::LoadPlaybackRinkMap(request.game.string(), interrupted);
    if (loaded.ok()) {
      const auto rotation = session->RinkCameraRotation(**loaded);
      if (rotation.ok() && std::abs(*rotation - media.stitching->rotation) < 1e-8) {
        rink_map = *loaded;
        rink_reason.clear();
      } else
        rink_reason = rotation.ok() ? "The selected cameras use a different rotation from this recording."
                                    : rotation.status().ToString();
    } else
      rink_reason = loaded.status().ToString();
  }
  if (!session->rink_mask_png().empty()) {
    const auto& png = session->rink_mask_png();
    hm::gpu_preview::RinkMaskLoadOptions bounds;
    bounds.downsample_to_texture_budget = true;
    bounds.maximum_source_dimension =
        std::max(hm::gpu_preview::kMaximumRinkMaskDimension, std::max(session->width(), session->height()));
    bounds.maximum_source_pixels = hm::gpu_preview::kMaximumPreviewRinkMaskSourcePixels;
    bounds.maximum_resource_bytes = hm::gpu_preview::kMaximumPreviewRinkMaskResourceBytes;
    auto decoded = hm::gpu_preview::decode_rink_mask_png({png.begin(), png.end()}, {}, bounds);
    if (decoded && hm::gpu_preview::rink_mask_dimensions_match(decoded.image, session->width(), session->height()))
      mask = std::make_shared<const hm::gpu_preview::RinkMaskImage>(std::move(decoded.image));
  }
  hm::ui::HighlightWebPreviewConfig config;
  config.options = take(hm::preview::ReadWebPreviewOptions(take(hm::baseline_config::load()).values));
  config.run = request.run_id;
  config.port = preview_port;
  config.stun_server = config.options.stun_server;
  std::string error;
  auto output = MakeCameraExperimentWebPreviewOutput(config, &error);
  require(bool(output), error);
  CameraExperimentPreviewWorker preview;
  std::vector<replay::TrialResult> trials;
  std::map<std::size_t, QJsonObject> saved_trials;
  std::size_t view = 0;
  bool preview_open = false, finished = false;
  std::uint64_t last_request = 0, catalog_revision = 0;
  const auto selected_frames = [&]() -> std::shared_ptr<const std::vector<replay::Frame>> {
    if (view == 0)
      return {session, &session->original()};
    if (view == 1)
      return {session, &session->baseline()};
    return std::make_shared<const std::vector<replay::Frame>>(trials.at(view - 2).frames);
  };
  const auto publish_catalog = [&] {
    QJsonArray catalog{
        QJsonObject{{"name", "Recorded"}, {"frames", frames(session->original())}},
        QJsonObject{{"name", "Baseline replay"}, {"frames", frames(session->baseline())}}};
    for (const auto& trial : trials)
      catalog.append(QJsonObject{{"name", QString::fromStdString(trial.name)}, {"frames", frames(trial.frames)}});
    WriteObject(
        request.root / "camera-experiment-catalog.json",
        {{"version", 1},
         {"run", QString::fromStdString(request.run_id)},
         {"revision", QString::number(++catalog_revision)},
         {"canvas", QJsonArray{int(session->width()), int(session->height())}},
         {"provenance", QString::fromStdString(session->provenance())},
         {"views", catalog},
         {"mask_available", bool(mask)},
         {"source_confirmed", confirmed},
         {"rink_available", bool(rink_map)},
         {"rink_reason", QString::fromStdString(rink_reason)}});
    PublishEvent(request, {{"kind", "catalog"}, {"revision", QString::number(catalog_revision)}});
  };
  const auto seek = [&](std::size_t frame, bool playing, bool replace) {
    require(confirmed, "Playback needs source confirmation. Confirm the uncropped panorama or original cameras.");
    const auto selected = selected_frames();
    require(frame < selected->size(), "Camera frame lies outside the selected range");
    const bool open = !preview_open;
    preview_open = true;
    preview.Submit([=](CameraExperimentPreview& target, std::string* failure) {
      if (open && !target.Open(media, session->width(), session->height(), 0, 0, 0, failure, mask, output))
        return false;
      target.SetLoop(loop);
      target.SetRealtime(realtime);
      target.SetOverlays(players, play_overlay, rink);
      if (open || replace)
        return target.SetTrajectory(selected, session->end_pts_ns(), failure, playing, frame);
      return target.Seek(frame, playing, failure);
    });
  };
  const auto& o = config.options;
  const QJsonObject presentation{
      {"version", 1},
      {"run", QString::fromStdString(request.run_id)},
      {"stage", 0},
      {"topology", "1"},
      {"operation", "1"},
      {"channel", "program"},
      {"channels", QJsonArray{"program"}},
      {"width", int(o.width)},
      {"height", int(o.height)},
      {"seek", true},
      {"receiptLimits",
       QJsonObject{{"maxFrames", int(o.browser_max_frames)}, {"maxAgeMs", int(o.browser_max_age_ms)}}}};
  PublishEvent(request, {{"kind", "session"}, {"presentation", presentation}});
  publish_catalog();
  Commands commands;
  auto last_state = std::chrono::steady_clock::time_point{};
  while (!finished && !interrupted()) {
    for (const auto& command : commands.Read()) {
      QJsonObject reply{{"kind", "ack"}, {"request", command.value("request")}, {"status", "rejected"}};
      try {
        const auto request_id = index(command, "request", 9007199254740991ULL);
        require(request_id > last_request, "Stale camera command sequence");
        require(
            RequiredString(command, "run", 64).toStdString() == request.run_id && index(command, "generation", 1) == 1,
            "Camera command belongs to another session");
        last_request = request_id;
        const auto action = RequiredString(command, "action", 32);
        command_fields(command, action);
        const auto state = preview.Poll();
        if (action == "close")
          finished = true;
        else if (action == "stop") {
          preview.Close();
          preview_open = false;
        } else if (action == "pause")
          preview.Submit([](auto& target, auto*) {
            target.Pause();
            return true;
          });
        else {
          require(!state.busy && !state.seeking, "Wait for the current camera preview operation");
          if (action == "play")
            seek(state.frame.value_or(0), true, false);
          else if (action == "seek") {
            require(!state.playing, "Pause the preview before stepping or seeking");
            seek(index(command, "frame", selected_frames()->size() - 1), false, false);
          } else if (action == "view") {
            require(confirmed, "Playback needs source confirmation before changing preview views");
            view = index(command, "view", trials.size() + 1);
            seek(std::min(state.frame.value_or(0), selected_frames()->size() - 1), state.playing, true);
          } else if (action == "overlays") {
            require(
                !boolean(command, "rink") || bool(mask),
                "The selected recording has no compatible historical rink mask");
            players = boolean(command, "players");
            play_overlay = boolean(command, "play");
            rink = boolean(command, "rink");
            preview.Submit([=](auto& target, auto*) {
              target.SetOverlays(players, play_overlay, rink);
              return true;
            });
          } else if (action == "source-confirm") {
            require(!state.playing, "Pause the preview before changing source confirmation");
            confirmed = boolean(command, "confirmed");
            publish_catalog();
          } else if (action == "loop") {
            loop = boolean(command, "loop");
            preview.Submit([=](auto& target, auto*) {
              target.SetLoop(loop);
              return true;
            });
          } else if (action == "realtime") {
            require(!state.playing, "Pause the preview before changing playback pacing");
            realtime = boolean(command, "realtime");
            preview.Submit([=](auto& target, auto*) {
              target.SetRealtime(realtime);
              return true;
            });
          } else if (action == "trial") {
            require(
                !state.playing && confirmed, "Pause the preview and confirm the video source before applying a trial");
            require(
                trials.size() + (boolean(command, "rink_aware") ? 2 : 1) <= 128,
                "Camera experiment history exceeds its trial limit");
            const auto parameters = tuning(command);
            const auto name = RequiredString(command, "name", 100).toStdString();
            std::optional<replay::RinkCameraSetup> rink_setup;
            if (boolean(command, "rink_aware")) {
              require(bool(rink_map), "Prepare original cameras with a matching reviewed rink map first.");
              rink_setup = replay::RinkCameraSetup{
                  rink_map,
                  {boolean(command, "anticipate"), boolean(command, "frame_goals"), boolean(command, "adaptive_zoom")}};
            }
            auto trial = std::async(std::launch::async, [&]() -> absl::StatusOr<std::vector<replay::TrialResult>> {
              std::vector<replay::TrialResult> calculated;
              if (rink_setup) {
                auto native = session->RunTrial(name + " · native", parameters, &cancelled);
                if (!native.ok())
                  return native.status();
                calculated.push_back(std::move(*native));
              }
              auto result = session->RunTrial(
                  rink_setup ? name + " · rink-aware" : name,
                  parameters,
                  &cancelled,
                  rink_setup ? &*rink_setup : nullptr);
              if (!result.ok())
                return result.status();
              calculated.push_back(std::move(*result));
              return calculated;
            });
            while (trial.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready)
              if (interrupted())
                cancelled = true;
            for (auto& result : take(trial.get()))
              trials.push_back(std::move(result));
            view = trials.size() + 1;
            publish_catalog();
            seek(0, true, true);
          } else if (action == "save") {
            require(
                !state.playing && confirmed && view >= 2,
                "Pause, confirm the source and select a trial before saving it");
            const auto path = request.root / ("camera-trial-" + std::to_string(view - 1) + ".yaml");
            if (!saved_trials.count(view)) {
              require(!std::filesystem::exists(path), "The trial output already exists");
              const auto saved = session->SaveTrial(path.string(), trials.at(view - 2), media);
              require(saved.ok(), saved.ToString());
              saved_trials.emplace(view, Artifact(request, path, "metadata"));
              artifacts.append(saved_trials.at(view));
            }
            reply.insert("saved", QString::fromStdString(path.filename().string()));
            reply.insert("artifact", saved_trials.at(view));
          } else
            throw std::runtime_error("Unsupported camera experiment command");
        }
        reply.insert("status", "ok");
      } catch (const std::exception& failure) {
        reply.insert("error", QString::fromUtf8(failure.what()));
      }
      PublishEvent(request, reply);
    }
    require(!commands.closed(), "Camera experiment command owner disconnected");
    if (std::chrono::steady_clock::now() - last_state >= std::chrono::milliseconds(100)) {
      const auto state = preview.Poll();
      PublishEvent(
          request,
          {{"kind", "state"},
           {"busy", state.busy},
           {"seeking", state.seeking},
           {"playing", state.playing},
           {"open", state.open},
           {"frame", state.frame ? QJsonValue(int(*state.frame)) : QJsonValue()},
           {"view", int(view)},
           {"operation", QString::number(output->PresentationOperation())},
           {"error", QString::fromStdString(state.error)},
           {"loop", loop},
           {"realtime", realtime},
           {"players", players},
           {"play", play_overlay},
           {"rink", rink}});
      last_state = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  preview.Close();
  while (preview.Poll().busy)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  require(!interrupted(), "cancelled");
  return {{"trials", int(trials.size())}, {"closed", true}};
}
} // namespace hm::web
