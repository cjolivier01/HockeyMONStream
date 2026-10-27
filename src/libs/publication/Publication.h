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

// The supervisor creates one private directory per run and durably binds it in
// the game receipt before starting a producer. Retries retain that location if
// the configured output root changes. These functions throw on I/O or validation
// failure; callers must not announce success afterwards.
fs::path CreateRun(const fs::path& game, const fs::path& output_root, const std::string& id);
// Shared by exporters and the supervisor; throws for an unsafe receipt identity.
void ValidateRunId(const std::string& id);
fs::path ReceiptPath(const fs::path& game, const std::string& id);
void BeginRun(const fs::path& manifest, bool telemetry_required = false);
bool PipelineCompleted(const fs::path& manifest);
fs::path EnvironmentManifest();
fs::path EnvironmentWorkingDirectory();
// Validate a producer's manifest before allocating resources or writing outputs.
// A producer lacking telemetry must not silently clear the supervisor's request.
void ValidatePipelineStart(bool supports_telemetry);
void RegisterOutput(const fs::path& path, const std::string& kind, bool hevc = false, int expected_fd = -1);
// Explicit calibration replacement may retire a registered output. Caps-only
// replacement must additionally prove that the old telemetry contains no samples.
void RetireOutput(const fs::path& path, bool require_empty_telemetry = false);
// Record an exact owned file/tree, including scratch outside the private run.
void RegisterScratch(const fs::path& path, const fs::path& manifest = {}, int expected_fd = -1);
void RequireTelemetry(bool required);
void CompletePipeline(bool success, int64_t start_time_ms);

// Retrying a published run verifies its destinations and resumes cleanup. A
// receipt survives removal of the working directory and can itself be resumed.
std::vector<PublishedOutput> Finalize(const fs::path& manifest_or_receipt, const Progress& progress = {});
// Telemetry-only admission requires a recording and rejects video outputs.
// Ordinary admission continues to require video, even when telemetry exists.
int RunJob(
    const fs::path& game,
    const fs::path& output_root,
    const std::string& id,
    const std::vector<std::string>& command,
    const Progress& progress = {},
    bool telemetry_only = false);

// Shared by desktop legacy publication and the headless transaction.
std::vector<std::string> RemuxArguments(const std::string& input, const std::string& output, bool hevc);
void CopyTelemetrySnapshot(const fs::path& source, const fs::path& destination, const std::string& game_id = {});
} // namespace hm::publication
