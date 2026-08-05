#include "hstream/src/libs/assets/AssetManager.h"

#include <cstdlib>
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
} // namespace

int main() {
  bool ok = true;
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hmstream-assets-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "configs");
  fs::create_directories(root / "pretrained");
  {
    std::ofstream asset(root / "pretrained" / "model.bin");
    asset << "native asset\n";
  }
  auto hash = hm::assets::AssetManager::Sha256(root / "pretrained" / "model.bin");
  ok &= expect(hash.ok(), "fixture hash must compute");
  {
    std::ofstream child(root / "configs" / "child.yaml");
    child << "pretrained-assets:\n  - name: model\n    url: https://example.invalid/model.bin\n    sha256: "
          << (hash.ok() ? *hash : std::string(64, '0')) << "\n    path: ../pretrained/model.bin\n";
    std::ofstream parent(root / "configs" / "parent.yaml");
    parent << "enabled-child:\n  enable: 1\n  config-file: child.yaml\n"
              "disabled-child:\n  enable: 0\n  config-file: missing.yaml\n";
  }
  auto discovered = hm::assets::AssetManager::Discover({root / "configs" / "parent.yaml"});
  ok &= expect(discovered.ok() && discovered->size() == 1, "enabled child asset must be discovered once");
  if (discovered.ok())
    ok &= expect(
        discovered->front().target == root / "pretrained" / "model.bin", "relative target must resolve from config");
  auto ensured = hm::assets::AssetManager::Ensure({root / "configs" / "parent.yaml"});
  ok &= expect(ensured.ok(), "valid cached asset must work offline");
  fs::remove(root / "pretrained" / "model.bin.lock");
  fs::permissions(root / "pretrained", fs::perms::owner_read | fs::perms::owner_exec);
  auto read_only_ensured = hm::assets::AssetManager::Ensure({root / "configs" / "parent.yaml"});
  ok &= expect(read_only_ensured.ok(), "a valid packaged asset must not require a writable sibling lock file");
  ok &= expect(
      !fs::exists(root / "pretrained" / "model.bin.lock"),
      "read-only packaged asset verification must not create a lock file");
  fs::permissions(root / "pretrained", fs::perms::owner_all);
  ok &= expect(
      hm::assets::AssetManager::Verify({root / "configs" / "parent.yaml"}).ok(),
      "verification must accept a present checksummed asset without opening a lock file");
  ok &= expect(
      hm::assets::internal::fsync_asset_parent_directory(root / "pretrained" / "model.bin").ok(),
      "asset parent directory fsync must succeed for a valid publication target");
  ::setenv("HM_TEST_ASSET_DIRECTORY_FSYNC_FAILURE", "1", 1);
  ok &= expect(
      !hm::assets::internal::fsync_asset_parent_directory(root / "pretrained" / "model.bin").ok(),
      "asset publication must propagate a parent-directory fsync failure");
  ::unsetenv("HM_TEST_ASSET_DIRECTORY_FSYNC_FAILURE");
  {
    std::ofstream asset(root / "pretrained" / "model.bin", std::ios::app);
    asset << "tampered\n";
  }
  ok &= expect(
      !hm::assets::AssetManager::Verify({root / "configs" / "parent.yaml"}).ok(),
      "verification must reject a stale or tampered cached asset");
  {
    std::ofstream asset(root / "pretrained" / "model.bin");
    asset << "native asset\n";
  }
  ok &= expect(
      !hm::assets::AssetManager::Discover({root / "configs" / "missing.yaml"}).ok(),
      "a missing requested config must fail discovery");
  {
    std::ofstream parent(root / "configs" / "missing-child.yaml");
    parent << "enabled-child:\n  enable: 1\n  config-file: absent.yaml\n";
  }
  ok &= expect(
      !hm::assets::AssetManager::Discover({root / "configs" / "missing-child.yaml"}).ok(),
      "a missing enabled child config must fail discovery");
  {
    std::ofstream generated(root / "configs" / "generated.yaml");
    generated << "pretrained-assets:\n"
                 "  - name: generated model\n"
                 "    url: https://example.invalid/model.onnx\n"
                 "    sha256: "
              << std::string(64, '0')
              << "\n"
                 "    path: ../pretrained/generated.onnx\n"
                 "    onnx-dynamic-batch: true\n";
  }
  ok &= expect(
      !hm::assets::AssetManager::Discover({root / "configs" / "generated.yaml"}).ok(),
      "runtime model mutation must remain forbidden");
  {
    const char* original_home_value = std::getenv("HOME");
    const std::string original_home = original_home_value == nullptr ? "" : original_home_value;
    fs::create_directories(root / "real-home" / ".cache" / "hmstream");
    fs::create_directory_symlink(root / "real-home", root / "home-link");
    {
      std::ofstream asset(root / "real-home" / ".cache" / "hmstream" / "linked-home-model.bin");
      asset << "linked home asset\n";
    }
    auto linked_hash =
        hm::assets::AssetManager::Sha256(root / "real-home" / ".cache" / "hmstream" / "linked-home-model.bin");
    {
      std::ofstream config(root / "configs" / "linked-home.yaml");
      config << "pretrained-assets:\n"
                "  - name: linked home model\n"
                "    url: https://example.invalid/model.bin\n"
                "    sha256: "
             << (linked_hash.ok() ? *linked_hash : std::string(64, '0'))
             << "\n"
                "    path: $HOME/.cache/hmstream/linked-home-model.bin\n";
    }
    ::setenv("HOME", (root / "home-link").c_str(), 1);
    auto linked_home = hm::assets::AssetManager::Ensure({root / "configs" / "linked-home.yaml"});
    ok &= expect(linked_home.ok(), "a symlinked home directory must retain its canonical approved asset root");
    if (original_home_value == nullptr)
      ::unsetenv("HOME");
    else
      ::setenv("HOME", original_home.c_str(), 1);
  }
  fs::remove_all(root);
  return ok ? 0 : 1;
}
