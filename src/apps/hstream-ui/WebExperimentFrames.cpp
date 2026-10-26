#include "src/apps/hstream-ui/WebExperimentFrames.h"
#include "src/apps/hstream-ui/StitchingExperimentDefaults.h"

#include <cmath>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/common/BaselineConfig.h"

namespace hm::web {
namespace {
void check(const absl::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}
QString path_string(const std::filesystem::path& path) {
  return QString::fromStdString(path.string());
}
} // namespace

StitchingExperimentPlayerSelection ScanExperimentFrames(
    const Request& request,
    const StitchingExperimentWorkspace& baseline,
    const StitchingExperimentWorkspace& candidate,
    const ExperimentProcessRunner& run) {
  namespace fs = std::filesystem;
  const double requested = Number(request.payload, "search_duration_seconds", 1, 300, -1);
  if (requested < 1 || requested != std::floor(requested))
    throw std::runtime_error("Player search requires an integer duration from 1 to 300 seconds");
  const int duration = static_cast<int>(requested);
  const auto defaults = hm::baseline_config::load();
  if (!defaults.ok())
    throw std::runtime_error(defaults.status().ToString());
  const auto controls = ReadStitchingExperimentDefaults(defaults->values);
  check(CompleteStitchingExperimentWorkspace(baseline, true));
  const auto runtime_path = [&](const char* key) {
    const fs::path path(RequiredString(request.runtime, key, 4096).toStdString());
    if (!path.is_absolute() || !fs::exists(path))
      throw std::runtime_error("Native experiment runtime path is unavailable");
    return path_string(fs::canonical(path));
  };
  const auto exporter = runtime_path("exporter"), cli = runtime_path("cli"), config = runtime_path("config");
  const auto working = runtime_path("root");
  const fs::path work = request.root / "work";
  fs::create_directories(work / "scan-output");
  fs::create_directories(work / "scan-runtime");
  const fs::path report = work / "player-selection.json";
  {
    const auto lock = hm::stitching::GameConfigTransactionLock::Acquire(baseline.game_directory);
    if (!lock.ok())
      throw std::runtime_error(lock.status().ToString());
    auto document = YAML::LoadFile((baseline.game_directory / "config.yaml").string());
    YAML::Node arguments(YAML::NodeType::Sequence);
    for (const auto& argument : std::vector<std::string>{
             "--enable-sinks=FAKE",
             "--stitching-player-scan-output=" + report.string(),
             "--stitching-player-scan-interval-ms=" + std::to_string(controls.scan_interval_ms),
             "--stitching-player-scan-frame-count=" + std::to_string(candidate.settings.frame_count),
             "--start-time=" + candidate.settings.stitch_frame_time,
             "--time-limit=" + std::to_string(duration)})
      arguments.push_back(argument);
    document["hstream_ui"]["job"]["arguments"] = arguments;
    check(hm::stitching::publish_game_config(baseline.game_directory, YAML::Dump(document)));
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  for (const char* key :
       {"HSTREAM_CALIBRATION_PENDING", "HSTREAM_CALIBRATION_START_STAGE", "HSTREAM_CALIBRATION_INVALIDATION_ID"})
    environment.remove(key);
  environment.insert("HM_GAME_DIR", path_string(baseline.root));
  environment.insert("HM_OUTPUT_WORK_DIR", path_string(work / "scan-output"));
  environment.insert("HSTREAM_RUNTIME_CACHE_DIR", path_string(work / "scan-runtime"));
  environment.insert("HM_MAX_CONTROL_POINTS", QString::number(baseline.settings.control_points));
  environment.insert("HM_STITCH_CALIBRATION_FRAME_COUNT", QString::number(baseline.settings.frame_count));
  environment.insert("HSTREAM_RENDER_AUDIO_MUTED", "1");
  environment.insert("USE_NEW_NVSTREAMMUX", "yes");
  const auto script = path_string(work / "scan.sh");
  run(exporter,
      {"--interactive",
       "--game-dir",
       path_string(baseline.game_directory),
       "--output",
       script,
       "--runner",
       cli,
       "--config",
       config,
       "--working-directory",
       working},
      working,
      environment);
  run("/bin/bash", {script}, working, environment);
  const auto result = PreparePlayerSelectedStitchingExperiment(baseline, candidate, report, duration);
  if (!result.ok())
    throw std::runtime_error(result.status().ToString());
  return *result;
}
} // namespace hm::web
