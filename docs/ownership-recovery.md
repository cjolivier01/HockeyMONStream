# Recovering game ownership

When a game is locked, the toolbar shows **Review ownership…** and explains why
the controls are unavailable. After an interrupted local run, if the recorded
process cannot be verified, the notice directs the operator to **Force release**
once they have checked that the work has stopped. Review opens **Game is busy**.
If cleanup finishes between display and click, Review still opens the dialog.
The next Play is a separate action.

The desktop dialog reports the operation, ID, host, PID, ownership start, record
update, boot ID, process start ticks, workflow directory and cancellation intent.
On the recorded local host it checks the boot and `/proc` process identity and
shows the executable if the PID still matches. It does not SSH to remote hosts.
Unknown facts stay unknown; timestamps are not heartbeats and a process state
is not a progress measurement. Paused playback, quiet logs, missing PIDs and
unreachable hosts do not automatically release ownership.

- **Refresh** rereads the record and available local evidence.
- **Request stop** uses this window's ordinary Stop when it supervises the run,
  or records cancellation for a newer pipeline owner. INT8 and experiments
  use Stop in their owning preparation/workflow dialog. NVIDIA and AMD CLI
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
Delayed child admission and later helper permits check the durable authority.
Cancellation cannot mark a partial run successfully encoded. Managed receipts
retain the producer token so a separate finalizer cannot resume a revoked run.
Publication and destructive completion cleanup hold admission authority; long
remux/copy work releases it and checks again before committing. Legacy desktop
archive and telemetry finalizers follow the same rule. If authority was revoked
or cancellation requested, they leave named working files in place.
Ordinary completion still requires exact process-guard/kernel proof; recovery is
a separate operator action. Recovery does not delete or publish existing logs,
configuration or named working outputs. Active AMD encodes use unnamed temporary
files: cancellation closes and discards those incomplete encodes; force release
cannot retain them. Releasing does not automatically start Play.

New owner records use schema 2, adding `started_unix_ms` and
`process_start_ticks`. Schema 1 remains readable, with unknown start identity.
Older readers reject schema 2; update cooperating binaries together.

The browser has the same Review ownership entry, recorded details, Refresh and Request stop.
Its supervisor stores host, PID, boot, process start and start time where known;
HTTP reads do not probe hosts or infer liveness. Stop targets the displayed run
or exact experiment-session revision and preserves backend admission until
verified cleanup. Browser force release is intentionally outside this change:
Slurm capacity, session ownership and late publication remain supervised.
