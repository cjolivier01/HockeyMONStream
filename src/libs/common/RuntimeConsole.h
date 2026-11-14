#pragma once

namespace hm {

// GLib stdout handler: keep HSTREAM_ records on complete lines when SDK
// diagnostics share stdout/stderr. Ordinary fragmented output is unchanged.
void WriteRuntimeConsole(const char* message);

} // namespace hm
