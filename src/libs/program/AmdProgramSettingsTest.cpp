#include "hstream/src/libs/program/AmdProgramSettings.h"
#include "hstream/src/libs/common/BaselineConfig.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  try {
    // Bazel omits the user's HOME/XDG settings. This resolver creates no files.
    Check(setenv("XDG_CACHE_HOME", "/tmp/hstream-config-test-cache", 1) == 0, "Cannot set test cache base");
    auto baseline = hm::baseline_config::load_from_root(std::filesystem::path(argv[1]).parent_path());
    Check(baseline.ok(), "Cannot load baseline");
    const auto settings = hm::program::ResolveAmdProgramSettings(baseline->values);
    if (!settings.ok())
      throw std::runtime_error(settings.status().ToString());
    Check(
        settings->gpu_id == 0 && settings->reader_queue_frames == 2 && settings->output_pool_frames == 8 &&
            settings->operation_timeout_ns == 30000000000ULL && settings->cancellation_poll_ns == 10000000 &&
            settings->maximum_inference_io_bytes == 268435456 && settings->maximum_encoded_packet_bytes == 67108864 &&
            settings->compact_stitch_workspace && settings->tracker.maximum_tracks == 512 &&
            settings->maximum_runtime_command_bytes == 8192,
        "Shipped resource bounds changed");
    auto override = YAML::Clone(baseline->values);
    override["runtime"]["amd"]["gpu_id"] = 1;
    override["runtime"]["amd"]["reader_queue_frames"] = 3;
    override["runtime"]["amd"]["tracker"]["keep_lost_frames"] = 0;
    auto changed = hm::program::ResolveAmdProgramSettings(override);
    Check(
        changed.ok() && changed->gpu_id == 1 && changed->reader_queue_frames == 3 &&
            changed->tracker.keep_lost_frames == 0,
        "Explicit settings were ignored");
    Check(
        settings->model_cache && settings->model_cache->directory.is_absolute() &&
            settings->model_cache->maximum_entry_bytes == 2147483648ULL,
        "Missing shipped cache policy");
    override["runtime"]["amd"]["model_cache"]["directory"] = "/tmp/explicit-model-cache";
    changed = hm::program::ResolveAmdProgramSettings(override);
    Check(
        changed.ok() && changed->model_cache->directory == "/tmp/explicit-model-cache", "Explicit cache path ignored");
    override["runtime"]["amd"]["model_cache"]["directory"] = "relative";
    Check(!hm::program::ResolveAmdProgramSettings(override).ok(), "Relative cache directory accepted");
    override["runtime"]["amd"]["model_cache"]["enable"] = false;
    changed = hm::program::ResolveAmdProgramSettings(override);
    Check(changed.ok() && !changed->model_cache, "Disabled cache resolves storage");
    // Every resource/association setting must come from the effective document.
    // Playback/output settings have their own resolver and missing-key tests.
    const auto amd = baseline->values["runtime"]["amd"];
    for (const auto& entry : amd) {
      const auto key = entry.first.as<std::string>();
      if (key == "output" || key == "audio" || key == "playback_start_seconds")
        continue;
      auto missing = YAML::Clone(baseline->values);
      missing["runtime"]["amd"].remove(key);
      Check(!hm::program::ResolveAmdProgramSettings(missing).ok(), "Missing AMD key acquired a hidden default");
    }
    for (const auto& entry : amd["tracker"]) {
      auto missing = YAML::Clone(baseline->values);
      missing["runtime"]["amd"]["tracker"].remove(entry.first.as<std::string>());
      Check(!hm::program::ResolveAmdProgramSettings(missing).ok(), "Missing tracker key acquired a hidden default");
    }
    for (const auto& entry : amd["model_cache"]) {
      auto missing = YAML::Clone(baseline->values);
      missing["runtime"]["amd"]["model_cache"].remove(entry.first.as<std::string>());
      Check(!hm::program::ResolveAmdProgramSettings(missing).ok(), "Missing cache key acquired a hidden default");
    }
    for (const auto* key : {"gpu_id", "reader_queue_frames", "maximum_inference_io_bytes", "operation_timeout_ms"}) {
      auto invalid = YAML::Clone(baseline->values);
      invalid["runtime"]["amd"][key] = -1;
      Check(!hm::program::ResolveAmdProgramSettings(invalid).ok(), "Negative integer wrapped into an unsigned setting");
    }
    auto invalid = YAML::Clone(baseline->values);
    invalid["runtime"]["amd"]["operation_timeout_ms"] = std::numeric_limits<int64_t>::max();
    Check(!hm::program::ResolveAmdProgramSettings(invalid).ok(), "Nanosecond overflow accepted");
    invalid = YAML::Clone(baseline->values);
    invalid["runtime"]["amd"]["cancellation_poll_ms"] = 30001;
    Check(!hm::program::ResolveAmdProgramSettings(invalid).ok(), "Poll interval exceeded operation deadline");
    invalid = YAML::Clone(baseline->values);
    invalid["runtime"]["amd"]["tracker"]["high_match_cost"] = 1;
    Check(!hm::program::ResolveAmdProgramSettings(invalid).ok(), "Association validation was bypassed");
    invalid["runtime"]["amd"]["tracker"]["high_match_cost"] = ".nan";
    Check(!hm::program::ResolveAmdProgramSettings(invalid).ok(), "Non-finite association setting accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
