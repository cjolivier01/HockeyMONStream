# Recovering game ownership

Play remains available while an operation owns the selected game. Clicking it
opens **Game is busy**, without starting another pipeline. The main toolbar has
no additional controls. All other conflicting mutations remain disabled.

The desktop dialog reports the operation, ID, host, PID, ownership start, record
update, boot ID, process start ticks, workflow directory and cancellation intent.
On the recorded local host it checks the boot and `/proc` process identity and
shows the executable if the PID still matches. It does not SSH to remote hosts.
Unknown facts stay unknown; timestamps are not heartbeats and a process state
is not a progress measurement. Paused playback, quiet logs, missing PIDs and
unreachable hosts do not automatically release ownership.

- **Refresh** rereads the record and available local evidence.
- **Request stop** uses this window's ordinary Stop when it supervises the run,
  or records cancellation for a newer non-workflow owner. NVIDIA and AMD CLI
  control loops observe cancellation; a paused, hung or legacy process may not.
  The claim remains until the supervisor proves descendant shutdown and cleanup.
  Distributed workflows retain their existing monitor cancellation flow.
- **Release verified owner** is available after a verified reboot of the same
  local host, only for a local operation with no distributed workflow directory.
- **Force release** explicitly abandons the displayed owner without asserting
  shutdown. A separate confirmation defaults to Cancel. A surviving process,
  especially an older binary, can still conflict with later work. Stop first
  where possible. This window cannot force-release a run it still supervises;
  use its Stop and wait for cleanup, or reopen the game in a fresh window for
  explicit recovery if that supervisor cannot complete.

Both release actions compare the exact displayed token and revision under the
admission lock. A changed owner requires a fresh inspection. They first persist
`_hockeymon/game-operation-revoked-<token>.yaml`, including the original owner,
reason, actor and timestamp, then unlink the active claim. An interrupted release
can retry against that same audit. Audit failures retain the claim. Revoked
tokens cannot authorize mutations, claim again, or hand off to a successor.
Ordinary completion still requires exact process-guard/kernel proof; recovery is
a separate operator action. Logs, configuration and unfinished outputs are never
deleted or published by recovery. Releasing does not automatically start Play.

New owner records use schema 2, adding `started_unix_ms` and
`process_start_ticks`. Schema 1 remains readable, with unknown start identity.
Older readers reject schema 2; update cooperating binaries together.

The browser has the same Play entry, recorded details, Refresh and Request stop.
Its supervisor stores host, PID, boot, process start and start time where known;
HTTP reads do not probe hosts or infer liveness. Stop targets the displayed run
or exact experiment-session revision and preserves backend admission until
verified cleanup. Browser force release is intentionally outside this change:
Slurm capacity, session ownership and late publication remain supervised.
