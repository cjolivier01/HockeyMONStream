# Generic runtime worker

`hstream-runtime-worker` is a C++17 execution owner for private shared-storage
jobs. Build it with `make hstream-runtime-worker`; the desktop and Jetson Debian
packages install it under `/opt/hstream/bin` and expose a `/usr/bin` symlink.
It has no Qt, GPU or web-application dependency. Its native children retain their
normal runtime dependencies and allocated GPU environment.

`--capabilities` reports protocol `hstream-runtime-worker-v1` and supported
operations. `--request FILE` consumes an explicit session request. The request
specifies `schema`, opaque `id`, a random hex `token`, an absolute `workspace`,
runtime `files` mapped to SHA-256 hashes, and required `limits`: `poll_ms`,
`retire_ms`, `timeout_ms`, `client_timeout_ms`, `max_bytes`, `max_log_bytes` and
`max_tasks`. Optional `architecture` and `nodes` constrain allocation identity.
Optional `tcp_port` supplies an owned `lease_root` and a half-open port range.
Optional `read_files` grants bounded inspection of exact external files, for
example a controller's explicit compute readiness check. It never grants execute
permission; launch still requires the pinned `files` map.
Optional `tools` lists explicit utility aliases to resolve from the allocated
host's absolute PATH entries. The worker records their canonical paths in state,
pins their bytes before becoming ready, and checks those bytes again at launch.
An existing qualified file pin must agree. This supports compute-only native
dependencies without requiring the controller to install them.
There are no implicit processing defaults in the worker.

`compute-files-v1` in `--capabilities` permits an explicit `resolve_files` list
of absolute installed paths. The worker resolves configured symlinks on its own
host, pins the canonical regular files before readiness, and reports the exact
hashes in `state.json`'s `files` map and the path aliases in `tools`. Existing
qualified hashes always win: discovery cannot replace a conflicting pin.
Controllers use this for explicit deployment qualification across different OS
builds, verify the native contract and recheck the captured bytes before adopting
the result. Subsequent sessions send the qualified `files` hashes and node and
architecture constraints; they do not rediscover or silently adopt replacements.
Artifact inspection still rejects leaf symlinks. Executables still run through
their verified descriptors and are checked again before each launch.

The request lives one directory below the workspace. That directory must be
private to the service account. Before accepting commands, the worker validates
the runtime file hashes and allocation identity and takes an exclusive session
lock. Existing session state cannot be replayed.

Controllers atomically publish `commands/00000000000000000001.json` and increasing
sequence numbers. Commands carry the schema, token, sequence, operation and its
arguments. The same filename in `replies/` contains the correlated outcome;
controllers remove consumed replies. Requests and responses are bounded to 4 MiB.
`client.json` contains the session token and an increasing heartbeat sequence.
The `cancel` marker, a signal, expired controller lease or expired session causes
bounded descendant retirement. The heartbeat is an execution lease, not user
authentication. Controllers own user authorization and must keep these directories
inaccessible to other accounts.
Lease reads consume a stable opened snapshot across atomic replacement; token
and increasing-sequence checks still apply. Immutable protocol documents retain
strict pathname and inode checks.

Operations:

- `launch`: unique 32-hex task ID, absolute pinned executable argv, confined cwd,
  stdout/stderr paths, environment overrides and explicit timeout. An explicitly
  admitted compute utility alias may replace the absolute executable name.
- `stdin`, `stdin-close`: bounded base64 input and end-of-input. `accepted` is the
  number of bytes queued, not a native application acknowledgment.
- `signal`, `poll`, `retire-task`: task control and exact retirement state.
- `retire`, `finish`: retire all tasks; Finish also closes the session.
- `forget`: discard a completed task handle; its consumed permit remains.
- `inspect`: hash a confined regular file and compare optional expected identity
  fields on the producing host. Pinned runtime files and exact `read_files` grants
  are also eligible. It returns both content and host-local stat data.
- `read`: the same identity check plus at most 1 MiB of base64 content, read through
  the hashing descriptor. Used for readiness configuration, never preview media.
- `http`: bounded GET/POST to an explicit loopback port/path. Proxy settings and
  redirects cannot forward requests off-node.

Each native task has a dedicated `ProcessGuard` subreaper. The executable is
hashed through a pinned descriptor and executed with `fexecve`; original argv
semantics remain available to native resource discovery. The execution child
keeps that descriptor available for shebang interpreters. Forked guards close
other tasks' pipe ends and the outer lock. Guard completion requires root status
and kernel `ECHILD`, so new sessions and detached descendants do not escape
ownership. Missing task evidence triggers outer containment and fails the session.
Task deadline expiry remains distinguishable from ordinary process exit.
Hashing, loopback HTTP and storage accounting continue task supervision. Storage
traversal pins directory descriptors and tolerates concurrent native cleanup.

`state.json` includes host, boot, PID/start identity, architecture, allocated node,
tasks and optional leased port. `result.json` binds terminal success/error and
retirement proof to the admitted session. A scheduler's state alone cannot
authorize artifact adoption; controllers must verify their native/application
receipts after descendant retirement. Loss of proof must retain reservations.

HockeyMONWeb uses the protocol from a service-side coordinator. The worker knows
no Django models, browser commands or camera concepts. It sends no video bytes
through these control documents: native WebRTC owns streaming. Standalone `hstream-workflow` uses the same worker in batch mode. Its Python
controller remains on the service/controller host; compute needs only the native
package and the task's own dependencies.

Optional `batch` admits exactly one task using the ordinary launch `spec` and a
bounded `artifacts` list of `{path, maximum}` declarations. Set `max_tasks` to one
and `client_timeout_ms` to zero: the frozen task runs to completion without a
live controller. Mailbox commands are disabled in this mode. The worker closes
stdin, supervises the same process guards, and hashes declared result files only
after all descendants retire successfully. Task errors, deadlines and cancellation
never publish successful artifact receipts. `--capabilities` advertises
`batch: native-batch-v1`; qualify this version before enabling standalone batches.

Optional `cancel_files` declares exact paths and scalar `match` fields. A matching
document cancels execution; a stale epoch does not. Paths outside the workspace
require an explicit `read_files` grant. Checks continue during startup hashing,
execution and artifact hashing. `cancelled` in the result distinguishes this from
other failures. Every state/result binds `request_sha256` to the exact request
bytes consumed at startup, in addition to the session ID and token.

A launch spec can set `rotate_log: true` with a single combined stdout/stderr path.
The native owner drains a pipe into the current log and one previous `.1` segment,
each bounded by `max_log_bytes`. The child never opens those log paths. Capture
continues during retirement and must reach EOF before completion. Explicit
`unset_environment` names clear inherited settings before applying frozen overrides.

Run `bazelisk test --config=opt //src/libs/runtime_worker:runtime_worker_test`
for the Linux process/protocol integration tests. These tests use CPU helpers;
GPU playback qualification still belongs to the Slurm deployment checks.

Standalone admission validates the worker's translated time limits before
reserving a task: shutdown allowance is at most 600 seconds, and task time plus
both retirement phases and the command allowance must fit within 30 days.
The shared process guard passes absolute phase deadlines through repeated reap
and process-signaling passes. Interrupted waits and continuously exiting children
cannot starve cancellation; an expired proof pass makes one nonretrying kernel
observation and never infers shutdown from an empty process scan.
