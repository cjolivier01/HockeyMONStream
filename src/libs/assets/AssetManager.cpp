#include "hstream/src/libs/assets/AssetManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

extern char** environ;

namespace hm::assets {

absl::Status internal::fsync_asset_parent_directory(const std::filesystem::path& target) {
  if (const char* injected = std::getenv("HM_TEST_ASSET_DIRECTORY_FSYNC_FAILURE");
      injected != nullptr && std::string(injected) == "1") {
    return absl::InternalError("Injected asset directory fsync failure");
  }
  const int directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0)
    return absl::InternalError("Unable to open asset directory for fsync: " + std::string(std::strerror(errno)));
  if (::fsync(directory) != 0) {
    const std::string message = std::strerror(errno);
    ::close(directory);
    return absl::InternalError("Unable to fsync asset directory: " + message);
  }
  if (::close(directory) != 0)
    return absl::InternalError("Unable to close asset directory after fsync: " + std::string(std::strerror(errno)));
  return absl::OkStatus();
}

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaximumGithubTokenBytes = 16 * 1024;
constexpr auto kGithubCliTimeout = std::chrono::seconds(5);

std::string trim_whitespace(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

std::string github_token_from_cli() {
  int output[2];
  if (::pipe2(output, O_CLOEXEC | O_NONBLOCK) != 0)
    return {};

  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    ::close(output[0]);
    ::close(output[1]);
    return {};
  }
  bool actions_ok = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0 &&
      ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
  if (output[0] != STDOUT_FILENO)
    actions_ok = actions_ok && ::posix_spawn_file_actions_addclose(&actions, output[0]) == 0;
  if (output[1] != STDOUT_FILENO)
    actions_ok = actions_ok && ::posix_spawn_file_actions_addclose(&actions, output[1]) == 0;
  if (!actions_ok) {
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(output[0]);
    ::close(output[1]);
    return {};
  }

  char executable[] = "gh";
  char auth[] = "auth";
  char token[] = "token";
  char hostname_option[] = "--hostname";
  char hostname[] = "github.com";
  char* arguments[] = {executable, auth, token, hostname_option, hostname, nullptr};
  pid_t child = -1;
  const int spawn_status = ::posix_spawnp(&child, executable, &actions, nullptr, arguments, environ);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(output[1]);
  if (spawn_status != 0) {
    ::close(output[0]);
    return {};
  }

  std::string result;
  bool output_closed = false;
  bool child_exited = false;
  bool failed = false;
  int child_status = 0;
  const auto deadline = std::chrono::steady_clock::now() + kGithubCliTimeout;
  while (!output_closed || !child_exited) {
    if (!output_closed) {
      std::array<char, 1024> buffer{};
      while (true) {
        const ssize_t count = ::read(output[0], buffer.data(), buffer.size());
        if (count > 0) {
          if (result.size() + static_cast<size_t>(count) > kMaximumGithubTokenBytes)
            failed = true;
          else
            result.append(buffer.data(), static_cast<size_t>(count));
          continue;
        }
        if (count == 0) {
          output_closed = true;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          failed = true;
          output_closed = true;
        }
        break;
      }
    }

    if (!child_exited) {
      const pid_t waited = ::waitpid(child, &child_status, WNOHANG);
      if (waited == child) {
        child_exited = true;
      } else if (waited < 0 && errno != EINTR) {
        failed = true;
        child_exited = true;
      }
    }
    if (output_closed && child_exited)
      break;

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (!child_exited) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
        }
      }
      failed = true;
      child_exited = true;
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int timeout = static_cast<int>(std::min(remaining, std::chrono::milliseconds(50)).count());
    if (output_closed)
      ::poll(nullptr, 0, timeout);
    else {
      pollfd descriptor{output[0], POLLIN | POLLHUP, 0};
      ::poll(&descriptor, 1, timeout);
    }
  }
  ::close(output[0]);

  if (failed || !child_exited || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    return {};
  result = trim_whitespace(std::move(result));
  if (std::any_of(result.begin(), result.end(), [](unsigned char c) { return std::isspace(c); }))
    return {};
  return result;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

bool is_yaml(const fs::path& path) {
  const std::string extension = lowercase(path.extension().string());
  return extension == ".yaml" || extension == ".yml";
}

std::string expand_environment(const std::string& raw) {
  std::string result;
  size_t index = 0;
  if (!raw.empty() && raw[0] == '~' && (raw.size() == 1 || raw[1] == '/')) {
    if (const char* home = std::getenv("HOME"); home != nullptr)
      result += home;
    index = 1;
  }
  while (index < raw.size()) {
    if (raw[index] != '$') {
      result.push_back(raw[index++]);
      continue;
    }
    size_t begin = index + 1;
    size_t end = begin;
    if (begin < raw.size() && raw[begin] == '{') {
      ++begin;
      end = raw.find('}', begin);
      if (end == std::string::npos) {
        result.push_back(raw[index++]);
        continue;
      }
      index = end + 1;
    } else {
      while (end < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[end])) || raw[end] == '_'))
        ++end;
      index = end;
    }
    const std::string name = raw.substr(begin, end - begin);
    if (const char* value = std::getenv(name.c_str()); value != nullptr)
      result += value;
  }
  return result;
}

fs::path resolve_path(const std::string& raw, const fs::path& base) {
  fs::path result(expand_environment(raw));
  if (!result.is_absolute())
    result = base / result;
  return result.lexically_normal();
}

bool enabled(const YAML::Node& node) {
  if (!node || !node.IsMap())
    return true;
  const YAML::Node value = node["enable"];
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

void collect_child_configs(const YAML::Node& node, const fs::path& base, std::vector<fs::path>* children) {
  if (!node || !enabled(node))
    return;
  if (node.IsMap()) {
    for (const auto& entry : node) {
      const std::string key = entry.first.as<std::string>();
      if (key == "config-file" && entry.second.IsScalar()) {
        fs::path child = resolve_path(entry.second.as<std::string>(), base);
        if (is_yaml(child))
          children->push_back(std::move(child));
      } else {
        collect_child_configs(entry.second, base, children);
      }
    }
  } else if (node.IsSequence()) {
    for (const YAML::Node& child : node)
      collect_child_configs(child, base, children);
  }
}

void normalize_specs(const YAML::Node& node, std::vector<YAML::Node>* specs) {
  if (!node)
    return;
  if (node.IsSequence()) {
    for (const YAML::Node& child : node)
      if (child.IsMap())
        normalize_specs(child, specs);
    return;
  }
  if (!node.IsMap())
    return;
  if (node["url"] || node["source"] || node["command"] || node["generate-command"] || node["generate_command"]) {
    specs->push_back(node);
    return;
  }
  for (const auto& entry : node)
    normalize_specs(entry.second, specs);
}

absl::StatusOr<AssetSpec> make_spec(const YAML::Node& node, const YAML::Node& config, const fs::path& path) {
  AssetSpec result;
  result.declaring_config = path;
  result.name = node["name"] ? node["name"].as<std::string>() : "unnamed asset";
  if (node["command"] || node["generate-command"] || node["generate_command"] || node["onnx-dynamic-batch"] ||
      node["onnx_dynamic_batch"]) {
    return absl::UnimplementedError(
        path.string() + ": runtime asset generation/mutation is forbidden; publish a checksummed artifact instead");
  }
  const YAML::Node url = node["url"] ? node["url"] : node["source"];
  if (!url || !url.IsScalar())
    return absl::InvalidArgumentError(path.string() + ": asset has no URL");
  result.url = url.as<std::string>();
  if (result.url.rfind("https://", 0) != 0)
    return absl::InvalidArgumentError(path.string() + ": asset URL must use HTTPS");
  if (!node["sha256"] || !node["sha256"].IsScalar())
    return absl::InvalidArgumentError(path.string() + ": remote asset requires sha256");
  result.sha256 = lowercase(node["sha256"].as<std::string>());
  if (result.sha256.size() != 64 ||
      !std::all_of(result.sha256.begin(), result.sha256.end(), [](unsigned char c) { return std::isxdigit(c); }))
    return absl::InvalidArgumentError(path.string() + ": sha256 must contain 64 hexadecimal characters");

  std::string raw_target;
  if (node["path"])
    raw_target = node["path"].as<std::string>();
  else if (node["file"])
    raw_target = node["file"].as<std::string>();
  else if (node["property"]) {
    const std::string property = node["property"].as<std::string>();
    if (!config["property"] || !config["property"][property])
      return absl::InvalidArgumentError(path.string() + ": asset property does not exist: " + property);
    raw_target = config["property"][property].as<std::string>();
  } else {
    return absl::InvalidArgumentError(path.string() + ": asset needs path, file, or property");
  }
  result.target = resolve_path(raw_target, path.parent_path());
  std::error_code canonical_error;
  const fs::path canonical_target = fs::weakly_canonical(result.target, canonical_error);
  if (!canonical_error)
    result.target = canonical_target;
  return result;
}

bool path_is_under(const fs::path& path, const fs::path& root) {
  auto path_part = path.begin();
  for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++path_part)
    if (path_part == path.end() || *path_part != *root_part)
      return false;
  return true;
}

std::vector<fs::path> allowed_roots(const AssetSpec& spec) {
  std::vector<fs::path> roots = {
      (spec.declaring_config.parent_path().parent_path() / "pretrained").lexically_normal(),
      "/mnt/data/pretrained",
      "/opt/hmstream/pretrained",
  };
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    roots.push_back((fs::path(home) / ".cache" / "hmstream").lexically_normal());
  if (const char* extra = std::getenv("HMSTREAM_ASSET_ROOTS"); extra != nullptr) {
    std::istringstream values(extra);
    std::string value;
    while (std::getline(values, value, ':'))
      if (!value.empty())
        roots.push_back(fs::path(value).lexically_normal());
  }
  return roots;
}

absl::Status validate_target(const AssetSpec& spec) {
  const fs::path target = fs::absolute(spec.target).lexically_normal();
  bool allowed = false;
  for (const fs::path& root : allowed_roots(spec)) {
    std::error_code root_error;
    fs::path canonical_root = fs::weakly_canonical(fs::absolute(root), root_error);
    if (root_error)
      canonical_root = fs::absolute(root).lexically_normal();
    if (path_is_under(target, canonical_root)) {
      allowed = true;
      break;
    }
  }
  if (!allowed)
    return absl::PermissionDeniedError("Asset target is outside approved roots: " + target.string());
  std::error_code error;
  if (fs::is_symlink(fs::symlink_status(target, error)))
    return absl::PermissionDeniedError("Asset target may not be a symlink: " + target.string());
  fs::path current;
  for (const fs::path& part : target.parent_path()) {
    current /= part;
    if (!fs::exists(current, error)) {
      error.clear();
      continue;
    }
    if (fs::is_symlink(fs::symlink_status(current, error)))
      return absl::PermissionDeniedError("Asset parent may not contain symlinks: " + current.string());
  }
  return absl::OkStatus();
}

struct DownloadState {
  FILE* file;
  size_t received;
  size_t maximum;
};

size_t write_download(char* data, size_t size, size_t count, void* opaque) {
  auto* state = static_cast<DownloadState*>(opaque);
  const size_t bytes = size * count;
  if (bytes > state->maximum - state->received)
    return 0;
  const size_t written = std::fwrite(data, 1, bytes, state->file);
  state->received += written;
  return written;
}

absl::Status download(const AssetSpec& spec, const fs::path& temporary, size_t maximum, size_t* received) {
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file)
    return absl::InternalError("Unable to open temporary asset: " + temporary.string());
  DownloadState state{file, 0, maximum};
  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fclose(file);
    return absl::InternalError("Unable to initialize HTTPS client");
  }
  struct curl_slist* headers = nullptr;
  const bool github_api = spec.url.rfind("https://api.github.com/", 0) == 0;
  bool github_token_available = false;
  if (github_api) {
    headers = curl_slist_append(headers, "Accept: application/octet-stream");
    const std::string token = internal::github_token();
    if (!token.empty()) {
      github_token_available = true;
      headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    }
  }
  curl_easy_setopt(curl, CURLOPT_URL, spec.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "hmstream-assets/1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  const CURLcode result = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  const bool flushed = std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
  std::fclose(file);
  if (result != CURLE_OK) {
    std::string message = "Asset download failed: " + std::string(curl_easy_strerror(result));
    if (github_api && !github_token_available)
      message += "; authenticate with gh or set GH_TOKEN/GITHUB_TOKEN when accessing private GitHub release assets";
    return absl::UnavailableError(message);
  }
  if (!flushed)
    return absl::InternalError("Unable to durably flush downloaded asset");
  *received = state.received;
  return absl::OkStatus();
}

absl::Status ensure_one(const AssetSpec& spec, const Limits& limits, size_t* total) {
  auto status = validate_target(spec);
  if (!status.ok())
    return status;
  std::error_code error;
  // Packaged assets live under root-owned /opt/hmstream and are intentionally
  // read-only to ordinary users. Hash an existing immutable file before
  // attempting to create a sibling lock; atomic publishers cannot change the
  // inode being read underneath this verification.
  if (fs::is_regular_file(spec.target, error) && !error && fs::file_size(spec.target, error) > 0 && !error) {
    auto hash = AssetManager::Sha256(spec.target);
    if (hash.ok() && *hash == spec.sha256)
      return absl::OkStatus();
  }
  error.clear();
  fs::create_directories(spec.target.parent_path(), error);
  if (error)
    return absl::PermissionDeniedError("Unable to create asset directory: " + error.message());

  const fs::path lock_path = spec.target.string() + ".lock";
  const int lock = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0)
    return absl::InternalError("Unable to open asset lock: " + lock_path.string());
  struct Close {
    int fd;
    ~Close() {
      ::close(fd);
    }
  } lock_cleanup{lock};
  if (::flock(lock, LOCK_EX) != 0)
    return absl::InternalError("Unable to lock asset target");
  if (fs::is_regular_file(spec.target, error) && !error && fs::file_size(spec.target, error) > 0 && !error) {
    auto hash = AssetManager::Sha256(spec.target);
    if (hash.ok() && *hash == spec.sha256)
      return absl::OkStatus();
    std::cerr << "Cached asset checksum mismatch; downloading a verified replacement: " << spec.target << '\n';
  }

  std::string pattern = (spec.target.parent_path() / ("." + spec.target.filename().string() + ".XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int fd = ::mkstemp(writable.data());
  if (fd < 0)
    return absl::InternalError("Unable to create temporary asset file");
  ::close(fd);
  const fs::path temporary(writable.data());
  struct Remove {
    fs::path path;
    ~Remove() {
      std::error_code ignored;
      fs::remove(path, ignored);
    }
  } temporary_cleanup{temporary};
  std::cout << "Downloading pretrained asset: " << spec.target << '\n';
  size_t received = 0;
  const size_t remaining = limits.maximum_total_bytes - std::min(*total, limits.maximum_total_bytes);
  status = download(spec, temporary, std::min(limits.maximum_asset_bytes, remaining), &received);
  if (!status.ok())
    return status;
  *total += received;
  auto hash = AssetManager::Sha256(temporary);
  if (!hash.ok())
    return hash.status();
  if (*hash != spec.sha256)
    return absl::DataLossError("Downloaded asset checksum mismatch for " + spec.target.string());
  status = validate_target(spec);
  if (!status.ok())
    return status;
  if (::chmod(temporary.c_str(), 0644) != 0)
    return absl::InternalError("Unable to set asset permissions");
  fs::rename(temporary, spec.target, error);
  if (error)
    return absl::InternalError("Unable to publish asset: " + error.message());
  return internal::fsync_asset_parent_directory(spec.target);
}

} // namespace

std::string internal::github_token() {
  for (const char* name : {"GH_TOKEN", "GITHUB_TOKEN"}) {
    if (const char* token = std::getenv(name); token != nullptr && *token != '\0') {
      std::string result = trim_whitespace(token);
      if (!std::any_of(result.begin(), result.end(), [](unsigned char c) { return std::isspace(c); }))
        return result;
    }
  }
  return github_token_from_cli();
}

absl::StatusOr<std::vector<AssetSpec>> AssetManager::Discover(
    const std::vector<fs::path>& configs,
    const Limits& limits) {
  struct Pending {
    fs::path path;
    size_t depth;
  };
  std::vector<Pending> pending;
  for (const fs::path& path : configs)
    pending.push_back({fs::absolute(path).lexically_normal(), 0});
  std::set<fs::path> visited;
  std::vector<AssetSpec> assets;
  for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
    const Pending item = pending[cursor];
    if (item.depth > limits.maximum_recursion_depth)
      return absl::ResourceExhaustedError("Asset config recursion is too deep");
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(item.path, error);
    if (error || !fs::is_regular_file(canonical)) {
      return absl::NotFoundError("Asset config does not exist or is not a regular file: " + item.path.string());
    }
    if (!visited.insert(canonical).second)
      continue;
    if (visited.size() > limits.maximum_configs)
      return absl::ResourceExhaustedError("Too many asset config files");
    YAML::Node config;
    try {
      config = YAML::LoadFile(canonical.string());
    } catch (const YAML::Exception& exception) {
      return absl::InvalidArgumentError("Unable to parse asset config " + canonical.string() + ": " + exception.what());
    }
    std::vector<YAML::Node> specs;
    for (const char* key : {"pretrained-assets", "assets", "downloads"})
      normalize_specs(config[key], &specs);
    for (const YAML::Node& node : specs) {
      auto spec = make_spec(node, config, canonical);
      if (!spec.ok())
        return spec.status();
      assets.push_back(std::move(*spec));
      if (assets.size() > limits.maximum_assets)
        return absl::ResourceExhaustedError("Too many pretrained assets");
    }
    std::vector<fs::path> children;
    collect_child_configs(config, canonical.parent_path(), &children);
    for (const fs::path& child : children)
      pending.push_back({child, item.depth + 1});
  }
  return assets;
}

absl::Status AssetManager::Ensure(const std::vector<fs::path>& configs, const Limits& limits) {
  auto assets = Discover(configs, limits);
  if (!assets.ok())
    return assets.status();
  static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK)
    return absl::InternalError("Unable to initialize HTTPS asset manager");
  size_t total = 0;
  for (const AssetSpec& spec : *assets) {
    auto status = ensure_one(spec, limits, &total);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

absl::Status AssetManager::Verify(const std::vector<fs::path>& configs, const Limits& limits) {
  auto assets = Discover(configs, limits);
  if (!assets.ok())
    return assets.status();
  for (const AssetSpec& spec : *assets) {
    auto status = validate_target(spec);
    if (!status.ok())
      return status;
    std::error_code error;
    if (!fs::is_regular_file(spec.target, error) || error) {
      return absl::NotFoundError("Required pretrained asset is unavailable: " + spec.target.string());
    }
    auto hash = Sha256(spec.target);
    if (!hash.ok())
      return hash.status();
    if (*hash != spec.sha256)
      return absl::DataLossError("Pretrained asset checksum mismatch for " + spec.target.string());
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> AssetManager::Sha256(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return absl::NotFoundError("Unable to open asset for hashing: " + path.string());
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    if (context)
      EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to initialize SHA256");
  }
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const std::streamsize count = input.gcount();
    if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(count)) != 1) {
      EVP_MD_CTX_free(context);
      return absl::InternalError("Unable to update SHA256");
    }
  }
  if (!input.eof()) {
    EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to read asset while hashing");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
    EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to finalize SHA256");
  }
  EVP_MD_CTX_free(context);
  std::ostringstream value;
  value << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index)
    value << std::setw(2) << static_cast<unsigned>(digest[index]);
  return value.str();
}

absl::StatusOr<std::string> AssetManager::Sha256Bytes(std::string_view contents) {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    if (context)
      EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to initialize SHA256");
  }
  if (EVP_DigestUpdate(context, contents.data(), contents.size()) != 1) {
    EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to update SHA256");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
    EVP_MD_CTX_free(context);
    return absl::InternalError("Unable to finalize SHA256");
  }
  EVP_MD_CTX_free(context);
  std::ostringstream value;
  value << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index)
    value << std::setw(2) << static_cast<unsigned>(digest[index]);
  return value.str();
}

} // namespace hm::assets
