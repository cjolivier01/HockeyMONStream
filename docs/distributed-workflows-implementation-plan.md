# Distributed workflow implementation plan

Status: accepted implementation plan. The design in
[distributed-workflows-design.md](distributed-workflows-design.md) completed two
independent reviews and follow-up checks with no remaining necessary findings.
This plan also completed its separate two-reviewer gate before implementation.

## Deliverables and sequence

Deliver coordinated ready-for-review native and web PRs, plus the required shared
baseline update in HockeyMON. Implement a usable experiment workflow end to end,
including direct execution, optional Slurm profiles, a dependency graph, diagnostics,
retry/cancel/reopen, independent controller services, Dagster observation and
installation documentation. Keep the existing scientific validators authoritative.

Changes are divided by ownership below. The shared schema and fixture set land
first; native task, controller and web integration can then proceed in parallel.
Public UI activation waits for their complete admission/completion paths. Installer
and observer work can proceed against the frozen schema. Integration and actual GPU
qualification precede PR opening; review rounds include validation and fixes.

## 1. Shared schema, defaults and conformance fixtures

Add `scripts/hstream_workflow/graph.py` as the canonical dependency kernel and
`scripts/hstream_workflow/schema.py` for strict bounded validation. Use JSON and the
standard library; no Django/Qt/Dagster imports. Synchronize these files into a
clearly marked web package, with a copy/check command and tests for exact bytes.

The plan schema includes `schema`, `workflow_id`, `kind`, `source_identity`,
`configuration_digest`, `runtime_identity`, immutable `profiles` and `stages`.
Stages contain `id`, `kind`, `label`, `candidate_id`, `dependencies`, `inputs`,
`profile_id`. Inputs refer to sealed initial inputs or a named verified predecessor
output, never a mutable latest-result lookup. Runtime state contains monotonic
revision/epoch, desired cancellation, node outcomes and append-only attempt records.
An attempt binds UUID, stage, epoch, request digest, backend/cluster/job identity,
profile digest, native receipt, diagnostic reason, timing and stopped/cleanup proof.

Kernel entry points validate a plan, calculate ready/blocked nodes, apply checked
attempt transitions, create explicit retry transitions and produce a bounded graph
snapshot. Effects (submit/signal/hash/DB/filesystem) belong to adapters. Receipts
enter the kernel only as already verified results bound to that exact attempt.
Terminal failed dependencies propagate even when no runnable node remains.

Define stable error/status identifiers and numeric/string bounds as protocol
constraints. User-tunable defaults go in `configs/baseline.yaml`, including local
execution, initial concurrency, polling and bounded operational deadlines; synchronize
HockeyMON `hmlib/config/baseline.yaml`. Site profiles, hosts, accounts, secrets and
deployments have no guessed defaults. Export workflow defaults/capabilities in the
native contract; update the identical contract generator in both repositories.

Shared fixture cases cover ordinary independent nodes, baseline/scan/materialize
fan-out, several selection groups, cached inputs, partial failure, unavailable
coverage, explicit retry epochs, cancellation and invalid plans. Both frontend
adapters must generate equivalent dependency edges and outcomes for those cases.

## 2. Durable native game admission

Add a small CPU-only `src/libs/common/GameOperationOwner.*` library with an on-disk
owner record beneath `GameStorage::StateDirectory` and a non-removable lock in the
same private namespace. Expose checked acquire, inspect, exact-token handoff,
request-cancel and release-after-shutdown operations. Record operation kind,
workflow/run UUID, host/boot identity and the appropriate local process or remote
scheduler/controller identity. Uncertain ownership is an explicit recovery result.
All updates use bounded no-follow reads, lock exclusion, atomic write/fsync and
revision/token comparison. Do not steal ownership based on an absent local PID.

Wire the same admission into native main CLI startup/cleanup, UI prelaunch and INT8
preparation, experiment submission, Clean/source/config mutation, removal/discard
and promotion. Long-running operations retain ownership through cleanup; short
mutations hold exclusion through commit. Pass a one-time exact successor token
across UI-to-runner launch without a release gap. Private stage games use their own
attempt authorization and cannot use it to mutate the original game. Read-only
inspection and diagnostics remain available.

Extend `StoredStitchingExperiment` with explicit workflow ownership/reference,
distinct from legacy PID/session ownership. Update store validation, revision CAS,
frame-count reservations, recovery, remove/discard and dialog restoration. Preserve
existing records and fail closed on unrecognized active ownership. The controller
is the only writer of the shared candidate catalog; tasks write private successors
and receipts. Reopening resolves the durable owner even after the original dialog
or application exited.

Tests use two processes/windows and both start orders for main/INT8/workflow;
simultaneous acquisition has one winner. Cover failed handoff, stale release,
controller restart, remote recovery, cancellation and deletion while cleanup waits.

Containment qualification requires a kernel-backed process boundary. Add the
small Qt-free `hstream-process-guard` binary and a shared command-permit API.
Dedicated CLIs enable subreaping before launching children. Qt main, archive,
INT8 and experiment commands use individual guards; aggregate exact completed
receipts before releasing the game. Preserve standard input/output, exit status,
pause semantics and finish consent. A killed guard cannot certify cleanup.
Test detached children that clear their environment or become nondumpable, a fork
between enumeration and retirement, unrelated Qt children, replaced receipts,
FailedToStart and forced-stop escalation. Package the guard on desktop and Jetson.
Keep native admission/adoption locks inherited by helper processes across a
controller crash; test lost acquisition response plus Stop and Retry, and actual
process-limit exhaustion before local acceptance.

## 3. Native experiment task boundary and input materialization

Extract reusable experiment execution from `WebJobMain.cpp` and
`WebExperimentFrames.cpp` into `StitchingExperimentTask.*`, depending on existing
backend/store/spec utilities rather than Highlights. Add a narrow installed
`hstream-experiment-task` entry point. The web wrapper delegates to the same task
functions and retains its existing receipt envelope and confined staging policy.

Typed task kinds are prepare, baseline, scan, materialize, solve and validate/import.
Each request binds schema, workflow/stage/attempt, expected input generations,
private root, runtime/profile digest and expected result location. Native workers
validate the full request and approved paths before launching a CLI. Result receipts
carry the same identities and explicit domain outcome. No task can infer a newer
workspace or claim success from file existence.

Add `--stitching-materialize-selected-frames` to the native CLI through the existing
calibration-only launch policy. Factor selected input extraction/publication out of
`create_control_points()` so capture-only and ordinary solve call the same code.
Stop after the complete validated PNG/JPEG bundle and materialized fingerprint are
durable, before feature model creation; leave calibration pending. Propagate this
through exact replay completion/EOS so successful capture does not request maps or
allocate an output stitch pool. Validate it is selected-input-only, requires a
pending expected generation, and cannot combine with publication/preview sinks.
Apply materialize-only policy before runtime asset discovery: explicitly suppress
`PipelineApp` matcher provisioning/binding and the configurator's matcher-model
requirement, not just matcher creation inside `create_control_points()`. A missing
matcher asset, failed download or unavailable network cannot fail frame extraction.
Test the actual CLI with unavailable matcher assets and networking disabled: the
materialize stage succeeds, while a subsequent selected matcher solve independently
reports its asset failure.

Keep NVIDIA replay/source timestamps and start-map checks unchanged. The shared
operation/schema compiles in AMD builds, but runtime capability admission rejects
automatic capture/scan/solve where the AMD CLI lacks those existing features. Record
that gap in `docs/amd-gpu.md`; do not add a software substitute.

Scan a private clone of the verified baseline, retaining its calibration/mask
identity. Separate scan, materialize and solver attempts have separate writable
successors. The materialize node succeeds independently of matcher availability;
solver variants clone/reflink or read the verified immutable bundle through existing
`PlayerFrameInputStore` validation. All original source references and snapshot
configuration are frozen before admission.

Tests cover capture without model creation, exact physical frames for positive and
negative reference times, reused bundle without capture, partial/corrupt/deleted
bundle, cancellation before publication, changed generation/start map, source drift
and solver failure preserving a successful materialization. Run existing experiment
backend/store/dialog and web-job tests after extracting shared functions.

## 4. Portable distributed receipts

Introduce a capability/version for portable workflow receipts in `WebJobSpec` and
the native task adapter. Stream SHA-256 on bounded pinned regular files; bind an
approved storage namespace, confined relative path, size and digest to the exact
task receipt. Verify host-local descriptor identity before/after reading and
final-path identity against the open pin. Keep legacy local receipt handling strict.

Update web staging/adoption/subsequent reads to distinguish portable content identity
from local pin identity. Audit operation artifact rows, workspace companions and
original-media aliases for device-number comparisons that cross hosts. The same
portable verifier serves the standalone controller. Never accept a portable receipt
solely by stripping `device` from a legacy record. Bound total bytes/file count and
perform hashing on workers, not HTTP requests.

Tests emulate different device IDs across producer/consumer, altered bytes with
same size/mtime, replacement during hashing, symlink/FIFO inputs, changed paths,
wrong attempt or generation, unsupported schema and unchanged legacy behavior.
Actual compute/dispatcher checks use different hosts sharing the same mount.

## 5. Standalone controller and Slurm/local executors

Add `scripts/hstream_workflow/{state,profiles,slurm,local,controller,cli}.py` and an
installed `hstream-workflow` launcher. Commands export/register, submit, reconcile,
status, events/log, cancel and retry operate on validated typed plans. A direct
foreground `run` uses the same state and executor. Export emits a runnable shell
launcher referencing a sealed plan; shell quoting is tested and export never starts
processing. The launcher forwards only documented arguments.

File-backed state uses a private directory, atomic durable transitions and an
exclusive controller lock. A registry allows the independent controller service to
resume all unresolved workflows. Control requests are revision/identity fenced.
Bound status/event/log reads and retained event segments; preserve attempt history.
The game owner and candidate catalog are reconciled only after verified completion
and confirmed cleanup. A controller crash cannot release either.

Resource admission uses a registry-wide lock and durable reservation ledger shared
by every service and foreground controller in that installed registry. Atomically
reserve workflow/profile-pool/user capacity and persist the attempt's submission
intent before launching an effect. Count submitting, queued, running, cancelling
and recovery attempts until exact shutdown/cleanup permits release; reconcile
ledger/state divergence after crashes before granting capacity. Profiles name the
same pool when they share resources. A per-workflow lock cannot grant global slots.
Web deployments retain their shared dispatcher/ORM reservation authority. These
application limits cover one installation; scheduler QoS/accounts enforce physical
limits across separate installations rather than claiming independent ledgers are
globally shared.

Local GPU profiles additionally enumerate qualified stable device UUIDs. Reserve
one device exclusively across all workflows in that registry, set the child-visible
CUDA/HIP mapping and translate its logical index consistently; reject conflicting
saved host-global GPU indices. Do not start more GPU stages than qualified device
leases, even if the requested concurrency is larger. With one qualified device,
local GPU tasks serialize. Slurm profiles use the scheduler allocation instead.
Test two controllers on different games, competing pools/users, crash between
reservation and submission, recovery retention, and concurrent local device mapping.

Slurm adapter uses argv arrays and `sbatch --parsable --no-requeue`; persist intent
first, use stable attempt identity, capture job/cluster ID and reconcile uncertain
acceptance through `squeue` and `sacct`. Query the exact job and attempt association
before cancellation. Distinguish unavailable accounting from proof that no job
exists. Persist queue reason/host/resource diagnostics. Ready-stage submission
requires validated prerequisite artifacts; no scheduler-only `afterok` adoption.

The task supervisor owns its process session/runtime cache and emits shutdown
evidence. Slurm cgroup containment plus terminal accounting establishes remote
shutdown; local supervision uses the existing native session-token/descendant
primitives or an equivalently tested process owner. Missing supervisor output leaves
recovery until independently proven stopped. Cancellation deadlines escalate only
within the exact owned attempt. Never delete uncertain workspaces.

Operator profiles include allowed task capabilities, immutable runtime/config
deployment, backend/architecture, partition, GRES, node/constraint, CPU/RAM/time and
global/per-user concurrency. Probe each allowed target and recheck the deployment
on compute before work. Use only allocated visible devices. A profile cannot mix
unqualified deployments. CPU metadata tasks avoid GPU requests. Native desktop
remote submission uses a configured fixed SSH host and fixed installed entry point,
with shared-path/profile preflight; never assemble untrusted shell commands.

Tests use real subprocess fixtures and a stateful fake Slurm command suite to cover
lost submission replies after acceptance, controller crash before/after receipt,
duplicate submissions, delayed/missing accounting, transient failures, queue waits,
failed siblings, resource caps, wrong job identity, cancel/complete races and retries.
Real Slurm qualification later validates containment and allocation behavior.

## 6. Web durable stage membership and fan-out

Add generic workflow/stage-attempt ORM records or an equivalently bounded additive
schema, linked to the owning experiment session and each immutable `StreamRun`.
Update `ensure_schema()` and reset fixtures without introducing SQL migrations or
raw SQL. Persist plan digest, execution epoch, dependencies, immutable request,
attempt identity, resource profile, diagnostics and desired cancellation.

Replace `experiment_sessions._next()` singleton sequencing with the shared kernel's
ready-stage set and independent reconciliation of all owned attempts. Keep existing
`operations.submit_operation()`, standalone supervisors and receipt verification.
Extend operation schemas with materialization and portable identity capabilities.
Source/create preparation may remain sequential when required by existing freeze
semantics; independent baseline/materialize/solve work must actually fan out.

Create a batch-owned effective configuration snapshot at admission, including account
defaults/overlay, project settings, selected sources, prepared-model identities and
exact native/runtime configuration. Pass this explicit snapshot through
`operations.submit_operation()`, `_queue_locked()` and staging for every child;
workflow children must not call the current `project_settings()` or current
default/model/runtime freeze path again. Revalidation checks continued availability
and integrity of the frozen identities, never substitutes a newer deployment or
model. Preserve existing behavior for unrelated non-workflow requests. Change account
defaults and deployment between stage admissions in a regression: later siblings
must retain their frozen settings or fail because that exact deployment is unavailable.

Update all admission gates using an exact session/stage/attempt capability. Only
children belonging to that admitted workflow bypass its own project-busy check;
unrelated runs/tabs cannot. Keep preview/promotion identity separate from concurrent
candidate children. Update toolbar cancellation, project deletion, cleanup, quota,
notifications, readiness and status projections to enumerate membership rather than
the old `active_run` FK. Preserve legacy inactive/active sessions with an explicit
upgrade/recovery path; do not reinterpret an in-flight old run as a new attempt.

Freeze selected resource profiles in run settings. Replace hardcoded GRES and
single-target readiness only for qualified workflow profiles, preserving unrelated
existing jobs. Existing per-user/global limits count real child allocations and
reservations, not graph-only bookkeeping. Adopt native results transactionally
after cancellation checks, and retain workflow ownership through all cleanup.

Regression tests extend existing sessions/staging/preview/workflow-contract suites
with actual simultaneous child records, reverse-order admission, two tabs, restart,
quota caps, unknown profiles, failed prerequisites, retained siblings and retry.
HTTP endpoints remain authenticated/revision checked; browser IDs cannot inject
profiles/paths/commands outside the published choices.

## 7. Qt/browser workflow controls and graph

Add `ExperimentWorkflowClient.*` and a graph/detail widget next to the existing
candidate list. The client launches only bounded controller commands asynchronously,
polls the authoritative snapshot and reconciles catalog results. Replace serial
dialog execution for submitted batches; do not create a parallel list of QProcesses
inside the old singleton state machine. Retain preview lifetime separately.

Both Qt and `stream-experiments.js` expose execution/profile, concurrency, graph,
stage details, attempt history/logs, Retry and Stop with matching native icons and
enablement. Graph nodes show prerequisites and distinct waiting/queued/running/
unavailable/blocked/recovery outcomes. Selecting nodes and reading logs works during
execution. Ready results can be inspected; preview/promotion remains exclusively
admitted. Closing requires cancellation and verified cleanup; refusing cancellation
keeps the dialog open. Reopen after an unexpected exit recovers the same workflow.
Main controls remain disabled by durable ownership.

Export batch in both interfaces as an owner-scoped runnable launcher plus sealed
plan, with explicit execution mode. Web export must use admitted profile/input IDs
and not disclose private deployment credentials or unrelated host paths; downloadable
plans are for the authorized operator/shared-storage deployment, not a portable
bundle of recordings. Document those requirements and reject unsupported export
contexts rather than fabricate a runnable export.

Web exports execute through the web service's authenticated admission API, not the
standalone file-backed controller. The export contains an opaque owner-scoped export
ID and frozen plan/revision binding; the launcher requires separately supplied
authentication, never embeds credentials. Export creation registers only a bounded
non-owning descriptor and does not reserve compute or mutate batch state. Execution
atomically rechecks ownership/revision/input availability and acquires workflow
membership, quotas and the project lease before any job can start. Stable execution
identity makes a repeated launcher request attach to the same accepted workflow;
an altered or stale export fails explicitly. Web Stop, deletion protection, logs
and reopen therefore see all exported execution. The native desktop export uses
native game admission instead. Add export→execute→Stop/delete/reopen, stale export,
duplicate execution and unauthorized-user tests, including response loss after
accepted execution. No path may run directly against service-owned private storage
outside its database ownership and cleanup authority.

Regenerate native layout/default/capability contracts and icon provenance. Extend
offscreen Qt and Chromium tests for the same plan/state fixtures, wheel protection,
button icons/enablement, stage selection/logs, detach/reopen, retry and Stop. Record
before/after screenshots externally and compare workflows against the same native
revision. A graph screenshot alone is not parity evidence.

## 8. Dagster observer and deployment tooling

Add optional observer code under `scripts/hstream_workflow/dagster_observer.py`.
An operator-owned bounded registry supplies sanitized plan/status paths. Generate
one graph with stable stage IDs per workflow epoch. Observer ops only read status,
emit diagnostic metadata and reflect terminal outcomes. Compute retry advances the
epoch and creates a new observation; observer failure/re-execution never submits or
retries compute. Preserve earlier links/history and identify observational actions
in descriptions. Reconcile run launches by workflow/epoch/observation tags and
handle late registration/reload. HStream remains usable when this adapter fails.

Add `scripts/install_workflow_services.py` with read-only `check`, existing-cluster
`integrate` and explicit `bootstrap` modes. Use a validated operator manifest and
fixed supported package versions. Separate controller, compute and submission roles.
Use argv subprocesses, protected secrets read from files, stable `/opt` code and
`/var/lib` state, private permissions, managed-file hashes/backups, atomic replacement,
config validation and post-start health checks. Unchanged installs avoid restarts.
Never overwrite unmanaged Slurm/Munge/accounting configuration or delete histories.

Bootstrap requires explicit topology, partition/GRES/device inventory, UID identity,
Munge key source and accounting configuration. Configure cgroup descendant/device
containment and working `sacct`; no accounting-free mode. Integrate preserves the
existing cluster and validates required behavior. Retain rollback configuration on
failed activation, and distinguish failure to roll back from successful recovery.
Document Ubuntu versions/package compatibility and unsupported platforms explicitly.

Install independent standalone controller and/or web `stream_worker` services
according to deployment roles. Existing Dagster-only dispatch upgrades record a
handover under the shared lock, release it, establish worker reconciliation, then
retire only the exact old dispatcher schedule. Preserve unrelated schedules and
notifications. Verify both upgrade paths by stopping Dagster while admitting ready
children and completing cancellation/cleanup. Run the installer twice against a
temporary test root and the actual repaired Dagster integration configuration.

Write `docs/distributed-workflows.md` for user operation/troubleshooting and
`docs/workflow-deployment.md` for installation, shared mounts, profiles, service
ownership, diagnostics, upgrades and rollback. Update native/web architecture maps,
existing experiment/export docs and web SaaS/parity docs. Include ripper repair
outcome and the limitation that its missing historical job definitions were not
recovered. Do not apply the old single-node installer to this cluster.

## 9. Packaging and coordinated validation

Install the task binary, controller Python package/launcher, immutable profile/schema
support and optional service tooling through native package scripts/Makefile/Bazel.
Python is an explicit offline orchestration dependency; native playback remains
independent. Check x86, Jetson and AMD packaging/capability boundaries. Keep the web
shared-kernel and contract generators byte-identical to their canonical sources.
Coordinate the baseline commit/pin so both repositories ship identical defaults.

Required validation before PRs:

- Full local `bazelisk build --config=opt --cpu=k8 //...` and relevant native
  experiment/admission/materialization/web-adapter tests.
- Jetson cross build plus `stubby` hardware checks when reachable; ARM64/SBSA build
  on a suitable host if available, never label x86 `--config=arm64` as a valid
  native ARM64 build. AMD build and explicit unsupported-task admission checks.
- Full relevant web regression suite, Python black/ruff, synchronized contract and
  icons, shared schema/kernel conformance and installer/observer tests.
- Real local and Slurm batch executions with at least two overlapping compatible
  GPU allocations, ordinary and player-rich candidates, multiple selection groups,
  cached reuse, failed sibling, retry, controller restart and confirmed cancellation.
- Actual Qt and authenticated browser workflows against the intended native
  revision, using disposable game/projects and original camera recordings.
- Independent end-to-end tester explores success, failure, interruption,
  unavailable resources, stale inputs/receipts, UI reopen and troubleshooting
  permutations; distinguish injected protocol failures from real hardware runs.

Keep logs, plan/status/receipt evidence and screenshots outside Git. Before/after
screenshots are hosted externally for PR descriptions. Validation failures are fixed
or described precisely when an external platform is unreachable; no fake success,
unsupported backend substitution or removal of preexisting parity limitations.

## 10. Review and completion

Review this plan with two new independent reviewers and repeat fixes until
no necessary findings remain. Record the design/plan findings and resolutions in a
concise review history without transient machine-specific detail in architecture docs.

After implementation validation, commit scoped changes without attribution and open
normal PRs against the user's master branches. Include concrete behavior, dependency
revisions, paired PR URLs, labeled screenshots and actual test/platform evidence.
Run two independent current-PR reviews in parallel; fix findings, rerun affected
checks, and request another pair until no necessary findings remain. The separate
independent tester reports permutation coverage and remaining limits. Summarize each round
in the PR/handoff without attribution. Do not merge or deploy unrelated production
application changes unless separately requested; the ripper Dagster repair and its
authorized workflow-service integration are part of this task.
