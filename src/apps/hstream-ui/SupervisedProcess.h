#pragma once

#include <QtCore/QProcess>
#include <filesystem>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::ui {

// Wrap the configured command in an isolated subreaper. Call immediately before
// start(); a pending permit exists before QProcess can launch any descendant.
// The guard inherits the exact command streams/environment and owns only this
// command's descendants. Qt remains the owner of every QProcess child.
// If command_game names a different private workspace, reserve it before
// launch and release its owner only after this command's cleanup is proven.
absl::Status PrepareSupervisedProcess(
    QProcess& process,
    const std::filesystem::path& game,
    const std::string& token,
    int64_t grace_ms,
    int64_t poll_ms,
    bool new_session,
    const std::filesystem::path& command_game = {});

// Signal descendants through the guard's control receipt, never the guard.
// SIGKILL therefore still permits the guard to reap to ECHILD and publish proof.
absl::Status SignalSupervisedProcess(QProcess& process, int signal);
absl::StatusOr<bool> SupervisedProcessStopped(const QProcess& process);
absl::StatusOr<bool> SupervisedProcessPaused(const QProcess& process);
bool HasSupervisedProcess(const QProcess& process);

} // namespace hm::ui
