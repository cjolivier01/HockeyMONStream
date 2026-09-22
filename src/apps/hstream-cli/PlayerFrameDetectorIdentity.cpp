#include "PlayerFrameDetectorIdentity.h"

#include <dlfcn.h>
#include <link.h>
#include <openssl/evp.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "OnnxExternalData.h"

namespace hm::pipeline {
namespace {
namespace fs = std::filesystem;

absl::Status error(const std::string& message) {
  return absl::FailedPreconditionError("Player detector identity: " + message);
}

std::string hex(const unsigned char* bytes, unsigned int count) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  for (unsigned int i = 0; i < count; ++i)
    output << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  return output.str();
}

absl::StatusOr<std::string> digest_string(const std::string& bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
    return error("cannot hash identity");
  return hex(digest.data(), length);
}

absl::StatusOr<std::string> digest_file(const fs::path& path) {
  std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), &fclose);
  struct stat before{}, after{};
  if (!file || fstat(fileno(file.get()), &before) || !S_ISREG(before.st_mode) || before.st_size <= 0)
    return error("missing or invalid input " + path.string());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
    return error("cannot initialize hash");
  std::array<unsigned char, 65536> buffer;
  while (const size_t count = fread(buffer.data(), 1, buffer.size(), file.get())) {
    if (EVP_DigestUpdate(hash.get(), buffer.data(), count) != 1)
      return error("cannot hash input " + path.string());
  }
  if (ferror(file.get()) || fstat(fileno(file.get()), &after) || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    return error("input changed or could not be read: " + path.string());
  struct stat current{};
  if (stat(path.c_str(), &current) || current.st_dev != after.st_dev || current.st_ino != after.st_ino)
    return error("input was replaced while hashing: " + path.string());
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(hash.get(), digest.data(), &length) != 1 || length != 32)
    return error("cannot finalize input hash");
  return hex(digest.data(), length);
}

fs::path resolve_path(const YAML::Node& value, const fs::path& directory) {
  if (!value.IsScalar() || value.Scalar().empty())
    throw std::invalid_argument("configured input path must be a nonempty scalar");
  fs::path path(value.Scalar());
  return path.is_absolute() ? path : directory / path;
}

absl::StatusOr<fs::path> resolve_parser(const YAML::Node& value, const fs::path& directory) {
  if (!value.IsScalar() || value.Scalar().empty())
    return error("custom parser path must be a nonempty scalar");
  fs::path requested(value.Scalar());
  // A basename has normal dynamic-loader search semantics. Relative paths with
  // a slash have been resolved against the inference configuration directory.
  if (requested.is_relative() && requested.has_parent_path())
    requested = directory / requested;
  // TensorRT parser libraries may register factories in process-global
  // registries. Keep their code resident after inspecting it, as inference
  // will consume those registrations during the immediately following preroll.
  std::unique_ptr<void, decltype(&dlclose)> library(
      dlopen(requested.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE), &dlclose);
  if (!library) {
    const char* reason = dlerror();
    return error("cannot resolve parser " + requested.string() + ": " + (reason ? reason : "unknown loader error"));
  }
  link_map* mapping = nullptr;
  if (dlinfo(library.get(), RTLD_DI_LINKMAP, &mapping) != 0 || !mapping || !mapping->l_name || !*mapping->l_name)
    return error("resolved parser does not identify its loaded file");
  std::error_code ec;
  const auto path = fs::canonical(mapping->l_name, ec);
  if (ec)
    return error("cannot resolve loaded parser file " + std::string(mapping->l_name));
  return path;
}

absl::Status append_file(std::string* material, const std::string& role, const fs::path& path) {
  const auto hash = digest_file(path);
  if (!hash.ok())
    return hash.status();
  *material += ":" + role + ":" + *hash;
  return absl::OkStatus();
}

absl::Status append_parser(std::string* material, const YAML::Node& properties, const fs::path& directory) {
  if (!properties["custom-lib-path"])
    return absl::OkStatus();
  auto path = resolve_parser(properties["custom-lib-path"], directory);
  if (!path.ok())
    return path.status();
  return append_file(material, "parser", *path);
}
} // namespace

absl::StatusOr<std::string> PlayerFrameDetectorModelIdentity(
    const fs::path& config_path,
    uint32_t component_id,
    const fs::path& engine_override) {
  auto config_hash = digest_file(config_path);
  if (!config_hash.ok())
    return config_hash.status();
  std::string material = "player-detector-model-v1:" + *config_hash + ":" + std::to_string(component_id);
  try {
    const auto config = YAML::LoadFile(config_path.string());
    const auto properties = config["property"];
    if (!properties.IsMap())
      return error("detector configuration requires a property map");
    const auto directory = config_path.parent_path();
    if (properties["onnx-file"]) {
      const auto model = resolve_path(properties["onnx-file"], directory);
      auto status = append_file(&material, "onnx", model);
      if (!status.ok())
        return status;
      auto external = OnnxExternalDataFiles(model);
      if (!external.ok())
        return external.status();
      for (const auto& tensor : *external) {
        status = append_file(&material, "external:" + tensor.generic_string(), model.parent_path() / tensor);
        if (!status.ok())
          return status;
      }
    } else {
      const auto engine =
          engine_override.empty() ? resolve_path(properties["model-engine-file"], directory) : engine_override;
      auto status = append_file(&material, "engine-only", engine);
      if (!status.ok())
        return status;
    }
    if (!engine_override.empty())
      material += ":engine-override-path:" + engine_override.generic_string();
    if (properties["labelfile-path"]) {
      auto status = append_file(&material, "labels", resolve_path(properties["labelfile-path"], directory));
      if (!status.ok())
        return status;
    }
    auto status = append_parser(&material, properties, directory);
    if (!status.ok())
      return status;
    // Reading/parsing and hashing must refer to the same configuration bytes.
    auto after = digest_file(config_path);
    if (!after.ok())
      return after.status();
    if (*after != *config_hash)
      return error("detector configuration changed while identifying inputs");
    return digest_string(material);
  } catch (const std::exception& exception) {
    return error(exception.what());
  }
}

absl::StatusOr<std::string> PlayerFrameDetectorRuntimeIdentity(
    const fs::path& config_path,
    const fs::path& actual_engine_path) {
  if (actual_engine_path.empty())
    return error("inference did not expose its actual TensorRT engine path");
  try {
    std::string material = "player-detector-runtime-v1";
    const auto engine =
        actual_engine_path.is_absolute() ? actual_engine_path : config_path.parent_path() / actual_engine_path;
    auto status = append_file(&material, "engine", engine);
    if (!status.ok())
      return status;
    const auto config = YAML::LoadFile(config_path.string());
    const auto properties = config["property"];
    if (!properties.IsMap())
      return error("detector configuration requires a property map");
    status = append_parser(&material, properties, config_path.parent_path());
    if (!status.ok())
      return status;
    return digest_string(material);
  } catch (const std::exception& exception) {
    return error(exception.what());
  }
}

} // namespace hm::pipeline
