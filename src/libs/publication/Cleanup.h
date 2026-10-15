#pragma once

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <functional>

namespace hm::publication {

// The callback must durably save run outside every directory being removed.
// Callers serialize cleanup for a run. One pending operation is stored in
// run["cleanup_pending"], so it can be recovered from the publication receipt.
using CleanupCheckpoint = std::function<void()>;
void ResumeCleanup(YAML::Node& run, const CleanupCheckpoint& checkpoint);
void CleanupOwned(
    const std::filesystem::path& path,
    const YAML::Node& expected,
    bool contents,
    YAML::Node& run,
    const CleanupCheckpoint& checkpoint);

} // namespace hm::publication
