#include "hstream/src/libs/program/ProgramLaunchConfig.h"

#include <cmath>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/tracking/ProgramCameraMapping.h"

namespace hm::program {
namespace {
int Rank(const hm::config::LeafRanks& ranks, const std::string& key) {
  const auto found = ranks.find(key);
  return found == ranks.end() ? 0 : found->second;
}
absl::StatusOr<std::filesystem::path> Reference(
    const hm::config::ProgramConfiguration& config,
    const hm::config::ReferenceRoots& roots,
    const std::string& key) {
  const auto value = get_node(config.values, key);
  if (!value || !value->IsScalar())
    return absl::InvalidArgumentError("Missing configuration reference: " + key);
  return hm::config::ResolveReference(value->as<std::string>(), Rank(config.ranks, key), roots, key);
}
} // namespace
absl::StatusOr<ProgramLaunchConfig> LoadProgramLaunchConfig(
    const std::string& game_id,
    const std::filesystem::path& application_file,
    const std::vector<std::string>& option_groups) {
  namespace fs = std::filesystem;
  if (game_id.empty() || application_file.empty())
    return absl::InvalidArgumentError("Program requires a game and an explicit application YAML");
  auto baseline = baseline_config::load();
  if (!baseline.ok())
    return baseline.status();
  auto user = user_config::load_or_create();
  if (!user.ok())
    return user.status();
  try {
    const auto lower = hm::config::MergeNodes(YAML::Clone(baseline->values), YAML::Clone(*user), false);
    auto root = user_config::game_root(lower);
    if (!root.ok())
      return root.status();
    ProgramLaunchConfig result;
    result.game = fs::absolute(*root / game_id);
    auto game = stitching::load_game_config_file(result.game / "config.yaml");
    if (!game.ok())
      return game.status();
    if (!*game)
      return absl::FailedPreconditionError("Program requires saved game configuration and calibration");
    result.saved_game = YAML::Clone(**game);
    result.application_file = fs::absolute(application_file);
    const auto native = YAML::LoadFile(result.application_file.string());
    auto merged = hm::config::ResolveProgramConfiguration(baseline->values, *user, **game, native, option_groups);
    if (!merged.ok())
      return merged.status();
    result.configuration = std::move(*merged);
    const auto blend = stitching::ResolveBlendSettings(result.configuration.layers);
    if (!blend.ok())
      return blend.status();
    result.blend = *blend;
    const auto minimize = get_node(result.configuration.values, "stitching.minimize_blend");
    const auto native_minimize = get_node(result.configuration.values, "pipeline.hmstitcher.minimize-blend");
    if (!minimize)
      return absl::InvalidArgumentError("Effective baseline requires stitching.minimize_blend");
    const bool use_native = native_minimize && !native_minimize->IsNull() &&
        Rank(result.configuration.ranks, "pipeline.hmstitcher.minimize-blend") >=
            Rank(result.configuration.ranks, "stitching.minimize_blend");
    if (use_native) {
      const auto text = native_minimize->as<std::string>();
      result.minimize_blend = text == "1" ? true : text == "0" ? false : native_minimize->as<bool>();
    } else {
      result.minimize_blend = minimize->as<bool>();
    }
    auto settings = ResolveAmdProgramSettings(result.configuration.values);
    if (!settings.ok())
      return settings.status();
    result.amd = *settings;
    fs::path user_directory;
    const auto user_file = user_config::file_path();
    if (user_file.ok())
      user_directory = user_file->parent_path();
    const hm::config::ReferenceRoots roots{
        result.application_file.parent_path(), baseline->root, result.game, user_directory, fs::current_path()};
    const auto camera_file = Reference(result.configuration, roots, "pipeline.ds-playtracker.config-file");
    if (!camera_file.ok())
      return camera_file.status();
    auto camera = tracking::ResolveProgramCameraConfig(
        result.configuration.values,
        result.configuration.ranks,
        Rank(result.configuration.ranks, "pipeline.ds-playtracker.config-file"),
        YAML::LoadFile(camera_file->string()));
    if (!camera.ok())
      return camera.status();
    result.camera = (*camera)["play-tracker"];
    const auto inference_file = Reference(result.configuration, roots, "pipeline.primary-gie.config-file");
    if (!inference_file.ok())
      return inference_file.status();
    result.inference_file = fs::canonical(*inference_file);
    result.inference = YAML::LoadFile(result.inference_file.string());
    if (!result.inference.IsMap() || !result.inference["property"].IsMap())
      return absl::InvalidArgumentError("Program requires an inference YAML property map");
    for (size_t index = 0; index < 2; ++index) {
      const std::string name = index ? "right" : "left";
      const auto sources = get_node(result.configuration.values, "game.videos." + name);
      if (!sources || !sources->IsSequence() || !sources->size())
        return absl::FailedPreconditionError("Program requires saved camera chapters: " + name);
      for (const auto& value : *sources) {
        if (!value.IsScalar() || value.as<std::string>().empty())
          return absl::InvalidArgumentError("Camera chapter must be a nonempty local path");
        const fs::path path(value.as<std::string>());
        result.chapters[index].push_back(path.is_absolute() ? path : result.game / path);
      }
      auto offset = get_node(result.configuration.values, "game.stitching.frame_offsets." + name);
      if (!offset || offset->IsNull())
        offset = get_node(result.configuration.values, "stitching.frame_offsets." + name);
      if (!offset || !offset->IsScalar())
        return absl::FailedPreconditionError("Program requires saved camera synchronization: " + name);
      result.frame_offsets[index] = offset->as<double>();
      if (!std::isfinite(result.frame_offsets[index]) || result.frame_offsets[index] < 0)
        return absl::InvalidArgumentError("Camera synchronization must be a finite nonnegative frame offset");
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Program startup configuration: ") + error.what());
  }
}
} // namespace hm::program
