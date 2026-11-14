#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::process_guard {
// Dedicated process only. Reaping consumes all of this process's child statuses.
absl::Status Enable();
bool Enabled();
// An expired deadline permits one nonretrying proof read, never another signal.
using Deadline = std::chrono::steady_clock::time_point;
absl::StatusOr<bool> Reap(Deadline deadline = Deadline::max());
absl::Status Signal(int number, Deadline deadline = Deadline::max());
absl::Status Retire(int64_t grace_ms, int64_t poll_ms);

std::string ReceiptHeader(const std::string& token, const std::string& nonce);
absl::StatusOr<char> ReadReceipt(int descriptor, const std::filesystem::path& path, const std::string& header);
// SIGSTOP/SIGCONT affect the worker group only; the guard remains responsive.
absl::Status RequestSignal(int descriptor, const std::filesystem::path& path, const std::string& header, int number);

// The command inherits stdin/stdout/stderr unchanged. Completion is recorded
// only after the root and every adopted descendant have exited and been reaped.
absl::StatusOr<int> Run(
    const std::filesystem::path& receipt,
    const std::string& token,
    const std::string& nonce,
    int64_t parent_pid,
    int64_t grace_ms,
    int64_t poll_ms,
    const std::vector<std::string>& command,
    int executable_descriptor = -1);
} // namespace hm::process_guard
