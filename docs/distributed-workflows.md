# Running and troubleshooting distributed experiment workflows

Stitching Experiments can submit independent calibrations to a qualified local or
Slurm resource profile. HStream owns the dependency graph, artifact verification,
retries and cancellation. Slurm allocates resources and contains descendants.
Dagster optionally displays the same stage graph and history; restarting or
terminating an observation does not control native compute.

See the [design](distributed-workflows-design.md),
[implementation plan](distributed-workflows-implementation-plan.md), and
[service deployment instructions](workflow-deployment.md). Automatic calibration
and player-frame search retain the existing AMD limitations in
[AMD GPU support](amd-gpu.md). An unsupported profile must fail readiness, rather
than silently selecting another backend. The Jetson cross toolchain currently
excludes QtCore task adapters; do not advertise that deployment as a distributed
experiment target merely because its CLI cross-build succeeds.

## Graph and controls

With no resource profile selected, the existing local serial workflow remains
available. A configured profile exposes concurrency, dependencies, stage and
attempt details, logs, Stop, Retry and Export. Closing Stitching Experiments with
an active workflow asks whether to **Cancel workflow and close** or **Keep
workflow running**. Keeping it running leaves the dialog open. Confirming keeps
the monitor visible until cancellation and private runtime cleanup release its
resources; completed results stay saved. Errors and unconfirmed cleanup keep the
window open, with logs and an explicit retry available. A terminal job status
alone does not authorize closing. The desktop and browser use the same graph states
and preserve successful siblings when a different solve fails.

Unexpected application termination or browser-tab closure cannot prove that
remote jobs stopped. Durable recording ownership remains in place; reopening
the recording restores the monitor so the workflow can be cancelled and cleaned
up. Normal dialog close never releases that owner itself.

```mermaid
flowchart TD
    P[Set up experiment files - CPU] --> B[Initial calibration]
    B --> S[Find player-rich frames]
    S --> M[Extract selected frames]
    M --> A[Calibrate - 200 CP]
    M --> C[Use shared frames - CPU]
    C --> D[Calibrate - 500 CP]
    M --> E[Use shared frames - CPU]
    E --> F[Calibrate - 1500 CP]
```

Each executable stage is a graph node. A candidate can therefore span several
nodes. **Set up experiment files (CPU)** preserves the candidate's input files
and frozen settings on the controller; running it on `ripper` does not allocate
that host's GPU. **Initial calibration** finds the camera alignment and rink
mask needed to recognize players. **Find player-rich frames** searches the video
once for candidates with matching selection settings. **Extract selected frames**
writes those selected source-frame pairs to reusable PNGs; it does not run the
matcher. **Use shared frames (CPU)** copies that verified selection into each
dependent candidate. **Calibrate** runs the matcher and calibration with that
candidate's control-point budget.

Initial calibration precedes player search because the search needs its rink
mask. The extraction node is the common prerequisite for the dependent
calibrations, including experiments with different control-point budgets. Its
verified PNG bundle remains available if a following calibration fails. A failed or
unavailable prerequisite blocks descendants, while unrelated candidates
continue. Retry creates a new attempt and observation epoch, preserving verified
successful prerequisites. Retry is available after all attempts have stopped and
recording ownership has been released.

The desktop and browser show the graph from top to bottom, wrapping sibling
nodes to fit the available width. Select a node to see its named prerequisites,
dependents, allocation and attempts. The **Expand workflow** icon gives the
monitor the available window area; Restore or closing the expanded monitor
returns to the experiment controls without stopping the workflow.

New batches default to three parallel stages, from `workflow.max_parallel` in
the baseline. This is an HStream concurrency limit; Dagster only observes it.
The installed GPU pool must also allow at least three concurrent allocations,
and Slurm must have enough available resources. Dependencies still run in order.
Previously admitted batches retain their frozen limits and runtime.

When candidates already have materialized saved player frames, no new baseline,
scan or materialization node is needed. Preparation copies verified inputs into
each candidate's private workspace; it does not search the video again. The
desktop graph labels this step `Set up retained frames (CPU)`; browser candidate
preparation finishes before the distributed batch starts.
Candidates with the same selection fingerprint use the same selected frame pairs,
even when their control-point budgets differ. A running solve keeps the
`Player-rich (saved)` label while ownership still prevents conflicting actions.

For a failed stage, select its node and attempt in the workflow pane. The
reason includes scheduler status and a bounded tail of the native task log when
available; the Logs control shows the longer attempt log. A successful frame
selection remains reusable when a later calibration solve fails.

The recording has one durable operation owner. Main playback, INT8 preparation,
local experiment operations and distributed workflows respect the same owner,
including across independent desktop processes and window closure. Releasing a
GPU allocation alone does not release recording ownership before cleanup.

## Standalone configuration

`configs/baseline.yaml` owns the shipped `workflow` defaults. Set
`workflow.registry` through the normal user/game configuration to an operator
registry; `null` leaves distributed execution unconfigured. The registry must be
owned by the executing user and must not be writable by another user. Its
`config.json` has these fields:

```json
{
  "schema": 1,
  "defaults": {
    "max_parallel": 3,
    "poll_interval_ms": 1000,
    "command_timeout_seconds": 30,
    "shutdown_timeout_seconds": 30,
    "observer_timeout_seconds": 86400
  },
  "profiles": {},
  "admission_profile": "metadata",
  "allowed_game_roots": ["/srv/hockey/recordings"],
  "observer_registry": null
}
```

The empty `profiles` object is deliberately incomplete: install qualified
profiles before enabling the registry. Resolve `defaults` from the deployed
baseline, and use the exact same configuration for foreground and service
controllers. The separate baseline `workflow.metadata_resources` block supplies
CPU preparation resources to the web service; it is not part of this registry's
timing `defaults` object. `admission_profile` identifies a **local CPU** native task helper on
the controller architecture. It handles recording ownership and catalog adoption;
it need not be the executable deployed on a remote compute architecture.

Each profile has `executor` (`local` or `slurm`), `pool`, `task_command`, `runtime`,
`capabilities`, `resources`, `limits`, and `qualified_nodes`; `label` is optional.
No arbitrary scheduler arguments are accepted from a workflow. An illustrative
Slurm resource section is:

```json
{
  "gpus": 1,
  "cpus": 4,
  "memory_mb": 16384,
  "time_seconds": 1800,
  "partition": "gpu",
  "gres": "gpu:rtx5090:1",
  "nodes": ["meshy", "mini"],
  "constraint": "",
  "account": "",
  "qos": "",
  "devices": []
}
```

Choose values for the actual cluster. Each stage requests zero or one GPU. Slurm
assigns devices; native GPU stages address the allocation's logical device zero.
Local GPU profiles instead list qualified physical UUIDs in `devices`; registry
leases exclude a UUID across all local pools. Profiles sharing a pool must share
its executor, limits and device inventory. `limits` contains explicit
`concurrency` and `per_user` bounds. The workflow's own concurrency is a further
bound, not another pool allocation.

Profile IDs are configuration keys, separate from Slurm's GRES strings. IDs use
letters, digits, underscores, dots and hyphens; a colon belongs in `resources.gres`,
not the profile ID. For example:

| Profile ID | `resources.gres` | Eligible hardware |
| --- | --- | --- |
| `gpu` | `gpu:1` | Any GPU on the profile's qualified nodes |
| `gpu-rtx` (label `gpu:rtx`) | `gpu:1` | Qualified RTX4090 and RTX5090 nodes only |
| `gpu-rtx4090` | `gpu:rtx4090:1` | Qualified RTX4090 nodes |
| `nvidia-rtx5090` | `gpu:rtx5090:1` | Qualified RTX5090 nodes |
| `gpu-v100` | `gpu:v100:1` | V100 nodes, after runtime qualification |

These are operator choices, not compiled defaults. Configure them in the
registry's `config.json` and deploy the matching manifest. An untyped `gpu:1`
request still needs an explicit partition and node list: it must not select an
incompatible backend, architecture or userspace runtime. All nodes in a profile
use the same pinned runtime. If profiles overlap, give them the same `pool` and
limits so switching profiles cannot bypass the shared concurrency cap. Preserve
an existing pool name while admitted workflows still reference it.

`gpu:rtx` is the aggregate profile's display label, not a new Slurm GRES type.
Slurm's typed names match exactly: `gpu:rtx` does not select existing
`gpu:rtx4090` and `gpu:rtx5090` devices. For the aggregate, use `gpu:1` with the
explicit qualified RTX members (currently `meshy`, `mini` and `monster`) and
exclude `ripper`. Keep the typed profiles for selecting one model. A label such
as `gpu:rtx · RTX4090 / RTX5090` makes the membership visible in both UIs.
All overlapping GPU profiles must share the same pool and limits; set both
`concurrency` and `per_user` to at least three to permit a three-stage batch.

Workflow documents and installed registry catalogs have an 8 MiB bound. A
portable deployment's pinned runtime appears under every qualified node, so
adding aliases can increase the catalog substantially. Validate the complete
catalog through the installer and both UI clients before activation.

Choose a typed profile when the recording needs hardware-specific codec support.
RTX4090 qualification uses 8-bit H.264; its decoder rejected the H.264 High 10
fixture that passed on RTX5090. The generic profile does not inspect media and
route unsupported codec profiles automatically. Use `nvidia-rtx5090` for those
High 10 recordings; this limitation does not imply that all 10-bit codecs fail.

Slurm must advertise the same typed GRES in its node definition and `gres.conf`;
adding a profile alone does not configure the scheduler. Reopen Stitching
Experiments to reload installed profiles. Select **Execution profile**, set
**Parallel stages**, add candidates to the batch and start or export it. The web
dispatcher uses its own operator profile configuration and needs the same
qualified definitions; updating a standalone registry does not deploy a web
service configuration.

The current V100 on `ripper` advertises `gpu:v100:1`, but its DeepStream 9.1
`nvvideoconvert` fails the NV12-to-NVMM-RGBA path used by frame materialization.
It is excluded from the generic profile, and `gpu-v100` is not enabled. A
successful allocation or `--capabilities` response is insufficient evidence to
enable it; first qualify a compatible runtime with actual stitching stages.

`runtime` identifies `backend` (`nvidia`, `amd`, or metadata-only `cpu`),
`architecture` (`x86_64` or `aarch64`), `storage_namespace`, and an absolute-path
`files` map of SHA256 digests. Pin the task executable, exporter, CLI, baseline,
application configuration, models and supervisor deployment used by that profile.
Native GPU tasks additionally use `exporter`, `cli`, `config`, and `root` runtime
paths. Slurm also needs a pinned absolute `python` interpreter available on every
qualified target. Install the controller package at the same path on compute
nodes or use a shared installation. The supervisor itself has no Dagster or web
database dependency.

For each Slurm node, `qualified_nodes[node].runtime_identity` must equal the exact
runtime object and `qualified_nodes[node].capabilities` must include the profile's
stage kinds. Qualification is evidence from actual target checks, not a way to
bypass checks. The compute worker rechecks pinned files, host architecture and
allocated node before launching. NVIDIA automatic experiment kinds are
`experiment-baseline`, `experiment-scan`, `experiment-materialize`, and
`experiment-run`. Metadata kinds are `experiment-create`,
`experiment-reuse-selection`, and `experiment-complete`. The CPU preparation
profile needs at least create and reuse-selection. Query the installed task's
`--capabilities` output and run a disposable native pipeline on each target.

Use immutable deployment directories. Rebuilding or replacing a pinned binary
is expected to fail runtime verification; an already-admitted workflow must not
silently use the replacement. The UI freezes the active user configuration and
its native-reference provenance into the plan. Attempts read that snapshot,
so later Defaults edits do not change sibling tasks. Queued candidate workspaces
also retain their frozen experiment settings and manual matches.
Successor snapshots preserve discovered camera aliases before Auto orientation
has saved Left/Right playlists; an input-free snapshot fails during preparation.

Desktop and command-line clients may run from a different checkout than the
installed controller. Registry commands resolve the controller pinned by the
admission profile, verify its launcher and Python package against every profile,
then run that installed command. This also applies to export and monitoring;
there is no separate host or launcher setting to synchronize. Missing, changed
or conflicting controller pins stop before submission. Repair the operator
registry or restore its installed release; do not add checkout paths to frozen
profiles to bypass the check. Admitted workers retain their original supervisor.
Desktop command failures are retained in the process diagnostic log as well as
displayed in the dialog.

A Bazel-built desktop locates its checkout from the matching output tree. If a
later Bazel command removes the temporary source links, the desktop uses its own
executable-bound runfiles and source configuration links. It does not depend on
the current directory or the latest `bazel-bin` link to locate the workflow client.

## CLI and exported launchers

The package installs `hstream-workflow` and, on supported desktop targets,
`hstream-experiment-task`. From a source checkout the launcher is
`scripts/hstream-workflow`. Commands return bounded JSON suitable for a UI client.
For example, substituting actual registry, plan, workflow and stage identifiers:

```sh
hstream-workflow profiles --registry /srv/hockey/workflow-registry
hstream-workflow submit --registry /srv/hockey/workflow-registry --plan plan.json
hstream-workflow status --registry /srv/hockey/workflow-registry --workflow UUID
hstream-workflow log --registry /srv/hockey/workflow-registry --workflow UUID --stage STAGE
hstream-workflow events --registry /srv/hockey/workflow-registry --workflow UUID --stage STAGE
hstream-workflow cancel --registry /srv/hockey/workflow-registry --workflow UUID --epoch 0
hstream-workflow retry --registry /srv/hockey/workflow-registry --workflow UUID --epoch 0 --stage STAGE
hstream-workflow serve --registry /srv/hockey/workflow-registry
```

Read the current epoch from `status` before Stop or Retry. A control from an older
retry epoch is rejected. Cancellation intent is durable independently of the
controller's reconciliation lock, so status and Stop remain usable while the
controller checks scheduler state or hashes an artifact. Worker supervision sees
the attempt cancellation directly; queued Slurm allocations are cancelled by
reconciliation. The final bounded native catalog commit and cancellation are
serialized: a committed cancellation prevents adoption; a completed adoption is
retained if cancellation arrives afterward. Before adoption, the controller records
the exact verified attempt and outputs. If the response is lost, a read-only native
catalog query recovers the committed result, including when Stop arrives afterward.
This query does not reopen output artifacts or repeat adoption. A release helper
inherits the same control barrier, so an orphan from an earlier controller cannot
release ownership during a new retry. Cancellation needs exact shutdown and
cleanup proof, but does not need successful output files to remain available.

Export writes a sealed plan beside an executable launcher and does not submit:

```sh
hstream-workflow export --registry /srv/hockey/workflow-registry --plan plan.json --output experiment-batch.sh
./experiment-batch.sh
```

The launcher can also run within a CPU-only `sbatch` allocation. Child stages
request their own resources, including GRES. `run` acts as a foreground
controller; an independent `serve` service can continue after it exits. Repeated
submission of the exact UUID and plan reattaches without creating attempts. A
UUID cannot be reused with different inputs. A definite ownership conflict
rejects that workflow permanently until an explicit retry; it is not a delayed
submission that starts after the conflicting operation ends.

Browser exports use authenticated web admission, membership and quota checks.
They do not expose private web storage through the standalone CLI. See
HockeyMONWeb's deployment and parity documentation for its ORM-backed coordinator.

## Failure and recovery

State lives under `<recording>/_hockeymon/workflows/<UUID>/`. The registry contains
workflow pointers and shared reservations. Each attempt has its sealed request,
profile, scheduler submission intent, launch identity, native receipt, completion
proof, logs and private work directory. Keep these files for recovery; deleting a
PID, lock file or reservation is not proof that work stopped.

`recovery` means HStream cannot safely decide whether an external effect happened
or all descendants stopped. A lost `sbatch` response is reconciled by the exact
UUID job name, user, cluster and job ID through both `squeue` and `sacct`. A
conflicting comment is rejected; an absent accounting comment is supported on
clusters that do not retain comments. HStream never resubmits merely because a
queue query returned no row or failed. Slurm must use `proctrack/cgroup` and
`task/cgroup`, with working accounting. A successful scheduler exit still needs
a matching native receipt, content-verified artifacts, descendant shutdown and
runtime cleanup before catalog adoption.

Portable artifact schema 2 uses namespace, relative path, size and SHA256.
Descriptor and path checks remain local to each host; an NFS `st_dev` value is
not a portable identity. Files and their ancestors are pinned without following
symlinks during verification. The web adapter has additional qualified NFS
inode/mtime/ctime requirements for subsequent HTTP reads, documented in
[workflow deployment](workflow-deployment.md).

Inspect stage reasons, attempt IDs, scheduler IDs, resource requests and logs
before retrying. Standalone native logs retain two 8 MiB segments; `log` returns
at most the current segment's last 1 MiB. State transitions retain two 1 MiB
append-only diagnostic event segments; `events` returns at most 512 records and
can select one stage. `state.json` remains authoritative if a crash interrupts
a diagnostic append. Metadata and artifact verification also
have protocol bounds. Local supervisors require the kernel to report that no
children remain after reaping; an empty process-directory scan is not shutdown
proof because a child can fork during the scan. Python supervisors signal pinned
process descriptors and check kernel pidfd support before launching work. Python
builds without pidfd wrappers use the same libc interfaces; there is no numeric
PID signaling fallback. Native ownership checks keep
the CLI inside its own subreaper scope. Qt commands use the installed
`hstream-process-guard` and exact token-bound completion receipts, so unrelated Qt
children remain under their normal QProcess owners. Descendants that detach,
clear their environment or become unreadable still belong to the guard's kernel
scope. Native Pause asks the guard to stop its separate worker process group and
waits for acknowledgment. The guard remains responsive to parent death and
cancellation throughout. A missing local supervisor or killed guard cannot prove
that escaped children died, so ambiguous local shutdown retains ownership and capacity.
Restoring accounting, the expected deployment, storage or controller is safer
than discarding durable recovery state.

Dagster errors are reported separately and never stop native work. Its confirmed
run URL belongs to the current observation epoch. The service deployment guide
covers observer-only restart, retained historical runs, and installer rollback.

## Source ownership and checks

- `scripts/hstream_workflow/schema.py` and `graph.py`: pure plan/state contract,
  byte-synchronized into the web coordinator.
- `controller.py`, `state.py`, `local.py`, `slurm.py`, `supervisor.py`: standalone
  admission, shared reservations, execution, reconciliation and containment.
- `GameOperationOwner.*`: reciprocal durable recording ownership.
- `ExperimentTaskMain.cpp` and `ExperimentTaskRuntime.*`: typed native preparation,
  scan, materialization and solve; exact catalog adoption.
- `dagster_registry.py` and `dagster_observer.py`: sanitized, optional observation.

Run focused Python tests with `PYTHONPATH=scripts python3 -m unittest` over the
workflow graph, controller, Slurm, observer and installer test modules. The
corresponding graph/controller/Slurm Bazel targets are under `//scripts`.
`sync_stream_contract.py --check` checks both generated web controls and canonical
kernel bytes. Tests with mocked schedulers supplement actual local, Slurm,
Qt/browser and Dagster service validation; they do not establish target readiness.
