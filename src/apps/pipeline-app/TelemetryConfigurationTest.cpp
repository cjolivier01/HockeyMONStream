#include "src/apps/pipeline-app/TelemetryConfiguration.h"

#include <unistd.h>
#include <iostream>

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("telemetry-config-test-" + std::to_string(getpid()));
  fs::create_directories(root / "nested");
  const std::string nested = "# exact plugin settings\n[property]\nthreshold=0.4\n";
  std::ofstream(root / "nested" / "detector.txt") << nested;
  std::ofstream(root / "nested" / "settings.yaml") << "config-file: detector.txt\n";
  const auto config =
      YAML::Load("pipeline: {detector: {config-file: nested/settings.yaml}, tracker: {ll-config-file: missing.yaml}}");
  const auto snapshot = SnapshotTelemetryConfigFiles(config, {root});
  bool found = false, missing = false;
  if (snapshot.ok()) {
    for (const auto& entry : *snapshot) {
      if (entry["path"].as<std::string>() == (root / "nested" / "detector.txt").string())
        found = entry["contents"].as<std::string>() == nested;
      if (entry["status"].as<std::string>() == "not-found")
        missing = true;
    }
  }
  fs::remove_all(root);
  if (!snapshot.ok() || !found || !missing) {
    std::cerr << "Referenced configuration snapshot lost raw contents or nested path resolution\n";
    return 1;
  }
  return 0;
}
