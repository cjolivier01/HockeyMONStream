# Set up an existing Slurm cluster

Open **Stitching Experiments → Workflow → Set up my cluster**. A registry is the
shared folder containing cluster settings and experiment history. The editor
creates its JSON files for you.

1. Enter the main server name, such as `ripper`, and **Find computers / test
   connection**. This checks SSH, Slurm, accounting and cgroup containment. It
   also finds an existing controller's registry.
2. Choose a computer and **Add selected computer**. Its queue and GPU request
   are filled in. Name the profile and choose its parallel-job limit. Advanced
   settings expose resources, installation paths, account, QoS and constraints.
   Overlapping profiles must share concurrency limits.
3. Check **Storage, software and dashboard settings**. Recordings and installed
   runtimes must be available at the same absolute paths across the cluster.
   The main server needs the native experiment task and runtime worker too.
   Install the current HStream desktop package for each machine's OS/processor
   when these are missing. A checkout or interpreter-only installation is
   insufficient. Enter an existing Dagster observation folder if wanted.
4. **Save and build registry** pins installed files, creates CPU preparation
   settings and writes an isolated test registry. **Save setup draft** can save
   incomplete choices without replacing the running configuration.
5. Choose a sample recording with players on the ice. **Test next computer** runs
   preparation, calibration, player search, extraction, reuse and completion
   through the normal native controller and Slurm worker. Each selected computer
   must pass. Tests use private copies and never promote results to the recording.
6. Finish existing experiments, stop the controller and **Apply tested settings**.
   The previous configuration is backed up. **Start controller** starts a user
   systemd service on the main server. An older administrator-owned system
   service must be stopped there explicitly; the editor displays the command.
   Enable user lingering there for operation after logout.
7. **Check installed setup**, then **Use this cluster** reloads the profile list.

Defaults live in `workflow.setup` in `configs/baseline.yaml`. The suggested
registry is `~/.local/state/hstream-workflows/registry`; keep it on durable shared
storage. After reinstalling, choose **Recover saved setup**. The desktop also
tries the saved/default folder automatically. Recovery reads older profiles and
explains which parts need replacing.
Discovery also finds registries used by managed user services, including custom
folders. If it finds several, it lists them for selection. Changing the settings
folder starts a separate draft; it cannot carry another folder's save revision.

Closing setup retains test intent and ownership. The current native stage can
finish independently; **Resume / check tests** schedules later stages when you
reopen. **Stop test** uses normal epoch-fenced cancellation and cleanup. Never
delete test folders to dismiss an ownership or cleanup error.
If the controller proves a test was rejected before admission, setup records that
result and lets you correct the draft and rebuild. Lost responses and unresolved
Slurm acceptance never count as rejection or completed cleanup.

The editor configures HStream on an existing Slurm cluster. It does not install
Slurm, GPU drivers, native packages or a Dagster server. The existing service
installer is documented in [workflow-deployment.md](workflow-deployment.md).
Dagster is optional for standalone execution and required for Studio batches.

## Browser and persistence boundaries

The browser Workflow pane links to the same operations in **Cluster setup
(administrator)**. Only site administrators on the local Studio can use them.
Ordinary members and the public portal cannot edit host paths. Tests use operator
recordings outside managed tenant storage, preserving ownership and quota rules.
After applying, select the registry's `web-profiles.json` and Dagster folder in
Studio deployment, restart web/dispatcher together and run normal readiness.
The browser adapter's contract/receipt checks remain required; a sample test does
not certify every codec or replace browser workflow qualification.

`setup.py` owns revision-checked `setup.json` drafts and read-only SSH discovery.
Setup CLI commands bypass installed-controller dispatch to repair absent/invalid
registries. `setup_execution.py` copies a content-addressed controller release,
compiles pinned profiles and retains generations under `setup-builds/`. Tests use
ordinary Controller plans, game ownership, reservations, receipts and retirement.
`setup-test-only.json` prevents normal profile selection and publication in test
registries. Activation requires every node's successful retired test, unchanged
draft/configuration revisions, unchanged runtime pins and an idle registry.
The controller holds a service lock for its lifetime so activation cannot race
startup. Older services must have a verifiably retired process on the main server;
a stale heartbeat alone never permits replacing their configuration. Interrupted
builds reuse only matching immutable generation inputs.

Qt runs the helper asynchronously. Browser GETs are cache-only; explicit admin
POSTs call the installed setup CLI. Set the local web request timeout above the
baseline setup timeout for long inventories. After response loss, resume status
before retrying. Both editors retain log scroll position and follow at the tail.
