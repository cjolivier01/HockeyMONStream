#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "hstream/src/libs/assets/AssetManager.h"

int main(int argc, char** argv) {
  bool print_targets = false;
  bool verify = false;
  bool package_assets = false;
  std::vector<std::filesystem::path> configs;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--print-targets")
      print_targets = true;
    else if (argument == "--verify")
      verify = true;
    else if (argument == "--package-assets")
      package_assets = true;
    else if (argument == "-h" || argument == "--help") {
      std::cout << "usage: hstream-assets [--package-assets] [--print-targets|--verify] CONFIG.yaml...\n";
      return 0;
    } else
      configs.emplace_back(argument);
  }
  if (configs.empty()) {
    std::cerr << "hstream-assets requires at least one YAML config\n";
    return 64;
  }
  if (print_targets && verify) {
    std::cerr << "hstream-assets accepts only one of --print-targets and --verify\n";
    return 64;
  }
  if (package_assets && !print_targets && !verify) {
    std::cerr << "hstream-assets --package-assets requires --print-targets or --verify\n";
    return 64;
  }
  if (print_targets) {
    auto assets = package_assets ? hm::assets::AssetManager::DiscoverPackageAssets(configs)
                                 : hm::assets::AssetManager::Discover(configs);
    if (!assets.ok()) {
      std::cerr << assets.status() << '\n';
      return assets.status().raw_code();
    }
    for (const auto& asset : *assets)
      std::cout << asset.target.string() << '\n';
    return 0;
  }
  if (verify) {
    const auto status = package_assets ? hm::assets::AssetManager::VerifyPackageAssets(configs)
                                       : hm::assets::AssetManager::Verify(configs);
    if (!status.ok()) {
      std::cerr << status << '\n';
      return status.raw_code();
    }
    return 0;
  }
  const auto status = hm::assets::AssetManager::Ensure(configs);
  if (!status.ok()) {
    std::cerr << status << '\n';
    return status.raw_code();
  }
  return 0;
}
