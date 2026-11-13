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
There are no implicit processing defaults in the worker.

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

Operations:

- `launch`: unique 32-hex task ID, absolute pinned executable argv, confined cwd,
  stdout/stderr paths, environment overrides and explicit timeout.
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
semantics remain available to native resource discovery. Forked guards close
other tasks' pipe ends and the outer lock. Guard completion requires root status
and kernel `ECHILD`, so new sessions and detached descendants do not escape
ownership. Missing task evidence triggers outer containment and fails the session.
Task deadline expiry remains distinguishable from ordinary process exit.

`state.json` includes host, boot, PID/start identity, architecture, allocated node,
tasks and optional leased port. `result.json` binds terminal success/error and
retirement proof to the admitted session. A scheduler's state alone cannot
authorize artifact adoption; controllers must verify their native/application
receipts after descendant retirement. Loss of proof must retain reservations.

HockeyMONWeb uses the protocol from a service-side coordinator. The worker knows
no Django models, browser commands or camera concepts. It sends no video bytes
through these control documents: native WebRTC owns streaming. Other controllers
can use the same API. The existing standalone `hstream-workflow` Python supervisor
is unchanged and still requires its legacy interpreter profile.

Run `bazelisk test --config=opt //src/libs/runtime_worker:runtime_worker_test`
for the Linux process/protocol integration tests. These tests use CPU helpers;
GPU playback qualification still belongs to the Slurm deployment checks.
