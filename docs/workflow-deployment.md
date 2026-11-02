# Workflow services, Slurm and Dagster

The HStream controller owns native task admission, dependencies, retries,
cancellation and completion. Slurm owns GPU allocations and descendants. Dagster
is an optional **read-only observation** of that workflow. Stopping, retrying or
re-executing a Dagster observation does not stop or retry compute. Use HStream's
workflow controls for those actions.

The standalone installation runs `hstream-workflow serve`. A web installation
runs the existing Django `stream_worker` instead; it retains database admission,
quota and project ownership. A browser export uses authenticated web admission
and must never launch the standalone controller against web-owned private files.
Dagster is not the supervisor of either controller.

## Supported installation modes

`scripts/install_workflow_services.py` takes a JSON operator manifest:

```bash
python3 scripts/install_workflow_services.py check --manifest /etc/hstream-workflow/site.json
sudo python3 scripts/install_workflow_services.py integrate --manifest /etc/hstream-workflow/site.json
sudo python3 scripts/install_workflow_services.py bootstrap --manifest /etc/hstream-workflow/site.json
```

- `check` reads controller/accounting/cgroup configuration, shared directories,
  service state, Dagster versions, loaded code locations and daemon heartbeats. It
  does not submit a GPU job, restart services or write readiness state. Actual
  cross-host GPU/runtime qualification remains a separate required deployment test.
- `integrate` preserves an existing Slurm cluster, Munge key, accounts and
  accounting history. It installs managed workflow/Dagster configuration and
  services. It never calls the old single-node Slurm installer.
- `bootstrap` installs explicitly pinned distribution packages and explicitly
  supplied Slurm, GRES, cgroup, Munge and accounting files on a new host. It refuses
  existing unmanaged Slurm configuration, including files listed for adoption.
  Bootstrap accepts Ubuntu 22.04 and 24.04 with systemd/cgroup v2.
  Other distributions and Windows host services are not supported. WSL may submit
  to an existing qualified Linux cluster; it is not a supported cluster bootstrap.

The qualified optional Dagster version is **1.13.26**, for both `dagster` and
`dagster-webserver`. The installer never upgrades a live venv in place. Use a new,
versioned venv path when changing the supported version. Existing matching venvs
are reused. Python is an offline orchestration dependency; native playback does
not import Dagster or Python.

## Prerequisites and identities

Provision an unprivileged service account with the manifest's exact numeric UID
and GID. Shared-storage ownership must match on submission, compute, web and
dispatcher hosts. Provision matching Slurm/Munge identities before bootstrap;
do not renumber live service accounts or rotate Munge material as an install step.

Mount the recording, private state and immutable runtime paths at the same absolute
locations on every eligible host. Cross-host filesystem locking must work. Test
that one host's exclusive workflow/admission lock prevents acquisition on another
host. Local PID absence is not proof that a remote task stopped.

Native distributed artifact receipts verify a storage namespace, confined relative
path, size and content digest, with full host-local file pin checks. The current
web read path additionally requires matching inode, size, mtime and ctime across
web/dispatcher mounts; device numbers may differ. Qualify this mapping before
enabling that deployment. A mount translating inode identities is unsupported
until it has an independent CPU read verifier; equal paths alone are insufficient.

Every Slurm target must provide the pinned native task executable, configuration,
models and a qualified native runtime. In a profile, `runtime.python` names the
compute host's Python executable; never reuse the dispatcher's x86 interpreter
path for a different architecture. Pin the resolved interpreter binary and native
files in `runtime.files`. Install the same workflow supervisor code and
`scripts/hstream-workflow` launcher at the shared/identical compute path. Profile
readiness must enumerate every allowed target, including backend and architecture.
Current automatic calibration/player scanning requires the qualified NVIDIA
runtime; an AMD allocation does not imply support for those existing CLI gaps.

Slurm must expose working `sacct`, `accounting_storage/slurmdbd`,
`proctrack/cgroup`, `task/cgroup` and device containment. CPU-only preparation does
not request GPUs. Slurm profile GRES/node/partition choices must not span
unqualified runtime families. Use scheduler QoS/accounts for cluster-wide limits;
separate HStream registries do not share one application admission ledger.

## Manifest and files

All deployment choices are explicit operator inputs. Application polling/deadline
defaults come from the installed `configs/baseline.yaml`, not an installer default
set. The manifest contains file references with SHA-256 digests, never inline
passwords or a Munge key. Readable examples below use illustrative paths; replace
them with the qualified installation's paths and identities.

```json
{
  "schema": 1,
  "id": "studio-workflows",
  "health_timeout_seconds": 300,
  "identity": {"user": "hstream", "uid": 1001, "gid": 1001},
  "state_dir": "/var/lib/hstream-workflow-install",
  "shared_paths": ["/srv/hockey"],
  "slurm": {
    "cluster": "studio",
    "roles": ["submission"],
    "config_dir": "/etc/slurm",
    "packages": {},
    "bootstrap": null
  },
  "workflow": {
    "kind": "standalone",
    "unit": "hstream-workflow-controller",
    "python": "/usr/bin/python3",
    "code_dir": "/opt/hstream/scripts",
    "registry": "/srv/hockey/workflow-registry",
    "registry_config": {"path": "/etc/hstream-workflow/registry.json", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "environment_file": null,
    "handover": null
  },
  "dagster": {
    "version": "1.13.26",
    "python": "/usr/bin/python3",
    "venv": "/opt/dagster/venv-1.13.26",
    "home": "/var/lib/dagster",
    "code_dir": "/opt/dagster/code",
    "registry": "/srv/hockey/dagster-observations",
    "registry_config": {"path": "/etc/hstream-workflow/observer.json", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "instance_config": {"path": "/etc/hstream-workflow/dagster.yaml", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "workspace_config": {"path": "/etc/hstream-workflow/workspace.yaml", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "port": 3001,
    "install_packages": true
  },
  "adopt": {}
}
```

Use `sha256sum FILE` to populate each reference. An `adopt` entry maps an exact
existing destination path to its current SHA-256. Adoption is explicit even when
bytes happen to match generated output. Later edits to a managed file stop the
installer; reconcile the operator change before reinstalling. Do not authorize
adoption by a filename glob or overwrite a service belonging to another deployment.

For a compute-only bootstrap, `workflow` and `dagster` can both be `null`. A
deployment that enables Dagster must also declare its independent workflow owner.
Native package deployment supplies `/opt/hstream/scripts/hstream_workflow`; the
installer must be colocated with that package. The observer code location receives
its own managed copy so it does not depend on a development worktree.

The standalone registry configuration has `schema`, `defaults`, `profiles`,
`admission_profile`, `allowed_game_roots` and `observer_registry`. Resolve defaults
from the baseline; define fully qualified profiles as described in
[distributed-workflows.md](distributed-workflows.md). `admission_profile` is an
explicit local CPU profile for the controller-host native admission helper. It
must not infer its executable or architecture from a remote GPU profile.

An observer registry configuration contains all of these fields:

```json
{
  "schema": 1,
  "poll_interval_ms": 1000,
  "observer_timeout_seconds": 86400,
  "max_observations": 64,
  "public_url": "https://workflows.example.org",
  "max_concurrent_steps": 4
}
```

Polling/timeout values above are examples, to be resolved from the shipped
baseline. Definition and process limits are explicit operator capacity choices;
protocol bounds allow at most 128 active observations and 32 observer steps per
run. Dagster's instance run concurrency is also explicit in `dagster.yaml`.

Preserve the existing instance storage configuration when preparing `dagster.yaml`.
Do not replace a PostgreSQL instance with SQLite or point at a new empty history
directory. Prepare a merged workspace retaining unrelated locations and adding:

```yaml
load_from:
  - python_file:
      relative_path: workflow_definitions.py
      location_name: hstream_workflows
      executable_path: /opt/dagster/venv-1.13.26/bin/python
```

The installer leaves an existing `definitions.py` intact and writes only
`workflow_definitions.py` plus its package. Include existing code locations in the
merged workspace, particularly during a legacy dispatcher schedule handover.
Definitions and instance configuration are validated before service activation.

Dagster listens on loopback only. Use an existing authenticated reverse proxy or
SSH tunnel for operator access. The installer does not expose an unauthenticated
dashboard publicly or embed it in customer pages. No credentials enter native
plans or observation snapshots. Observation snapshots contain stage labels,
dependencies, states and restricted attempt/scheduler identifiers; input paths,
results, settings, command arguments and free-form errors stay in HStream.

## New cluster bootstrap

Add exact distribution package versions to `slurm.packages`. Include the required
Slurm client/controller/compute/accounting packages, Munge and Python venv support
for the selected host roles. Use matching Slurm versions throughout the cluster.
The installer uses the configured package repositories; it does not guess a
package version or replace repository signing policy.

Set `slurm.bootstrap` to:

```json
{
  "files": {
    "slurm.conf": {"path": "/root/cluster/slurm.conf", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "cgroup.conf": {"path": "/root/cluster/cgroup.conf", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "gres.conf": {"path": "/root/cluster/gres-gpu01.conf", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
    "slurmdbd.conf": {"path": "/root/cluster/slurmdbd.conf", "sha256": "REPLACE_WITH_64_HEX_DIGEST"}
  },
  "munge_key": {"path": "/root/cluster/munge.key", "sha256": "REPLACE_WITH_64_HEX_DIGEST"},
  "identities": {"slurm": {"uid": 64030, "gid": 64030}, "munge": {"uid": 64031, "gid": 64031}}
}
```

`slurm.conf` must declare the actual cluster/controller, complete node CPU/RAM/GRES
inventory, partitions, state/spool directories and accounting host. Each compute
host needs its actual local device mapping in `gres.conf`. Require
`SelectType=select/cons_tres`, `ProctrackType=proctrack/cgroup`,
`TaskPlugin=task/cgroup` and `AccountingStorageType=accounting_storage/slurmdbd`.
`cgroup.conf` must constrain devices, cores and RAM. `slurmd -G` validates compute
GRES configuration before activation.

Controller bootstrap requires a preprovisioned accounting database and protected
`slurmdbd.conf` declaring its storage host/database/user/password. Both that input
and the Munge key must be owner-only regular files. They are never printed or
passed as command-line secret values. Deploy the same Munge key through a protected
channel to every cluster member. Bootstrap does not initialize an external database,
guess grant policies, configure firewalls or rotate existing credentials. Configure
network rules and time synchronization before admitting work.

## Web worker and legacy Dagster dispatcher handover

For web deployments, set `workflow.kind` to `web`, `code_dir` to the installed
directory containing `manage.py`, `python` to its venv Python, `registry` to the
existing stream data root, `registry_config` to `null`, and `environment_file` to
the existing protected service environment. Preserve its database and mount values.
Use the existing worker unit name when taking over an existing deployment.
With Dagster enabled, the unit also sets `PYTHONPATH` to the managed observer code
directory and `HSTREAM_DAGSTER_REGISTRY` to its sanitized registry. If the existing
environment file defines either variable, systemd gives that file precedence;
keep those explicit values aligned with this deployment.

When Dagster previously called `stream_dispatcher`, supply `workflow.handover`:

```json
{
  "lock": "/srv/hockey/stream/dispatcher.lock",
  "receipt": "/srv/hockey/stream/dispatcher-handover.json",
  "location": "stream",
  "repository": "__repository__",
  "schedule": "stream_dispatcher_schedule",
  "database_identity": "REPLACE_WITH_64_HEX_DIGEST"
}
```

The database identity is SHA-256 of sorted JSON for the string values of Django's
default `ENGINE`, `NAME`, `HOST` and `PORT`; it contains no database password.
Use the exact configured location/repository/schedule, rather than matching every
schedule whose name contains “stream”.

The installer records a token-bound handover intent under the existing dispatcher
lock, releases it, then starts the independent worker. The worker emits a receipt
after a completed reconciliation pass. The installer verifies the exact data root,
database identity and current worker PID before stopping only that legacy dispatcher
schedule. Unrelated jobs, notifications and history remain intact. If the schedule
stop response is lost, the worker stays running and a rerun reconciles the pending
handover. Never stop that worker as a workaround for an uncertain old-schedule stop.

The shared dispatcher lock protects the handover overlap. Never wait for the worker
while holding that lock. Test both an existing standalone worker and a Dagster-only
deployment: stop Dagster afterward and confirm queued child admission, cancellation
and cleanup still progress through the worker.

## Idempotency, rollback and recovery

Changed files are backed up below
`<state_dir>/<deployment-id>/backups/<transaction-id>/`; the managed-file catalog
records installed hashes. Updates use private temporary files, fsync and atomic
replacement. A durable activation journal records prior file bytes/metadata and
service active/enabled state. A later invocation recovers an interrupted activation
before applying another update. Unexpected external edits stop rollback rather than
silently overwriting them. A rollback failure is reported separately and preserves
the protected backup for repair.

Configuration failures restore prior files and service activity. Database contents,
Slurm accounting, Munge identities and workflow/Dagster history are never deleted.
Package installation is not undone by purging packages; a newly installed unused
venv may remain after a failed activation. Unchanged installs perform health checks
without restarting services. Files named in a changed manifest remain operator
responsibility; the installer does not garbage-collect old deployments or histories.

Inspect failed activation evidence before rerunning. Do not clear native recovery,
delete a workspace or release a game/project owner merely because a controller
service restarted. Scheduler accounting and native shutdown/receipt validation
remain authoritative.

## Observation operation and troubleshooting

The native controller publishes sanitized graph/status files. The independent
`hstream-dagster-observer` registrar reloads the code location and launches one
observation per execution epoch. Its deterministic observation UUID binds the
server-assigned Dagster run ID through exact tags. A lost launch response is
reconciled using observation, workflow, plan-digest and epoch tags. If acceptance
cannot be established, the registrar retains recovery rather than submitting again;
it never creates a replacement compute attempt.

Native Retry advances the execution epoch and creates another observation, retaining
earlier links. Re-execution in Dagster observes the same native state. A separately
registered observer restart in the same epoch follows subsequent snapshots. Late
attachment renders completed/reused stages without re-running them. Failed or
unavailable prerequisites skip their dependent Dagster steps while independent
branches remain observable. An observer timeout/failure is a diagnostics failure,
not evidence that native work stopped.

The registry bounds active definitions. After both native work and its observer
have ended, retire only the definition:

```bash
python3 -m hstream_workflow.dagster_registry retire \
  --registry /srv/hockey/dagster-observations --observation-id OBSERVATION_UUID
```

Historical Dagster runs remain. Registry reads and definitions are bounded; old
retired registration files can be archived by the operator after retaining the
needed diagnostics. Do not remove an active registration or its snapshot.

Inspect `journalctl -u hstream-workflow-controller`, or the configured web worker,
first for native lifecycle problems. Then use `squeue`/`sacct` for the exact attempt's
Slurm job and `journalctl -u hstream-dagster-observer -u dagster-daemon
-u dagster-webserver` for observation problems. A healthy HTTP landing page alone
does not establish healthy code locations, daemon heartbeats or compute readiness.

Ripper's November 2026 repair preserved its historical runs and replaced the missing
`/home/colivier/dagster` workspace with `/opt/dagster/code`. Its original example job
definitions could not be recovered; historical runs remain inspectable, and an
explicit `infrastructure_health` job verified queued daemon execution. Preserve that
location in the merged workspace during integration. The older
`HockeyMON/scripts/slurm_gpu_installer.py` overwrites a single-node cluster and
disables accounting; it is not a migration path for this installation.

## Configuring the Dagster address

The registrar's `--url` is its GraphQL/control endpoint. The installer runs the
registrar beside Dagster and derives that loopback endpoint from the manifest's
`dagster.port`. A separately managed registrar can use any explicit reachable
HTTP(S) endpoint. No hostname such as `ripper` is compiled into HStream.

Set `public_url` in the observer registry's `config.json` to the address reachable
from desktop and browser clients. It may differ from the control endpoint, for
example `https://workflows.example.org` behind a reverse proxy. Use an explicit
`null` to omit external links. Confirmed observation records combine that URL
with the actual Dagster run ID; Qt and the browser consume those records.
`HSTREAM_DAGSTER_URL` can override the web-visible origin when a browser deployment
requires a different route. None of these service addresses are per-job processing
controls. Changing the operator configuration does not require recompiling the UI.
