#include "hstream/src/libs/common/UserConfig.h"

#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace hm::user_config {
namespace {

namespace fs = std::filesystem;

absl::StatusOr<fs::path> home_directory() {
  const char* home = ::getenv("HOME");
  if (!home || !*home) {
    return absl::FailedPreconditionError("HOME is not set; cannot resolve per-user HStream paths");
  }
  return fs::path(home);
}

absl::Status write_new_file(const fs::path& path, const std::string& contents) {
  std::string temporary_pattern = path.string() + ".tmp.XXXXXX";
  std::vector<char> temporary_name(temporary_pattern.begin(), temporary_pattern.end());
  temporary_name.push_back('\0');
  const int fd = ::mkstemp(temporary_name.data());
  if (fd < 0) {
    return absl::InternalError(
        absl::StrCat("Failed to create a temporary user config beside ", path.string(), ": ", std::strerror(errno)));
  }
  const fs::path temporary_path(temporary_name.data());
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);

  size_t written = 0;
  while (written < contents.size()) {
    const ssize_t result = ::write(fd, contents.data() + written, contents.size() - written);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      const int saved_errno = errno;
      ::close(fd);
      std::error_code ignored;
      fs::remove(temporary_path, ignored);
      return absl::InternalError(absl::StrCat("Failed to write ", path.string(), ": ", std::strerror(saved_errno)));
    }
    written += static_cast<size_t>(result);
  }
  if (::fsync(fd) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    return absl::InternalError(absl::StrCat("Failed to sync ", path.string(), ": ", std::strerror(saved_errno)));
  }
  if (::close(fd) != 0) {
    const int saved_errno = errno;
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    return absl::InternalError(absl::StrCat("Failed to close ", path.string(), ": ", std::strerror(saved_errno)));
  }
  if (::link(temporary_path.c_str(), path.c_str()) != 0 && errno != EEXIST) {
    const int saved_errno = errno;
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    return absl::InternalError(absl::StrCat("Failed to publish ", path.string(), ": ", std::strerror(saved_errno)));
  }
  std::error_code ignored;
  fs::remove(temporary_path, ignored);
  return absl::OkStatus();
}

absl::Status ensure_first_run_file(const fs::path& path) {
  std::error_code ec;
  if (fs::exists(path, ec))
    return absl::OkStatus();
  if (ec)
    return absl::InternalError(absl::StrCat("Failed to inspect ", path.string(), ": ", ec.message()));

  const fs::path parent = path.parent_path();
  if (::mkdir(parent.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
    return absl::InternalError(absl::StrCat("Failed to create ", parent.string(), ": ", std::strerror(errno)));
  }
  if (!fs::is_directory(parent, ec) || ec) {
    return absl::InternalError(absl::StrCat("User config parent is not a directory: ", parent.string()));
  }

  YAML::Node initial(YAML::NodeType::Map);
  auto home = home_directory();
  if (!home.ok())
    return home.status();
  initial[kPathsKey][kOutputRootKey] = (*home / "hstream_output").string();
  return write_new_file(path, YAML::Dump(initial) + "\n");
}

absl::StatusOr<fs::path> configured_path(
    const YAML::Node& config,
    const char* environment_name,
    const char* config_key,
    const char* default_leaf) {
  const char* environment_value = ::getenv(environment_name);
  if (environment_value && *environment_value)
    return fs::path(environment_value);

  auto home = home_directory();
  if (!home.ok())
    return home.status();
  if (config && config.IsMap()) {
    const YAML::Node paths = config[kPathsKey];
    const YAML::Node value = paths && paths.IsMap() ? paths[config_key] : YAML::Node();
    if (value && value.IsScalar()) {
      const std::string configured = value.as<std::string>();
      if (!configured.empty()) {
        if (configured == "~")
          return *home;
        if (configured.size() > 1 && configured[0] == '~' && configured[1] == '/')
          return *home / configured.substr(2);
        const fs::path configured_path(configured);
        return configured_path.is_absolute() ? configured_path : *home / configured_path;
      }
    }
  }
  return *home / default_leaf;
}

} // namespace

absl::StatusOr<fs::path> file_path() {
  auto home = home_directory();
  if (!home.ok())
    return home.status();
  return *home / ".hstream" / "hstream.yaml";
}

absl::StatusOr<YAML::Node> load_or_create() {
  auto path = file_path();
  if (!path.ok())
    return path.status();
  const absl::Status created = ensure_first_run_file(*path);
  if (!created.ok())
    return created;

  try {
    YAML::Node config = YAML::LoadFile(path->string());
    if (!config || config.IsNull())
      return YAML::Node(YAML::NodeType::Map);
    if (!config.IsMap())
      return absl::InvalidArgumentError(absl::StrCat("User config must be a YAML map: ", path->string()));
    return config;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(absl::StrCat("Failed to load user config ", path->string(), ": ", error.what()));
  }
}

absl::StatusOr<fs::path> game_root(const YAML::Node& config) {
  return configured_path(config, "HM_GAME_DIR", kGameRootKey, "Videos");
}

absl::StatusOr<fs::path> output_root(const YAML::Node& config) {
  return configured_path(config, "HM_OUTPUT_WORK_DIR", kOutputRootKey, "hstream_output");
}

} // namespace hm::user_config
