#include "hstream/src/libs/common/BaselineConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const std::string& message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

class EnvironmentRestore {
 public:
  explicit EnvironmentRestore(const char* name) : name_(name) {
    if (const char* value = std::getenv(name); value) {
      present_ = true;
      value_ = value;
    }
  }
  ~EnvironmentRestore() {
    if (present_)
      ::setenv(name_.c_str(), value_.c_str(), 1);
    else
      ::unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  std::string value_;
  bool present_{false};
};

} // namespace

int main() {
  bool ok = true;
  EnvironmentRestore restore_config_root("HM_CONFIG_ROOT");
  ::unsetenv("HM_CONFIG_ROOT");

  const auto bundled = hm::baseline_config::load();
  ok &= expect(bundled.ok(), bundled.ok() ? "Bundled baseline should load" : bundled.status().ToString());
  if (bundled.ok()) {
    const YAML::Node camera = bundled->values["rink"]["camera"];
    ok &= expect(
        bundled->path.filename() == hm::baseline_config::kBaselineFilename && bundled->values["camera"].IsMap() &&
            bundled->values["plot"].IsMap() && bundled->values["aspen"].IsMap() &&
            bundled->values["stitching"].IsMap() && bundled->values["model"].IsMap() &&
            camera["stop_on_dir_change_delay"].as<int>() == 10 &&
            camera["breakaway_detection"]["overshoot_stop_delay_count"].as<int>() == 6,
        "Bundled baseline should preserve the complete top-level config and camera defaults");
  }

  EnvironmentRestore restore_test_srcdir("TEST_SRCDIR");
  EnvironmentRestore restore_test_workspace("TEST_WORKSPACE");
  EnvironmentRestore restore_workspace_directory("BUILD_WORKSPACE_DIRECTORY");
  if (bundled.ok()) {
    const fs::path original_cwd = fs::current_path();
    ::unsetenv("TEST_SRCDIR");
    ::unsetenv("TEST_WORKSPACE");
    ::unsetenv("BUILD_WORKSPACE_DIRECTORY");
    fs::current_path(bundled->root.parent_path());
    const auto source_checkout = hm::baseline_config::resolve_root();
    fs::current_path(original_cwd);
    std::error_code equivalent_error;
    ok &= expect(
        source_checkout.ok() &&
            fs::equivalent(
                *source_checkout / hm::baseline_config::kBaselineFilename, bundled->path, equivalent_error) &&
            !equivalent_error,
        "Source-checkout discovery should find configs/baseline.yaml without Bazel runfiles variables");
  }

  const fs::path root = fs::temp_directory_path() / ("baseline-config-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "valid");
  std::ofstream(root / "valid" / "baseline.yaml") << "marker: explicit\n";
  ::setenv("HM_CONFIG_ROOT", (root / "valid").c_str(), 1);
  const auto explicit_config = hm::baseline_config::load();
  ok &= expect(
      explicit_config.ok() && explicit_config->root == root / "valid" &&
          explicit_config->values["marker"].as<std::string>() == "explicit",
      "HM_CONFIG_ROOT should take precedence over every bundled search path");

  ::setenv("HM_CONFIG_ROOT", (root / "missing").c_str(), 1);
  const auto missing = hm::baseline_config::load();
  ok &= expect(
      !missing.ok() && missing.status().message().find("HM_CONFIG_ROOT") != std::string::npos,
      "An invalid explicit config root should fail instead of falling back silently");

  fs::create_directories(root / "malformed");
  std::ofstream(root / "malformed" / "baseline.yaml") << "rink: [unterminated\n";
  ::setenv("HM_CONFIG_ROOT", (root / "malformed").c_str(), 1);
  const auto malformed = hm::baseline_config::load();
  ok &= expect(
      !malformed.ok() && malformed.status().message().find("Failed to load baseline config") != std::string::npos,
      "Malformed baseline YAML should report its source and fail");

  fs::create_directories(root / "scalar");
  std::ofstream(root / "scalar" / "baseline.yaml") << "42\n";
  const auto scalar = hm::baseline_config::load_from_root(root / "scalar");
  ok &= expect(!scalar.ok(), "The baseline document should be required to contain a YAML map");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
