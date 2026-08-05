#include "TensorRtModelCache.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "hstream/src/libs/assets/AssetManager.h"

namespace hm::pipeline {
namespace {

namespace fs = std::filesystem;

bool enabled(const YAML::Node& section) {
  const YAML::Node value = section["enable"];
  if (!value || !value.IsScalar())
    return true;
  try {
    return value.as<int>() != 0;
  } catch (...) {
    try {
      return value.as<bool>();
    } catch (...) {
      return true;
    }
  }
}

fs::path resolve_path(const std::string& raw, const fs::path& base) {
  fs::path path(raw);
  if (!path.is_absolute())
    path = base / path;
  return path.lexically_normal();
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool is_path_property(const std::string& raw_key) {
  const std::string key = lowercase(raw_key);
  return key.find("file") != std::string::npos || key.find("path") != std::string::npos;
}

uint64_t stable_path_hash(const fs::path& path) {
  constexpr uint64_t kOffset = 1469598103934665603ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffset;
  for (const unsigned char character : path.string()) {
    hash ^= character;
    hash *= kPrime;
  }
  return hash;
}

fs::path cache_root() {
  if (const char* configured = std::getenv("HMSTREAM_TENSORRT_CACHE_DIR"); configured && *configured)
    return fs::absolute(configured).lexically_normal();
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    return (fs::path(xdg) / "hmstream/tensorrt").lexically_normal();
  if (const char* home = std::getenv("HOME"); home && *home)
    return (fs::path(home) / ".cache/hmstream/tensorrt").lexically_normal();
  return fs::temp_directory_path() / ("hmstream-" + std::to_string(::getuid())) / "tensorrt";
}

absl::Status ensure_private_directory(const fs::path& directory) {
  std::error_code error;
  fs::create_directories(directory, error);
  if (error)
    return absl::InternalError("Unable to create TensorRT cache directory: " + error.message());
  if (!fs::is_directory(directory, error) || error)
    return absl::FailedPreconditionError("TensorRT cache path is not a directory: " + directory.string());
  fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
  if (error)
    return absl::InternalError("Unable to protect TensorRT cache directory: " + error.message());
  return absl::OkStatus();
}

absl::Status publish_model_file(const fs::path& source, const fs::path& target, const std::string& expected_hash) {
  std::error_code error;
  const fs::file_status target_status = fs::symlink_status(target, error);
  if (!error && fs::is_regular_file(target_status) && !fs::is_symlink(target_status)) {
    error.clear();
    if (fs::equivalent(source, target, error) && !error)
      return absl::OkStatus();
    error.clear();
    auto target_hash = hm::assets::AssetManager::Sha256(target);
    if (target_hash.ok() && *target_hash == expected_hash)
      return absl::OkStatus();
  }

  const fs::path temporary =
      target.parent_path() / ("." + target.filename().string() + "." + std::to_string(::getpid()) + ".tmp");
  fs::remove(temporary, error);
  error.clear();
  fs::create_hard_link(source, temporary, error);
  if (error) {
    error.clear();
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing, error);
  }
  if (error) {
    const std::string message = error.message();
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    return absl::InternalError("Unable to copy packaged ONNX model into TensorRT cache: " + message);
  }
  auto temporary_hash = hm::assets::AssetManager::Sha256(temporary);
  if (!temporary_hash.ok() || *temporary_hash != expected_hash) {
    fs::remove(temporary);
    return temporary_hash.ok() ? absl::DataLossError("Cached ONNX model hash mismatch") : temporary_hash.status();
  }
  const int model_fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
  if (model_fd < 0 || ::fsync(model_fd) != 0) {
    const std::string message = std::strerror(errno);
    if (model_fd >= 0)
      ::close(model_fd);
    fs::remove(temporary);
    return absl::InternalError("Unable to sync cached ONNX model: " + message);
  }
  if (::close(model_fd) != 0) {
    fs::remove(temporary);
    return absl::InternalError("Unable to close cached ONNX model: " + std::string(std::strerror(errno)));
  }
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(temporary);
    return absl::InternalError("Unable to publish cached ONNX model: " + error.message());
  }
  return absl::OkStatus();
}

absl::Status publish_yaml(const fs::path& target, const YAML::Node& config) {
  std::ostringstream serialized;
  serialized << config << '\n';
  const std::string contents = serialized.str();
  const fs::path temporary =
      target.parent_path() / ("." + target.filename().string() + "." + std::to_string(::getpid()) + ".tmp");
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    return absl::InternalError("Unable to create TensorRT runtime config: " + std::string(std::strerror(errno)));
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      const std::string message = std::strerror(errno);
      ::close(fd);
      fs::remove(temporary);
      return absl::InternalError("Unable to write TensorRT runtime config: " + message);
    }
    offset += static_cast<size_t>(written);
  }
  if (::fsync(fd) != 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    fs::remove(temporary);
    return absl::InternalError("Unable to sync TensorRT runtime config: " + message);
  }
  if (::close(fd) != 0) {
    fs::remove(temporary);
    return absl::InternalError("Unable to close TensorRT runtime config: " + std::string(std::strerror(errno)));
  }
  std::error_code error;
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(temporary);
    return absl::InternalError("Unable to publish TensorRT runtime config: " + error.message());
  }
  const int directory_fd = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0)
    return absl::InternalError("Unable to open TensorRT cache directory for sync");
  const int sync_status = ::fsync(directory_fd);
  const int sync_error = errno;
  ::close(directory_fd);
  if (sync_status != 0)
    return absl::InternalError("Unable to sync TensorRT cache directory: " + std::string(std::strerror(sync_error)));
  return absl::OkStatus();
}

absl::Status prepare_inference_config(YAML::Node section, const fs::path& config_directory) {
  if (!enabled(section) || !section["config-file"] || !section["config-file"].IsScalar())
    return absl::OkStatus();
  const fs::path inference_path = resolve_path(section["config-file"].as<std::string>(), config_directory);
  YAML::Node inference;
  try {
    inference = YAML::LoadFile(inference_path.string());
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to read inference config " + inference_path.string() + ": " + exception.what());
  }
  YAML::Node properties = inference["property"];
  if (!properties || !properties.IsMap() || !properties["onnx-file"] || !properties["model-engine-file"] ||
      !properties["onnx-file"].IsScalar() || !properties["model-engine-file"].IsScalar()) {
    return absl::OkStatus();
  }

  const fs::path onnx_path = resolve_path(properties["onnx-file"].as<std::string>(), inference_path.parent_path());
  std::error_code error;
  if (!fs::is_regular_file(onnx_path, error) || error)
    return absl::NotFoundError("Configured ONNX model does not exist: " + onnx_path.string());
  if (::access(onnx_path.parent_path().c_str(), W_OK) == 0)
    return absl::OkStatus();

  auto model_hash = hm::assets::AssetManager::Sha256(onnx_path);
  if (!model_hash.ok())
    return model_hash.status();
  std::ostringstream config_hash;
  config_hash << std::hex << std::setw(16) << std::setfill('0') << stable_path_hash(fs::absolute(inference_path));
  const fs::path model_directory = cache_root() / (model_hash->substr(0, 16) + "-" + config_hash.str());
  auto directory_status = ensure_private_directory(model_directory);
  if (!directory_status.ok())
    return directory_status;

  const fs::path cached_onnx = model_directory / onnx_path.filename();
  auto model_status = publish_model_file(onnx_path, cached_onnx, *model_hash);
  if (!model_status.ok())
    return model_status;

  for (auto property : properties) {
    const std::string key = property.first.as<std::string>();
    if (key == "model-engine-file" || !property.second.IsScalar() || !is_path_property(key))
      continue;
    const std::string value = property.second.as<std::string>();
    if (!value.empty())
      properties[key] = resolve_path(value, inference_path.parent_path()).string();
  }
  properties["onnx-file"] = cached_onnx.string();
  const fs::path configured_engine(properties["model-engine-file"].as<std::string>());
  if (configured_engine.filename().empty())
    return absl::InvalidArgumentError("TensorRT model-engine-file must name an engine file");
  const fs::path cached_engine = model_directory / configured_engine.filename();
  properties["model-engine-file"] = cached_engine.string();

  const fs::path runtime_config = model_directory / (inference_path.stem().string() + ".runtime.yaml");
  auto publish_status = publish_yaml(runtime_config, inference);
  if (!publish_status.ok())
    return publish_status;
  section["config-file"] = runtime_config.string();
  std::cout << "TensorRT writable model cache: " << cached_engine << '\n';
  return absl::OkStatus();
}

} // namespace

absl::Status PrepareTensorRtModelCache(YAML::Node pipeline, const fs::path& config_directory) {
  if (!pipeline || !pipeline.IsMap())
    return absl::OkStatus();
  for (auto entry : pipeline) {
    if (!entry.first.IsScalar() || !entry.second.IsMap())
      continue;
    const std::string name = entry.first.as<std::string>();
    if (name.rfind("primary-gie", 0) != 0 && name.rfind("secondary-gie", 0) != 0)
      continue;
    auto status = prepare_inference_config(entry.second, config_directory);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

} // namespace hm::pipeline
