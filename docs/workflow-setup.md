# Set up an existing Slurm cluster

Open **Stitching Experiments → Workflow → Set up my cluster**. A registry is the
shared folder containing cluster settings and experiment history. The editor
creates its JSON files for you.
The **Next** line explains what to do now. Buttons stay disabled until their
prerequisites are known, with a tooltip explaining why. Recovered builds need
**Resume / check tests** before continuing; changing tested choices disables
Apply until the current choices are built and tested. Recovery and Stop remain
available for unfinished or uncertain tests. These hints do not replace the
controller's admission, ownership or cleanup checks.

1. Enter the main server name, such as `ripper`, and **Find computers / test
   connection**. This checks SSH, Slurm, accounting and cgroup containment. It
   also finds an existing controller's registry.
   When it finds installed HStream programs, **Use installed HStream for main
   server** copies that folder into the draft. This changes only the main-server
   installation. Build also discovers the installed programs on each computer.
2. Choose a computer and **Add selected computer**. Its queue and GPU request
   are filled in. Name the profile and choose its parallel-job limit. Advanced
   settings expose resources, installation paths, account, QoS and constraints.
   Overlapping profiles share the lowest of their existing concurrency limits.
3. Check **Storage, software and dashboard settings**. Recordings and the settings
   folder must be available at the same absolute paths across the cluster.
   The main server needs the native experiment task and runtime worker too.
   Install the current HStream desktop package for each machine's OS/processor
   when these are missing. A checkout or interpreter-only installation is
   insufficient. Enter an existing Dagster observation folder if wanted.
4. **Save and build registry** finds installed HStream on every selected computer,
   repairs obsolete installation paths and prepares immutable shared copies.
   Computers with different OS builds are grouped automatically. It creates CPU
   preparation settings and writes an isolated test registry. **Save setup draft** can save
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
`make deploy` installs the native programs (the desktop package uses
`/opt/hstream/bin`). After deployment, **Save and build registry** repairs stale
preview-release paths automatically. It copies each distinct installed package
into `setup-runtimes/` under the registry, verifies its files and reuses matching
copies. Package-owned files are copied; local backups are excluded. Custom usable
installations remain selectable in Advanced profile settings. Missing programs
still require installing HStream on that computer.

Mixed OS or processor builds become separate execution profiles with readable
computer names. Their job limits remain shared across the original overlapping
profiles, so splitting a profile does not increase cluster capacity. The editor
retains the original editable profiles and displays any lowered overlapping limits.
Tasks select the prepared libraries and baseline before their native program
starts, including packages whose ELF search path names the deployment prefix.
Published copies stay in place for existing runs. Discovery and build never mark
computers qualified: each one still needs its native sample test.
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
compiles pinned profiles and retains generations under `setup-builds/`.
`setup_runtime.py` discovers and snapshots node installations over SSH using the
shared controller and a shared-file challenge. Preparation completes and the new
generation validates before revision-checked draft repairs are committed. Tests use
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
