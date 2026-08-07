#include "hstream/src/libs/assets/AssetManager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <unistd.h>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool process_is_running(pid_t process) {
  if (::kill(process, 0) != 0)
    return false;
  std::ifstream stat("/proc/" + std::to_string(process) + "/stat");
  std::string field;
  for (int index = 0; index < 3 && stat >> field; ++index) {
  }
  return field != "Z";
}

bool wait_for_process_to_stop(pid_t process) {
  for (int attempt = 0; process > 0 && process_is_running(process) && attempt < 100; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return process > 0 && !process_is_running(process);
}
} // namespace

int main(int, char**) {
  if (const char* pid_file = std::getenv("HM_TEST_GH_TRACED_PID_FILE"); pid_file != nullptr) {
    std::ofstream(pid_file) << ::getpid() << '\n';
    ::close(STDOUT_FILENO);
    if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0)
      return 71;
    ::raise(SIGSTOP);
    while (true)
      ::pause();
  }
  if (const char* pid_file = std::getenv("HM_TEST_GH_ESCAPE_GROUP_PID_FILE"); pid_file != nullptr) {
    const pid_t parent_group = ::getpgid(::getppid());
    if (parent_group < 0 || ::setpgid(0, parent_group) != 0)
      return 70;
    std::ofstream(pid_file) << ::getpid() << '\n';
    while (true)
      ::pause();
  }
  bool ok = true;
  namespace fs = std::filesystem;
  const fs::path root =
      fs::weakly_canonical(fs::temp_directory_path()) / ("hmstream-assets-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "configs");
  fs::create_directories(root / "pretrained");
  {
    const char* original_gh_token_value = std::getenv("GH_TOKEN");
    const char* original_github_token_value = std::getenv("GITHUB_TOKEN");
    const char* original_path_value = std::getenv("PATH");
    const std::string original_gh_token = original_gh_token_value == nullptr ? "" : original_gh_token_value;
    const std::string original_github_token = original_github_token_value == nullptr ? "" : original_github_token_value;
    const std::string original_path = original_path_value == nullptr ? "" : original_path_value;
    fs::create_directories(root / "bin");
    {
      std::ofstream gh(root / "bin" / "gh");
      gh << "#!/bin/sh\n"
            "if [ -n \"${HM_TEST_GH_FLOOD_PID_FILE:-}\" ]; then\n"
            "  printf '%s\\n' \"$$\" > \"$HM_TEST_GH_FLOOD_PID_FILE\"\n"
            "  while :; do printf 0123456789abcdef0123456789abcdef; done\n"
            "fi\n"
            "if [ -n \"${HM_TEST_GH_CHILD_PID_FILE:-}\" ]; then\n"
            "  /bin/sleep 30 &\n"
            "  printf '%s\\n' \"$!\" > \"$HM_TEST_GH_CHILD_PID_FILE\"\n"
            "  wait\n"
            "fi\n"
            "if [ \"$#\" -eq 4 ] && [ \"$1\" = auth ] && [ \"$2\" = token ] && "
            "[ \"$3\" = --hostname ] && [ \"$4\" = github.com ]; then\n"
            "  if [ -n \"${GH_TOKEN+x}\" ] || [ -n \"${GITHUB_TOKEN+x}\" ]; then\n"
            "    printf leaked-token-environment\n"
            "  elif [ -e \"/proc/self/fd/${HM_TEST_SENTINEL_FD:-missing}\" ]; then\n"
            "    printf leaked-fd\n"
            "  else\n"
            "    printf cli-token\n"
            "  fi\n"
            "  exit 0\n"
            "fi\n"
            "exit 1\n";
    }
    fs::permissions(root / "bin" / "gh", fs::perms::owner_all);
    ::setenv("PATH", (root / "bin").c_str(), 1);
    ::setenv("GH_TOKEN", "  gh-environment-token\n", 1);
    ::setenv("GITHUB_TOKEN", "github-environment-token", 1);
    ok &= expect(hm::assets::internal::github_token() == "gh-environment-token", "GH_TOKEN must take precedence");
    ::unsetenv("GH_TOKEN");
    ok &=
        expect(hm::assets::internal::github_token() == "github-environment-token", "GITHUB_TOKEN must be the fallback");
    ::setenv("GH_TOKEN", " \t\n", 1);
    ok &= expect(
        hm::assets::internal::github_token() == "github-environment-token",
        "a blank GH_TOKEN must not suppress later fallbacks");
    ::unsetenv("GH_TOKEN");
    ::unsetenv("GITHUB_TOKEN");
    ::setenv("GH_TOKEN", " \t\n", 1);
    ::setenv("GITHUB_TOKEN", "invalid token", 1);
    ok &= expect(
        hm::assets::internal::github_token() == "cli-token",
        "invalid token variables must not mask the gh CLI credential store");
    ::unsetenv("GH_TOKEN");
    ::unsetenv("GITHUB_TOKEN");
    const int sentinel = ::open((root / "sentinel").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    const int inherited_sentinel = sentinel < 0 ? -1 : ::fcntl(sentinel, F_DUPFD, 200);
    if (sentinel >= 0)
      ::close(sentinel);
    ::setenv("HM_TEST_SENTINEL_FD", std::to_string(inherited_sentinel).c_str(), 1);
    ok &= expect(
        inherited_sentinel >= 0 && hm::assets::internal::github_token() == "cli-token",
        "the authenticated gh CLI must be the fallback without inheriting unrelated descriptors");
    ::unsetenv("HM_TEST_SENTINEL_FD");
    if (inherited_sentinel >= 0)
      ::close(inherited_sentinel);
    const fs::path child_pid_file = root / "gh-child.pid";
    ::setenv("HM_TEST_GH_CHILD_PID_FILE", child_pid_file.c_str(), 1);
    ok &= expect(
        hm::assets::internal::github_token(std::chrono::milliseconds(500)).empty(),
        "a timed-out gh CLI must leave the token unavailable");
    ::unsetenv("HM_TEST_GH_CHILD_PID_FILE");
    pid_t spawned_child = -1;
    {
      std::ifstream child_pid(child_pid_file);
      child_pid >> spawned_child;
    }
    const bool child_stopped = wait_for_process_to_stop(spawned_child);
    if (!child_stopped && spawned_child > 0)
      ::kill(spawned_child, SIGKILL);
    ok &= expect(child_stopped, "timing out gh must terminate its descendant processes");
    const fs::path flood_pid_file = root / "gh-flood.pid";
    ::setenv("HM_TEST_GH_FLOOD_PID_FILE", flood_pid_file.c_str(), 1);
    const auto flood_started = std::chrono::steady_clock::now();
    ok &= expect(
        hm::assets::internal::github_token(std::chrono::seconds(2)).empty(),
        "excessive continuous gh output must be rejected");
    const auto flood_elapsed = std::chrono::steady_clock::now() - flood_started;
    ::unsetenv("HM_TEST_GH_FLOOD_PID_FILE");
    pid_t flood_process = -1;
    {
      std::ifstream flood_pid(flood_pid_file);
      flood_pid >> flood_process;
    }
    const bool flood_stopped = wait_for_process_to_stop(flood_process);
    if (!flood_stopped && flood_process > 0)
      ::kill(flood_process, SIGKILL);
    ok &= expect(
        flood_elapsed < std::chrono::milliseconds(1500) && flood_stopped,
        "the output limit must promptly terminate a continuously writing gh process");
    fs::remove(root / "bin" / "gh");
    fs::create_symlink(fs::read_symlink("/proc/self/exe"), root / "bin" / "gh");
    const fs::path escaped_pid_file = root / "gh-escaped.pid";
    ::setenv("HM_TEST_GH_ESCAPE_GROUP_PID_FILE", escaped_pid_file.c_str(), 1);
    const auto escaped_started = std::chrono::steady_clock::now();
    ok &= expect(
        hm::assets::internal::github_token(std::chrono::milliseconds(500)).empty(),
        "a gh process that leaves its assigned group must still time out");
    const auto escaped_elapsed = std::chrono::steady_clock::now() - escaped_started;
    ::unsetenv("HM_TEST_GH_ESCAPE_GROUP_PID_FILE");
    pid_t escaped_process = -1;
    {
      std::ifstream escaped_pid(escaped_pid_file);
      escaped_pid >> escaped_process;
    }
    const bool escaped_stopped = wait_for_process_to_stop(escaped_process);
    if (!escaped_stopped && escaped_process > 0)
      ::kill(escaped_process, SIGKILL);
    ok &= expect(
        escaped_elapsed < std::chrono::seconds(2) && escaped_stopped,
        "timeout must directly terminate and reap a gh process outside its original group");
    const fs::path traced_pid_file = root / "gh-traced.pid";
    ::setenv("HM_TEST_GH_TRACED_PID_FILE", traced_pid_file.c_str(), 1);
    ok &= expect(
        hm::assets::internal::github_token(std::chrono::milliseconds(500)).empty(),
        "a traced and stopped gh process must remain owned through timeout cleanup");
    ::unsetenv("HM_TEST_GH_TRACED_PID_FILE");
    pid_t traced_process = -1;
    {
      std::ifstream traced_pid(traced_pid_file);
      traced_pid >> traced_process;
    }
    const bool traced_stopped = wait_for_process_to_stop(traced_process);
    if (!traced_stopped && traced_process > 0)
      ::kill(traced_process, SIGKILL);
    ok &= expect(traced_stopped, "timeout must terminate and reap a traced gh process");
    ::setenv("PATH", (root / "missing-bin").c_str(), 1);
    ok &= expect(hm::assets::internal::github_token().empty(), "a missing gh CLI must leave the token unavailable");
    if (original_gh_token_value == nullptr)
      ::unsetenv("GH_TOKEN");
    else
      ::setenv("GH_TOKEN", original_gh_token.c_str(), 1);
    if (original_github_token_value == nullptr)
      ::unsetenv("GITHUB_TOKEN");
    else
      ::setenv("GITHUB_TOKEN", original_github_token.c_str(), 1);
    if (original_path_value == nullptr)
      ::unsetenv("PATH");
    else
      ::setenv("PATH", original_path.c_str(), 1);
  }
  {
    std::ofstream asset(root / "pretrained" / "model.bin");
    asset << "native asset\n";
  }
  auto hash = hm::assets::AssetManager::Sha256(root / "pretrained" / "model.bin");
  ok &= expect(hash.ok(), "fixture hash must compute");
  auto byte_hash = hm::assets::AssetManager::Sha256Bytes("native asset\n");
  ok &= expect(byte_hash.ok() && hash.ok() && *byte_hash == *hash, "byte and file SHA256 must agree");
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
