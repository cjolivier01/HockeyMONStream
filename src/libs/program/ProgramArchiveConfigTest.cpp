#include "hstream/src/libs/program/ProgramArchiveConfig.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
} // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 3, "Expected baseline and application YAML");
    const auto baseline = YAML::LoadFile(argv[1]);
    const auto app = YAML::LoadFile(argv[2]);
    const auto empty = YAML::Node(YAML::NodeType::Map);
    const auto directory = std::filesystem::absolute("unused-archive-test-output");
    const std::set<std::string> sinks{"RENDER", "ENCODE_FILE", "ENCODE_PROGRAM_4K_FILE"};
    const auto resolve = [&](const std::vector<std::string>& options = {},
                             const YAML::Node& game = YAML::Node(YAML::NodeType::Map),
                             const std::optional<hm::BitratePerPixel>& source = std::nullopt) {
      const auto config = hm::config::ResolveProgramConfiguration(baseline, empty, game, app, options);
      Check(config.ok(), config.status().ToString());
      const auto output = hm::program::ResolveProgramOutputConfig(*config);
      Check(output.ok(), output.status().ToString());
      return hm::program::ResolveProgramArchives(*config, sinks, *output, directory, source);
    };
    auto result = resolve();
    Check(result.ok(), result.status().ToString());
    Check(
        result->size() == 2 && result->at("ENCODE_FILE").sink_id == 2 &&
            result->at("ENCODE_FILE").path == directory / "tracking_output.mp4" &&
            result->at("ENCODE_PROGRAM_4K_FILE").kind == "program-4k" &&
            result->at("ENCODE_PROGRAM_4K_FILE").path == directory / "program_4k_output.mp4" &&
            result->at("ENCODE_FILE").output.bitrate_kbps == 20000 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.bitrate_kbps == 45000,
        "Native archive defaults or publication identities changed");
    Check(!std::filesystem::exists(directory), "Resolution created an output directory");
    const auto source = hm::make_bitrate_per_pixel(100000000, 5312, 2988);
    result = resolve({"runtime.amd.output.width=7680,runtime.amd.output.height=4320"}, empty, source);
    Check(result.ok(), result.status().ToString());
    const auto& full = result->at("ENCODE_FILE");
    const auto& small = result->at("ENCODE_PROGRAM_4K_FILE");
    Check(
        full.output.width == 7680 && full.output.height == 4320 && small.output.width == 3840 &&
            small.output.height == 2160 &&
            full.output.bitrate_kbps == (*hm::scale_bitrate(*source, 7680, 4320) + 999) / 1000 &&
            small.output.bitrate_kbps == (*hm::scale_bitrate(*source, 3840, 2160) + 999) / 1000,
        "Independent archive sizes or source bitrate density lost");
    result = resolve({"runtime.amd.output.width=4096,runtime.amd.output.height=2048"});
    Check(
        result.ok() && result->at("ENCODE_PROGRAM_4K_FILE").output.width == 3840 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.height == 1920,
        "4K fitting distorted Program aspect");
    result = resolve({"video_out.output_width=1280"});
    Check(
        result.ok() && result->at("ENCODE_PROGRAM_4K_FILE").output.width == 1280 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.height == 720,
        "4K archive upscaled smaller Program");
    result = resolve({"video_out.bit_rate=1234567,pipeline.sink6.bitrate=7654321"}, empty, source);
    Check(
        result.ok() && result->at("ENCODE_FILE").output.bitrate_kbps == 1235 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.bitrate_kbps == 7655,
        "Explicit archive bitrates ignored");
    result = resolve({"video_out.bit_rate=1234567,runtime.amd.output.bitrate_kbps=5000"}, empty, source);
    Check(
        result.ok() && result->at("ENCODE_FILE").output.bitrate_kbps == 5000 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.bitrate_kbps == 5000,
        "AMD bitrate lost same-layer precedence");
    result =
        resolve({"video_out.bit_rate=10000000"}, YAML::Load("runtime: {amd: {output: {bitrate_kbps: 5000}}}"), source);
    Check(
        result.ok() && result->at("ENCODE_FILE").output.bitrate_kbps == 10000 &&
            result->at("ENCODE_PROGRAM_4K_FILE").output.bitrate_kbps == 5000,
        "Higher Program-only bitrate leaked into the independent 4K branch");
    auto game = YAML::Load("pipeline: {sink2: {bitrate: 7000000, output-file: saved.mp4}}");
    result = resolve({"video_out.bit_rate=8000000,video_out.output_video_path=launch.mp4"}, game, source);
    Check(
        result.ok() && result->at("ENCODE_FILE").output.bitrate_kbps == 8000 &&
            result->at("ENCODE_FILE").path == std::filesystem::absolute("launch.mp4"),
        "Higher canonical layer did not override native sink");
    for (const auto* clear : {"video_out.output_video_path=null", "pipeline.sink2.output-file=null"}) {
      result = resolve({clear}, game);
      Check(
          result.ok() && result->at("ENCODE_FILE").path == directory / "tracking_output.mp4",
          "Cleared archive filename retained the lower-layer saved filename");
    }
    result =
        resolve({"video_out.bit_rate=8000000,pipeline.sink2.bitrate=9000000,"
                 "video_out.output_video_path=canonical.mp4,pipeline.sink2.output-file=native.mp4"});
    Check(
        result.ok() && result->at("ENCODE_FILE").output.bitrate_kbps == 9000 &&
            result->at("ENCODE_FILE").path == directory / "native.mp4",
        "Native archive settings lost same-layer precedence");
    for (const auto* invalid :
         {"pipeline.sink6.codec=1",
          "pipeline.sink2.enc-type=1",
          "pipeline.sink2.profile=0",
          "pipeline.sink2.container=2",
          "pipeline.sink6.sink-id=2",
          "pipeline.sink2.bitrate=-1",
          "pipeline.sink2.output-file=output.mkv",
          "video_out.program_4k_interpolation=nearest",
          "pipeline.sink2.width=1280",
          "pipeline.sink6.width=1920",
          "pipeline.sink2.output-file=same.mp4,pipeline.sink6.output-file=same.mp4"})
      Check(!resolve({invalid}).ok(), std::string("Unsupported archive setting accepted: ") + invalid);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
