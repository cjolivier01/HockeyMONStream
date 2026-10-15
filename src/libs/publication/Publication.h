#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hm::publication {
namespace fs = std::filesystem;
using Progress = std::function<void(const std::string&)>;
struct PublishedOutput {
  fs::path path;
  std::string kind;
};

// The supervisor creates one private directory per run. These functions throw
// on an I/O or validation failure; callers must not announce success afterwards.
fs::path CreateRun(const fs::path& game, const fs::path& output_root, const std::string& id);
fs::path ReceiptPath(const fs::path& game, const std::string& id);
void BeginRun(const fs::path& manifest, bool telemetry_required = false);
bool PipelineCompleted(const fs::path& manifest);
void DiscardUnusedRun(const fs::path& manifest);
fs::path EnvironmentManifest();
fs::path EnvironmentWorkingDirectory();
void RegisterOutput(const fs::path& path, const std::string& kind, bool hevc = false);
void RequireTelemetry(bool required);
void CompletePipeline(bool success, int64_t start_time_ms);

// Retrying a published run verifies its destinations and resumes cleanup. A
// receipt survives removal of the working directory and can itself be resumed.
std::vector<PublishedOutput> Finalize(const fs::path& manifest_or_receipt, const Progress& progress = {});
int RunJob(
    const fs::path& game,
    const fs::path& output_root,
    const std::string& id,
    const std::vector<std::string>& command,
    const Progress& progress = {});

// Shared by desktop legacy publication and the headless transaction.
std::vector<std::string> RemuxArguments(const std::string& input, const std::string& output, bool hevc);
void CopyTelemetrySnapshot(const fs::path& source, const fs::path& destination, const std::string& game_id = {});
} // namespace hm::publication
