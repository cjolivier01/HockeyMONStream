#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_config(const std::filesystem::path& path, const std::string& generation) {
  YAML::Node config(YAML::NodeType::Map);
  config["unrelated"] = "preserved";
  config["rink"]["stitched_output_generation"] = generation;
  std::ofstream(path) << config << '\n';
}

} // namespace

int main() {
  namespace fs = std::filesystem;
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("live-stitching-generation-test-" + std::to_string(::getpid()));
  fs::create_directories(root);

  const std::string hugin_generation = "exact\npost-stitch-rotate-degrees:payload\n";
  const std::string generation = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin_generation.size()) +
      "\n" + hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
  const std::string rotated_generation =
      "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin_generation.size()) + "\n" + hugin_generation +
      "post-stitch-rotate-degrees:9.25\noutput-size:320x180\n";
  write_config(root / "config.yaml", generation);

  const auto authorize = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25);
  YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      authorize.ok() && config["unrelated"].as<std::string>() == "preserved" &&
          config["rink"]["stitched_output_generation"].as<std::string>() == generation &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == rotated_generation,
      "authorization must preserve Hugin and output-size bytes while replacing only the rotation");

  const auto restore = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 0.0);
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      restore.ok() && !config["rink"]["stitched_output_pending_generation"].IsDefined(),
      "authorizing the completed generation must remove a superseded pending token");

  const std::string legacy_generation =
      "hstream-stitched-output-v1\nhugin-bytes:1\nh"
      "post-stitch-rotate-degrees:0\n";
  write_config(root / "config.yaml", legacy_generation);
  const auto legacy = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 1.0);
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      absl::IsFailedPrecondition(legacy) &&
          config["rink"]["stitched_output_generation"].as<std::string>() == legacy_generation &&
          !config["rink"]["stitched_output_pending_generation"].IsDefined(),
      "dimensionless generations must not create an inexact live authorization");

  ok &= expect(
      absl::IsInvalidArgument(hm::stitching::authorize_live_stitched_output_rotation(root.string(), std::nan(""))),
      "non-finite live rotations must be rejected");

  std::error_code ignored;
  fs::remove_all(root, ignored);
  return ok ? 0 : 1;
}
