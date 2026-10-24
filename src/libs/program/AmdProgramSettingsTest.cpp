#include "hstream/src/libs/program/AmdProgramSettings.h"
#include "hstream/src/libs/common/BaselineConfig.h"

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
    auto baseline = hm::baseline_config::load_from_root(std::filesystem::path(argv[1]).parent_path());
    Check(baseline.ok(), "Cannot load baseline");
    const auto settings = hm::program::ResolveAmdProgramSettings(baseline->values);
    Check(settings.ok(), "Cannot resolve shipped AMD settings");
    Check(
        settings->gpu_id == 0 && settings->reader_queue_frames == 2 && settings->output_pool_frames == 8 &&
            settings->operation_timeout_ns == 30000000000ULL && settings->cancellation_poll_ns == 10000000 &&
            settings->maximum_inference_io_bytes == 268435456 && settings->maximum_encoded_packet_bytes == 67108864 &&
            settings->compact_stitch_workspace && settings->tracker.maximum_tracks == 512,
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
    // Every scalar must come from the effective document, including every
    // association parameter. Removing any one must fail instead of defaulting.
    const auto amd = baseline->values["runtime"]["amd"];
    for (const auto& entry : amd) {
      auto missing = YAML::Clone(baseline->values);
      missing["runtime"]["amd"].remove(entry.first.as<std::string>());
      Check(!hm::program::ResolveAmdProgramSettings(missing).ok(), "Missing AMD key acquired a hidden default");
    }
    for (const auto& entry : amd["tracker"]) {
      auto missing = YAML::Clone(baseline->values);
      missing["runtime"]["amd"]["tracker"].remove(entry.first.as<std::string>());
      Check(!hm::program::ResolveAmdProgramSettings(missing).ok(), "Missing tracker key acquired a hidden default");
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
