#include "PipelineAssetOptions.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include "hstream/src/libs/assets/AssetManager.h"

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  bool ok = true;
  const fs::path root =
      fs::weakly_canonical(fs::temp_directory_path()) / ("hstream-pipeline-assets-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "configs");
  fs::create_directories(root / "pretrained");
  {
    std::ofstream asset(root / "pretrained" / "model.bin");
    asset << "asset\n";
  }
  const auto hash = hm::assets::AssetManager::Sha256(root / "pretrained" / "model.bin");
  ok &= expect(hash.ok(), "fixture hash must compute");

  {
    std::ofstream child(root / "configs" / "default-detector.yaml");
    child << "pretrained-assets:\n"
             "  - name: default detector\n"
             "    url: https://example.invalid/default.bin\n"
             "    sha256: "
          << (hash.ok() ? *hash : std::string(64, '0'))
          << "\n"
             "    path: ../pretrained/model.bin\n";
  }
  {
    std::ofstream child(root / "configs" / "override-detector.yaml");
    child << "pretrained-assets:\n"
             "  - name: override detector\n"
             "    url: https://example.invalid/override.bin\n"
             "    sha256: "
          << (hash.ok() ? *hash : std::string(64, '0'))
          << "\n"
             "    path: ../pretrained/model.bin\n";
  }
  {
    std::ofstream parent(root / "configs" / "app.yaml");
    parent << "pretrained-assets:\n"
              "  - name: calibration model\n"
              "    url: https://example.invalid/calibration.bin\n"
              "    sha256: "
           << (hash.ok() ? *hash : std::string(64, '0'))
           << "\n"
              "    path: ../pretrained/model.bin\n"
              "primary-gie:\n"
              "  enable: 1\n"
              "  config-file: default-detector.yaml\n";
  }

  const std::vector<std::map<std::string, std::string>> options = {
      {{"pipeline.primary-gie.config-file", "override-detector.yaml"}}};
  const auto discovered = hm::assets::AssetManager::Discover(
      {root / "configs" / "app.yaml"},
      [&options](YAML::Node config) {
        hm::pipeline_internal::apply_pipeline_options_for_asset_discovery(config, options);
      });
  std::set<std::string> names;
  if (discovered.ok()) {
    for (const auto& asset : *discovered)
      names.insert(asset.name);
  }
  ok &= expect(
      discovered.ok() && names.count("calibration model") == 1 && names.count("override detector") == 1 &&
          names.count("default detector") == 0,
      "asset discovery must follow runtime primary-gie config-file overrides while retaining direct app assets");

  YAML::Node inference = YAML::Load("property:\n  onnx-file: model.onnx\n");
  hm::pipeline_internal::apply_pipeline_options_for_asset_discovery(inference, options);
  ok &= expect(
      !inference["pipeline"].IsDefined(),
      "asset discovery options must not create synthetic pipeline children inside inference configs");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
