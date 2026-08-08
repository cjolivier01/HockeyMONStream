#include "TensorRtModelCache.h"

#include <algorithm>
#include <cctype>
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
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "hstream/src/libs/assets/AssetManager.h"

namespace hm::pipeline {
namespace {

namespace fs = std::filesystem;

struct HeldEngineLock {
  fs::path path;
  int descriptor{-1};
};

std::vector<HeldEngineLock>& held_engine_locks() {
  static std::vector<HeldEngineLock> locks;
  return locks;
}

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

bool is_yaml(const fs::path& path) {
  const std::string extension = lowercase(path.extension().string());
  return extension == ".yaml" || extension == ".yml";
}

bool is_path_property(const std::string& raw_key) {
  const std::string key = lowercase(raw_key);
  return key.find("file") != std::string::npos || key.find("path") != std::string::npos;
}

absl::Status ensure_owned_temporary_root(const fs::path& directory) {
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
    return absl::InternalError(
        "Unable to create private TensorRT temporary root: " + std::string(std::strerror(errno)));
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0)
    return absl::PermissionDeniedError("TensorRT temporary root is not a no-follow directory: " + directory.string());
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::getuid()) {
    ::close(descriptor);
    return absl::PermissionDeniedError(
        "TensorRT temporary root is not owned by the current user: " + directory.string());
  }
  if (::fchmod(descriptor, 0700) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to protect TensorRT temporary root: " + message);
  }
  if (::close(descriptor) != 0)
    return absl::InternalError("Unable to close TensorRT temporary root");
  return absl::OkStatus();
}

absl::StatusOr<fs::path> cache_root() {
  if (const char* configured = std::getenv("HSTREAM_TENSORRT_CACHE_DIR"); configured && *configured)
    return fs::absolute(configured).lexically_normal();
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    return (fs::path(xdg) / "hstream/tensorrt").lexically_normal();
  if (const char* home = std::getenv("HOME"); home && *home)
    return (fs::path(home) / ".cache/hstream/tensorrt").lexically_normal();
  const fs::path private_root = fs::path("/tmp") / ("hstream-" + std::to_string(::getuid()));
  auto status = ensure_owned_temporary_root(private_root);
  if (!status.ok())
    return status;
  return private_root / "tensorrt";
}

absl::Status ensure_private_directory(const fs::path& directory) {
  std::error_code error;
  fs::create_directories(directory, error);
  if (error)
    return absl::InternalError("Unable to create TensorRT cache directory: " + error.message());
  if (!fs::is_directory(directory, error) || error)
    return absl::FailedPreconditionError("TensorRT cache path is not a directory: " + directory.string());
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0)
    return absl::PermissionDeniedError("TensorRT cache directory may not be a symlink: " + directory.string());
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::getuid()) {
    ::close(descriptor);
    return absl::PermissionDeniedError(
        "TensorRT cache directory is not owned by the current user: " + directory.string());
  }
  if (::fchmod(descriptor, 0700) != 0) {
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to protect TensorRT cache directory: " + message);
  }
  if (::close(descriptor) != 0)
    return absl::InternalError("Unable to close TensorRT cache directory");
  return absl::OkStatus();
}

absl::Status publish_model_file(const fs::path& source, const fs::path& target, const std::string& expected_hash) {
  std::error_code error;
  const fs::file_status source_status = fs::symlink_status(source, error);
  if (error || fs::is_symlink(source_status))
    return absl::PermissionDeniedError("Packaged ONNX model may not be a symlink: " + source.string());
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

absl::StatusOr<std::string> inference_build_digest(
    const YAML::Node& inference,
    const YAML::Node& section,
    const YAML::Node& pipeline,
    const std::string& section_name,
    const fs::path& inference_path) {
  std::ostringstream fingerprint;
  fingerprint << "inference:\n"
              << inference << "\npipeline-section:\n"
              << section << "\napplication:\n"
              << pipeline["application"] << "\nsection-name:\n"
              << section_name << '\n';
  const YAML::Node properties = inference["property"];
  if (properties && properties.IsMap()) {
    for (const auto& property : properties) {
      if (!property.first.IsScalar() || !property.second.IsScalar())
        continue;
      const std::string key = property.first.as<std::string>();
      if (key == "model-engine-file" || !is_path_property(key))
        continue;
      const fs::path input = resolve_path(property.second.as<std::string>(), inference_path.parent_path());
      fingerprint << "input:" << key << '=' << fs::absolute(input).lexically_normal().string() << '\n';
      std::error_code error;
      if (fs::is_regular_file(input, error) && !error) {
        auto hash = hm::assets::AssetManager::Sha256(input);
        if (!hash.ok())
          return hash.status();
        fingerprint << "sha256:" << *hash << '\n';
      }
    }
  }
  return hm::assets::AssetManager::Sha256Bytes(fingerprint.str());
}

unsigned scalar_unsigned(
    const YAML::Node& preferred,
    const YAML::Node& fallback,
    const char* key,
    unsigned default_value) {
  for (const YAML::Node& node : {preferred, fallback}) {
    if (!node || !node[key] || !node[key].IsScalar())
      continue;
    try {
      return node[key].as<unsigned>();
    } catch (...) {
    }
  }
  return default_value;
}

bool scalar_bool(const YAML::Node& node, const char* key, bool default_value) {
  if (!node || !node[key] || !node[key].IsScalar())
    return default_value;
  try {
    return node[key].as<bool>();
  } catch (...) {
    try {
      return node[key].as<int>() != 0;
    } catch (...) {
      return default_value;
    }
  }
}

std::string network_mode_name(unsigned mode) {
  switch (mode) {
    case 0:
      return "fp32";
    case 1:
      return "int8";
    case 2:
      return "fp16";
    case 3:
      return "best";
    default:
      return "UNKNOWN";
  }
}

fs::path derived_engine_path(
    const fs::path& cached_onnx,
    const YAML::Node& properties,
    const YAML::Node& section,
    const YAML::Node& pipeline,
    bool secondary,
    const std::string& mode_override = {}) {
  const YAML::Node application = pipeline["application"];
  unsigned batch_size = scalar_unsigned(section, properties, "batch-size", 1);
  if (scalar_bool(application, "use-nvmultiurisrcbin", false) && section["batch-size"] &&
      section["batch-size"].IsScalar()) {
    const char* batch_key = secondary ? "sgie-batch-size" : "max-batch-size";
    batch_size = scalar_unsigned(application, {}, batch_key, batch_size);
  }
  unsigned gpu_id = scalar_unsigned({}, properties, "gpu-id", 0);
  gpu_id = scalar_unsigned(application, {}, "global-gpu-id", gpu_id);
  gpu_id = scalar_unsigned(section, {}, "gpu-id", gpu_id);
  std::string device = "gpu" + std::to_string(gpu_id);
  if (scalar_bool(properties, "enable-dla", false))
    device = "dla" + std::to_string(scalar_unsigned({}, properties, "use-dla-core", 0));
  return fs::path(
      cached_onnx.string() + "_b" + std::to_string(batch_size) + "_" + device + "_" +
      (mode_override.empty() ? network_mode_name(scalar_unsigned({}, properties, "network-mode", 0)) : mode_override) +
      ".engine");
}

absl::Status acquire_engine_lock(const fs::path& path) {
  for (const auto& lock : held_engine_locks())
    if (lock.path == path)
      return absl::OkStatus();
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return absl::InternalError("Unable to open TensorRT engine lock: " + std::string(std::strerror(errno)));
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::getuid()) {
    ::close(descriptor);
    return absl::PermissionDeniedError("TensorRT engine lock is not a user-owned regular file: " + path.string());
  }
  while (::flock(descriptor, LOCK_EX) != 0) {
    if (errno == EINTR)
      continue;
    const std::string message = std::strerror(errno);
    ::close(descriptor);
    return absl::InternalError("Unable to lock TensorRT engine cache: " + message);
  }
  held_engine_locks().push_back({path, descriptor});
  return absl::OkStatus();
}

void release_engine_locks() {
  for (auto& lock : held_engine_locks()) {
    if (lock.descriptor < 0)
      continue;
    while (::flock(lock.descriptor, LOCK_UN) != 0 && errno == EINTR) {
    }
    ::close(lock.descriptor);
    lock.descriptor = -1;
  }
  held_engine_locks().clear();
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

absl::Status prepare_inference_config(
    YAML::Node section,
    const YAML::Node& pipeline,
    const std::string& section_name,
    const fs::path& config_directory) {
  if (!enabled(section) || !section["config-file"] || !section["config-file"].IsScalar())
    return absl::OkStatus();
  const fs::path inference_path = resolve_path(section["config-file"].as<std::string>(), config_directory);
  if (!is_yaml(inference_path))
    return absl::OkStatus();
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
  const bool section_engine_override = section["model-engine-file"] && section["model-engine-file"].IsScalar();
  const fs::path configured_engine = section_engine_override
      ? resolve_path(section["model-engine-file"].as<std::string>(), config_directory)
      : resolve_path(properties["model-engine-file"].as<std::string>(), inference_path.parent_path());
  if (configured_engine.filename().empty())
    return absl::InvalidArgumentError("TensorRT model-engine-file must name an engine file");
  error.clear();
  if (fs::is_regular_file(configured_engine, error) && !error)
    return absl::OkStatus();
  if (lowercase(configured_engine.filename().string()).find("_bf16.engine") != std::string::npos) {
    return absl::NotFoundError(
        "Configured prebuilt BF16 TensorRT engine is unavailable: " + configured_engine.string());
  }
  if (::access(onnx_path.parent_path().c_str(), W_OK) == 0)
    return absl::OkStatus();

  auto build_digest = inference_build_digest(inference, section, pipeline, section_name, inference_path);
  if (!build_digest.ok())
    return build_digest.status();
  auto model_hash = hm::assets::AssetManager::Sha256(onnx_path);
  if (!model_hash.ok())
    return model_hash.status();
  auto root = cache_root();
  if (!root.ok())
    return root.status();
  auto root_status = ensure_private_directory(*root);
  if (!root_status.ok())
    return root_status;
  const fs::path model_directory = *root / (model_hash->substr(0, 16) + "-" + build_digest->substr(0, 16));
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
  const bool secondary = section_name.rfind("secondary-gie", 0) == 0;
  fs::path cached_engine = derived_engine_path(cached_onnx, properties, section, pipeline, secondary);
  auto lock_status = acquire_engine_lock(*root / "engine-build.lock");
  if (!lock_status.ok())
    return lock_status;
  if (scalar_unsigned({}, properties, "network-mode", 0) == 3) {
    // BEST resolves to a concrete precision during TensorRT build. Reuse the
    // concrete filename DeepStream emitted on a previous run when present.
    for (const char* candidate_mode : {"int8", "fp16", "fp32"}) {
      const fs::path candidate =
          derived_engine_path(cached_onnx, properties, section, pipeline, secondary, candidate_mode);
      std::error_code candidate_error;
      if (fs::is_regular_file(candidate, candidate_error) && !candidate_error) {
        cached_engine = candidate;
        break;
      }
    }
  }
  properties["model-engine-file"] = cached_engine.string();
  if (section_engine_override)
    section["model-engine-file"] = cached_engine.string();

  const fs::path runtime_config = model_directory / (inference_path.stem().string() + ".runtime.yaml");
  auto publish_status = publish_yaml(runtime_config, inference);
  if (!publish_status.ok()) {
    release_engine_locks();
    return publish_status;
  }
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
    auto status = prepare_inference_config(entry.second, pipeline, name, config_directory);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

void ReleaseTensorRtModelCacheLocks() {
  release_engine_locks();
}

} // namespace hm::pipeline
