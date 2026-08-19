#include "hstream/src/libs/common/BaselineConfig.h"

#include <cstdlib>
#include <system_error>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace hm::baseline_config {
namespace {

namespace fs = std::filesystem;

void append_candidate(std::vector<fs::path>* candidates, const fs::path& candidate) {
  if (!candidates || candidate.empty())
    return;
  const fs::path normalized = candidate.lexically_normal();
  for (const fs::path& existing : *candidates) {
    if (existing == normalized)
      return;
  }
  candidates->push_back(normalized);
}

void append_ancestor_config_directories(std::vector<fs::path>* candidates, fs::path path) {
  if (path.empty())
    return;
  for (int depth = 0; depth < 12 && !path.empty(); ++depth) {
    append_candidate(candidates, path / "configs");
    const fs::path parent = path.parent_path();
    if (parent == path)
      break;
    path = parent;
  }
}

bool contains_baseline(const fs::path& root) {
  std::error_code error;
  return fs::is_regular_file(root / kBaselineFilename, error) && !error;
}

std::string describe_candidates(const std::vector<fs::path>& candidates) {
  std::string result;
  for (const fs::path& candidate : candidates) {
    if (!result.empty())
      result += ", ";
    result += candidate.string();
  }
  return result;
}

} // namespace

absl::StatusOr<fs::path> resolve_root() {
  if (const char* configured = std::getenv("HM_CONFIG_ROOT"); configured && *configured) {
    const fs::path root = fs::path(configured).lexically_normal();
    if (!contains_baseline(root)) {
      return absl::NotFoundError(
          absl::StrCat("HM_CONFIG_ROOT does not contain a readable ", kBaselineFilename, ": ", root.string()));
    }
    return root;
  }

  std::vector<fs::path> candidates;
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  if (test_srcdir && *test_srcdir) {
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    append_candidate(
        &candidates,
        fs::path(test_srcdir) / (test_workspace && *test_workspace ? test_workspace : "kstream") / "configs");
  }
  if (const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY"); workspace && *workspace)
    append_candidate(&candidates, fs::path(workspace) / "configs");

  std::error_code error;
  const fs::path executable = fs::read_symlink("/proc/self/exe", error);
  if (!error && !executable.empty()) {
    // Installed binaries live in /opt/hstream/bin. Bazel binaries and tests
    // resolve through an execroot or runfiles tree whose ancestors contain the
    // data dependency under configs/.
    append_candidate(&candidates, executable.parent_path().parent_path() / "configs");
    append_ancestor_config_directories(&candidates, executable.parent_path());
    append_candidate(&candidates, fs::path(executable.string() + ".runfiles") / "kstream" / "configs");
  }

  append_ancestor_config_directories(&candidates, fs::current_path(error));
  append_candidate(&candidates, "/opt/hstream/configs");

  for (const fs::path& candidate : candidates) {
    if (contains_baseline(candidate))
      return candidate;
  }
  return absl::NotFoundError(
      absl::StrCat(
          "Could not locate HStream's bundled ",
          kBaselineFilename,
          ". Set HM_CONFIG_ROOT to an explicit config directory. Searched: ",
          describe_candidates(candidates)));
}

absl::StatusOr<BaselineConfig> load_from_root(const fs::path& root) {
  if (root.empty())
    return absl::InvalidArgumentError("Baseline config root is empty");
  const fs::path path = root / kBaselineFilename;
  if (!contains_baseline(root))
    return absl::NotFoundError(absl::StrCat("Baseline config does not exist or is not readable: ", path.string()));
  try {
    YAML::Node values = YAML::LoadFile(path.string());
    if (!values || !values.IsMap())
      return absl::InvalidArgumentError(absl::StrCat("Baseline config must be a YAML map: ", path.string()));
    return BaselineConfig{root, path, std::move(values)};
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to load baseline config ", path.string(), ": ", error.what()));
  }
}

absl::StatusOr<BaselineConfig> load() {
  auto root = resolve_root();
  if (!root.ok())
    return root.status();
  return load_from_root(*root);
}

} // namespace hm::baseline_config
