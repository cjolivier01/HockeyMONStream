#include "hstream/src/libs/player_analytics/PreparedModelValidation.h"

#include <unistd.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace hm::player_analytics {
namespace {
namespace fs = std::filesystem;

bool readable_file(const fs::path& path) {
  return fs::is_regular_file(path) && fs::file_size(path) > 0 && ::access(path.c_str(), R_OK) == 0;
}
YAML::Node document(const fs::path& path) {
  constexpr std::size_t maximum = 1024 * 1024;
  if (!readable_file(path) || fs::file_size(path) > maximum)
    throw std::invalid_argument("Cannot read bounded model/config metadata: " + path.string());
  std::ifstream stream(path, std::ios::binary);
  std::vector<char> data(maximum + 1);
  stream.read(data.data(), data.size());
  const auto size = stream.gcount();
  if (size <= 0 || std::size_t(size) > maximum || stream.bad())
    throw std::invalid_argument("Cannot read bounded model/config metadata: " + path.string());
  return YAML::Load(std::string(data.data(), size));
}
} // namespace

absl::StatusOr<ModelManifest> ValidatePreparedModelBundle(
    const fs::path& directory,
    ModelFeature feature,
    std::size_t minimum_batch) {
  try {
    const std::string name = feature == ModelFeature::kPose ? "pose"
        : feature == ModelFeature::kJersey                  ? "jersey"
        : feature == ModelFeature::kAction                  ? "action"
                                                            : "team";
    if (!fs::is_directory(directory))
      return absl::InvalidArgumentError("Missing " + name + " custom model folder: " + directory.string());
    auto manifest = ParseModelManifest(document(directory / "manifest.json"));
    if (!manifest.ok())
      return absl::InvalidArgumentError(
          "Invalid " + name + " prepared manifest: " + std::string(manifest.status().message()));
    if (manifest->feature != feature || manifest->maximum_batch < minimum_batch)
      return absl::InvalidArgumentError(
          "The " + name + " custom model has the wrong feature or insufficient batch capacity.");
    for (const auto& file : {manifest->engine_file, manifest->onnx_file})
      if (!readable_file(directory / file))
        return absl::InvalidArgumentError("Missing or unreadable prepared " + name + " file: " + file);
    return manifest;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

absl::StatusOr<fs::path> ValidatePreparedReidConfig(const fs::path& config) {
  try {
    const auto parsed = document(config);
    const auto reid = parsed["ReID"];
    const auto engine = reid && reid.IsMap() ? reid["modelEngineFile"] : YAML::Node();
    if (!engine.IsDefined() || !engine.IsScalar() || engine.as<std::string>().empty())
      return absl::InvalidArgumentError(
          "The ReID configuration requires a readable, nonempty prepared modelEngineFile.");
    const auto path = config.parent_path() / fs::path(engine.as<std::string>());
    if (!readable_file(path))
      return absl::InvalidArgumentError(
          "The ReID configuration requires a readable, nonempty prepared modelEngineFile.");
    return fs::absolute(path).lexically_normal();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
} // namespace hm::player_analytics
