#pragma once

#include <string>
#include <string_view>

namespace hm::diagnostics {

// One session per process, installed before SDK/GUI initialization. Failure to
// create local diagnostics never prevents startup. No arguments or environment
// values are recorded. Files remain private to the current user.
bool Initialize(const char* component, const char* executable) noexcept;
std::string Directory();
// These are ordinary control/log paths, never video-frame or signal callbacks.
// Producers use a bounded, nonblocking queue; only its background writer does
// normal disk I/O. Recent breadcrumbs also have a signal-safe memory snapshot.
// Contended/full/rate-limited records may be dropped. Calls before Initialize
// are no-ops. Finish waits at most 100 ms for pending writes.
void Log(std::string_view category, std::string_view message) noexcept;
void Breadcrumb(std::string_view category, std::string_view message) noexcept;
void Finish(int exit_code) noexcept;

} // namespace hm::diagnostics
