#include "hstream/src/libs/program/ProgramAudioConfig.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const std::string& text) {
  if (!ok)
    throw std::runtime_error(text);
}
} // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 3, "Expected baseline and native YAML");
    const auto baseline = YAML::LoadFile(argv[1]), native = YAML::LoadFile(argv[2]);
    const auto resolve = [&](const std::vector<std::string>& options,
                             std::array<double, 2> offsets = {0, 12.5},
                             const YAML::Node& game = YAML::Node(YAML::NodeType::Map)) {
      const auto config =
          hm::config::ResolveProgramConfiguration(baseline, YAML::Node(YAML::NodeType::Map), game, native, options);
      Check(config.ok(), config.status().ToString());
      return hm::program::ResolveProgramAudioConfig(*config, offsets);
    };
    const auto standard = resolve({});
    Check(
        standard.ok() && standard->has_value() && (*standard)->camera == 0 && (*standard)->bitrate == 192000,
        standard.status().ToString());
    const auto right = resolve({}, {12.5, 0});
    Check(right.ok() && (*right)->camera == 1, "Zero-offset audio camera was not selected");
    for (const auto* block : {"hmaudio", "hmaudio0"}) {
      const auto disabled = resolve({std::string("pipeline.") + block + ".enable=0"});
      Check(disabled.ok() && !*disabled, "Audio disable was ignored");
    }
    for (const auto* invalid :
         {"pipeline.hmaudio.src=0",
          "pipeline.hmaudio.dest=2",
          "pipeline.hmaudio.sink-id=1",
          "pipeline.hmaudio.source-id=2",
          "pipeline.hmaudio.sink_id=1",
          "runtime.amd.audio.queue_samples=1",
          "runtime.amd.audio.bitrate=0",
          "runtime.amd.audio.maximum_packet_bytes=-1"})
      Check(!resolve({invalid}).ok(), std::string("Invalid audio choice accepted: ") + invalid);
    Check(resolve({}, {1, 2}).ok(), "Independent nonzero source starts rejected");
    const auto selected = resolve({"pipeline.hmaudio.source-id=1"}, {1, 2});
    Check(
        selected.ok() && selected->has_value() && (*selected)->camera == 1,
        "Explicit audio camera with a nonzero source start rejected");
    Check(!resolve({}, {-1, 2}).ok(), "Negative source start accepted");
    auto lower = YAML::Load("pipeline: {hmaudio: {enable: 0}} ");
    Check(
        resolve({"pipeline.hmaudio0.enable=1"}, {0, 1}, lower)->has_value(),
        "Higher numbered block did not override generic disable");
    lower = YAML::Load("pipeline: {hmaudio0: {enable: 1, src: 0}} ");
    Check(
        resolve({"pipeline.hmaudio.src=2"}, {0, 1}, lower).ok(),
        "Higher generic source did not override numbered source");
    const auto aliases = resolve({"pipeline.hmaudio.source_id=1,pipeline.hmaudio.alsa_dest_device=null"}, {2, 0});
    Check(
        aliases.ok() && aliases->has_value() && (*aliases)->camera == 1 && (*aliases)->alsa_device == "null",
        "Native audio aliases were ignored");
    Check(!resolve({"pipeline.hmaudio7.enable=1"}).ok(), "Multiple enabled audio blocks accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
