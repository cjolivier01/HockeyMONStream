# Local crash diagnostics

`hstream-ui` and `hstream-cli` each create a private diagnostic session. This
uses Abseil's native failure handler (already a dependency), not Crashpad or a
minidump service. Nothing is uploaded. Both native Linux and the WSL package use
this path; Jetson's CLI uses the same implementation.

The default root is `${XDG_STATE_HOME:-$HOME/.local/state}/hstream/diagnostics`.
`XDG_STATE_HOME` must be absolute. Set `HSTREAM_DIAGNOSTICS_DIR` to override the
root, or `HSTREAM_DIAGNOSTICS_DISABLE=1` to opt out. Startup prints the session
path. Configuration failures or unavailable storage leave the application running
without diagnostics for that process. A busy retention lock skips pruning;
the new process still receives its own report directory.

Each `run-<timestamp>-<component>-<random>` directory contains:

| File | Contents |
| --- | --- |
| `process.txt` | Component, PID, parent PID, start time, executable path and ELF build ID |
| `events.log`, `.1` | Existing Qt/UI or GLib runner messages, two files of at most 1 MiB each |
| `breadcrumbs.log`, `.1` | Actions/lifecycle transitions, two files of at most 64 KiB each |
| `crash.txt` | Empty normally; fatal signal, fault address, instruction address, recent breadcrumbs and native stack after a crash (at most 64 KiB) |
| `maps.txt` | `/proc/self/maps`, sampled at startup and refreshed on a fatal signal (at most 1 MiB) |
| `owner` | Format marker and process-lifetime lock used by retention |

The root is shared by UI and CLI. On startup, retention keeps the 24 most recent
inactive owned sessions plus the current session and all active sessions. It
never prunes an active session or a directory without the owned format marker.
Session directories are mode 0700 and report files 0600. Logs can include local
paths, media URIs and values already present in application output; inspect
reports before sharing them. The collector does not dump the environment or
arguments, or record text typed into controls.

## Determining which process failed

Look for nonempty `crash.txt` files, then read the adjacent `process.txt` and the
last breadcrumbs. Use the PID/parent PID and timestamps to correlate a CLI run
with its UI session. The UI additionally records runner start, exit code, and
whether Qt classified the exit as a crash. Multiple experiment workers have
separate CLI sessions too.

A UI crash can terminate its CLI through the existing parent-death `SIGTERM`.
That does **not** mean the CLI crashed. SIGTERM and SIGINT retain their existing
handling and do not generate crash reports. A `process-exit` breadcrumb records
an orderly return from the entry point; a missing marker alone is not proof of
a crash (SIGKILL, system shutdown, an unavailable writer, and parent death can
all omit it).

## Wedged shutdown

A GStreamer teardown can hang instead of crashing: the `GST_STATE_NULL`
transition runs synchronously in the caller, so a streaming thread that died
holding a pad lock stalls it forever and no handler runs. The CLI arms a
watchdog around those calls. When one overruns its budget the process writes
the stalled thread and a collapsed dump of every other thread's name and
`wchan` to stderr and to `events.log`, records a `shutdown-watchdog`
breadcrumb, and exits 70.

So a `shutdown-watchdog` breadcrumb immediately before `process-exit` with code
70 means the teardown was abandoned, not that it completed. Output being
finalized at the time is lost. The thread dump names where each thread was
parked, which is usually enough to tell a stuck encoder or decoder release
apart from a pad lock left behind by an earlier failure.

`HM_SHUTDOWN_WATCHDOG_SECONDS` overrides the budget; `0` disables the watchdog
entirely, which is the right setting when attaching a debugger to a hang. The
default fires about 13 s after a stop request. Both supervisors that kill the
CLI — the UI's `kPipelineTerminateGraceMs` and the calibration matrix script's
`PROCESS_GROUP_INTERRUPT_GRACE_SECONDS` — are sized to outlast that, so raising
the budget past 30 s means the process gets SIGKILLed before it can report.

## Cost and failure behavior

There are no new frame probes, video readbacks, GPU transfers, frame images,
per-frame diagnostic calls, or global optimization changes. Existing logging
hooks and selected control/UI transitions enqueue preformatted bounded records.
A fixed-capacity queue uses a try-only atomic guard: producers drop records
rather than wait on contention, a full queue, or disk writes. A single background
thread performs file writes/rotation outside that guard. Logging, errors, and
breadcrumbs have separate 100-record/second quotas; dropped-record counts are
reported when the writer can run. UI logging reuses the existing encoded console
message. The queue consumes about 1.1 MiB per process; the thread sleeps while
idle. `Finish` waits at most 100 ms for queued writes, never joins a stuck writer.

The last 32 accepted breadcrumbs also remain in a fixed memory ring. The fatal
handler attempts to copy them into `crash.txt` without waiting on an interrupted
producer, before writing the stack. This preserves a recent action even if
normal disk logging is behind or blocked. It records a minimal fault marker
before Abseil writes stderr, since a dead parent or full stderr pipe can prevent
Abseil's persistent callback from running. Watchdogs bound best-effort reporting;
a hung report can ultimately terminate as SIGABRT. Ordinary reports preserve
the original fatal signal. Fork-only helpers cannot log to the parent's session;
all diagnostic descriptors are close-on-exec.

## Native report limitations

This is a stack report, not a core dump: it does not retain heap contents,
all-thread stacks, or arbitrary variables for later inspection. Reporting is
best effort after memory corruption, stack exhaustion or filesystem failure.
An interrupted producer can prevent the emergency breadcrumb snapshot. A blocked
stderr can leave only the initial fault/breadcrumb/maps report. SIGKILL and
kernel OOM kills cannot be intercepted. Abseil's alternate signal stack applies
to the initializing thread; no worker-thread stack-overflow guarantee is made.

Keep the executable and relevant shared libraries matching the crash. Compare
its ELF build ID with `readelf -n /path/to/hstream-ui` (or `hstream-cli`). Symbols
and frame depth depend on the toolchain, vendor-library unwind information and
optimization. The current x86 Bazel toolchain already preserves frame pointers;
this change does not change application optimization flags. The pinned Abseil
has a narrow ARM64 patch in `third_party/patches/abseil_aarch64_signal_stack.patch`:
it recognizes alternate-stack bounds from the signal context, so valid
signal-to-application stack transitions are not rejected as oversized frames.
This preserves alternate-stack protection. Vendor stacks may still be shorter. `maps.txt` identifies the libraries and load addresses
for unresolved PCs; after subtracting the module's ELF load bias, use
`addr2line -Cf -e /path/to/module <relative-address>`. Source lines require the
matching debug symbols. Preserve report directories before rebuilding if a
crash needs investigation.

## Validation

`//src/libs/common:process_diagnostics_test` runs actual subprocess faults on the
main thread and a worker, aborts, normal signals, fork isolation, blocked stderr,
blocked log-writer I/O, contended retention, unwritable storage, rotation and
active-session retention. It verifies bounded producers/shutdown and emergency
breadcrumbs when the writer is stalled. `//src/apps/hstream-ui:ui_diagnostics_test`
checks button/window breadcrumbs and Qt warnings without logging typed input.
