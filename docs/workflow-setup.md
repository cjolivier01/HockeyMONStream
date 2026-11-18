# Set up an existing Slurm cluster

Open **Stitching Experiments → Workflow → Set up my cluster**. Setup shows one
screen at a time with Back and one main action. The desktop uses the recording
already open in the experiment dialog. Advanced settings stay collapsed.

1. **Connect.** Enter the main server name, such as `ripper`, and choose
   **Find my computers**. This checks the connection and the existing scheduler.
   Failed checks stay on this screen with their explanation. An existing saved
   setup is recovered automatically; **Continue saved setup** can retry recovery.
2. **Computers.** Tick the computers to use. Available and busy computers are
   labeled in plain language. **Search again** refreshes discovery. Continue saves
   the choices; advanced job settings retain editable profiles and resource limits.
3. **Prepare.** Confirm the folder holding the game folders and choose
   **Prepare computers**. Setup discovers installed programs, verifies shared
   settings storage and repairs obsolete installation paths. Jobs use each computer’s
   deployed HStream installation directly. It does not install missing HStream packages.
   Activity names the deployment folder checked on each computer. No installation
   is copied to shared storage.
4. **Test.** Choose **Test my computers** to run a short native GPU and shared-storage
   check on each selected computer. The open recording supplies the shared folder;
   no video is read, scanned or calibrated. Successful computers are
   retained. The screen shows the current computer, GPU choice, check number,
   completed stages and remaining checks. Equivalent GPU choices on the same computer
   share one check. Different runtimes or resource requirements remain separate.
   Free computers run in parallel within the configured global and profile limits.
   Each check owns a private empty folder, so checking one computer does not lock
   the recording or prevent another computer from being checked. A queued stage says it is waiting for Slurm and shows
   the scheduler reason; it does not claim to be running. Test details include the
   current native stage log. Resume stays disabled while automatic checking runs,
   and Stop remains available across polls. Stopping and continuing skips passed
   checks for the current prepared setup; a failure stops the sequence and shows
   the failed stage, its cause and what to do next, including after reopening setup.
   Recovered diagnostics from successful stages are not reported as the failure.
   A GPU occupied by another Slurm job does not block setup. An exactly identified
   pending allocation waiting for resources or priority is cancelled through the
   ordinary supervised path and shown as **Busy — GPU check deferred**. It is
   never marked passed. Preparation must already have verified the supported
   NVIDIA runtime and shared storage. Unknown submission, unsupported runtime,
   a probe failure, or unproved cleanup still blocks enabling. A cancellation
   that races with execution is not accepted as a deferred check.
   Cleanup stays explicit until the controller proves the test stopped. The test
   writes only private readiness receipts and does not promote results to the recording. Stop testing
   cancels the current test and prevents later computers from starting, including
   when a status response is still pending. Unknown submission results must be
   recovered before another test can start. Closing retains the current native
   stage; later stages wait until setup is reopened and testing is continued.
   The original recording is recovered from the test plan if the field is empty.
   Resuming an unfinished test from an unchecked setup stops that old
   qualification through normal cancellation before scheduling any more stages.
   Cleanup must finish before preparing again. Read-only status does not request
   cancellation, and completed test history remains intact.
5. **Finish.** Passed checks and safely deferred busy GPU checks allow
   **Enable these computers**. It checks for existing work, stops the
   managed service when needed, applies tested settings, starts the service and
   verifies readiness in order. Failure stops this sequence. Existing service
   locks, native qualification, ownership and revision checks remain authoritative.
   **Use these computers** returns to Stitching Experiments with the tested setup.

An older administrator-owned system service cannot be stopped by this wizard.
Its refusal and the exact administrative action remain visible. The wizard does
not force-release ownership or treat a connection check as qualification.
The technical log and individual build/test/service actions remain available
under collapsed details for diagnosis. Editing tested choices requires preparation
and testing again; recovery resumes the saved step without submitting a new test.

Defaults live in `workflow.setup` in `configs/baseline.yaml`. The suggested
registry is `~/hstream-workflows/registry`; keep it on durable shared
storage. After reinstalling, choose **Continue saved setup**. The desktop also
tries the saved/default folder automatically. Recovery reads older profiles and
explains which parts need replacing.
Before building, setup tests both reads and writes from the main server and every
selected computer without relying on files already being shared. This handles
hosts such as Jetsons whose `.local` directory intentionally uses local disk.
If the old folder is not shared, setup verifies the baseline location, copies the
editable choices there and remembers that location for recovery. Existing
registries, test history and machine-local installations stay in place. A running
controller, unfinished tests or recording ownership prevent automatic relocation;
existing destination choices are preserved in a separate setup folder.
If the shared location already has the same saved choices, Prepare resumes that
independent setup instead of migrating the old registry. Missing old experiment
history cannot prevent this recovery. Unfinished qualification tests still need
cleanup, and the destination still needs its own successful tests and activation.
`make deploy` installs the native programs (the desktop package uses
`/opt/hstream/bin`). After deployment, **Prepare computers** repairs stale
preview-release paths automatically. Programs, libraries and configuration run
from the deployment directory, normally `/opt/hstream`. Setup does not create
`setup-runtimes/` or `setup-releases/` copies. A later `make deploy` updates what
new tasks execute without another Prepare, registry rebuild or GPU test.
Custom installation folders remain selectable in Advanced profile settings.
Missing programs still require installing HStream on that computer. Headless
Jetson packages include the experiment helper; the desktop UI is optional.
For an existing setup that uses copied programs, recover the saved setup and run
**Prepare computers → Test my computers → Enable** once. Enable stops the old
managed controller, replaces its service entry and updates both desktop and
website profiles together. After that transition, later deployments need no new
Prepare or qualification pass. Old copied folders are left unused.

Operators are responsible for jobs and long-running processes during deployment;
setup does not drain GPUs, block deployment or preserve executable releases.
Restart an already running UI/controller when changing its own loaded code.

Preparation also asks each installed experiment helper whether it can read the
current stitching settings, including saved control-point selection modes. An
older helper stops preparation with a deployment command naming the main server
and all selected computers. Building a desktop from source does not update those
installed helpers. To update the whole cluster, pass its nodes explicitly, for
example `make deploy NODES=ripper,monster,mini,meshy,stubby`; plain `make deploy`
targets the local computer. `make deploy-light` takes the same nodes and installs
only HStream, leaving each computer's DeepStream alone, which is usually what a
repeat deployment wants; a computer whose DeepStream cannot satisfy the package
is named and skipped instead of changed. Older prepared generations without this compatibility
check return to Prepare. They cannot begin a new test, acquire its recording or
enable their profiles, even if their earlier tests passed.
Their saved test history and immutable runtimes remain intact.

The normal computer selection creates one **Any qualified GPU** profile. Its
`gpu:1` request stays generic, and Slurm chooses among the selected computers.
Different OS, GPU models and x86/ARM builds do not split this profile: the native
worker resolves the installed executables on the allocated computer. Specific
GPU requests remain optional in Advanced profile settings. Overlapping profiles
still share job limits.
Prepare also checks typed GPU choices against Slurm's current computers and queues.
A saved family such as `gpu:rtx:1` becomes each selected computer's exact GPU type
when that family has one matching model there. The update appears in Prepare's
activity log; GPU counts, other resource requests and the selected computers stay
the same. Unknown or ambiguous typed choices stop before testing.
Numeric model names are exact choices: a missing A10 never becomes an A100.
Tasks use the deployed libraries and baseline. The native worker records and
rechecks the selected executable bytes for that attempt; immutable request and
result bindings remain checked. Discovery and build never mark
computers qualified: each still needs a completed check or a safely deferred busy
allocation after supported-runtime preparation.

Setup qualifies deployment readiness, not video processing quality or memory capacity
for every recording. A normal stitching experiment performs calibration and player
scanning after setup. The quick check uses the ordinary Slurm/native worker protocol,
GPU allocation, artifact verification, process retirement and game ownership.
Checks use `workflow.max_parallel` and the shared profile limits. Native retirement
receipts are opened through pinned directories so stale NFS absence caches do not
add a delay to each finished job. The probe
execution deadline is bounded by `workflow.command_timeout_seconds`; scheduler queue
time is shown separately. Matching node, runtime pins, command, capabilities and all
allocation resources are required to reuse a result across profile aliases. Generic
GPU requests become exact types only on computers reporting one available GPU type.
Unchanged prepared generations retain successful checks after Stop and Resume.
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
The browser has a folder picker for shared folders visible to the website. It
lists directory names only, skips symlink/hidden folders and excludes managed
uploads. Administrators can select original recordings without typing paths.

After enabling the cluster, **Save website connection** saves the generated
`web-profiles.json` and existing observation folder into deployment settings with
revision checks. It preserves other deployment fields. This does not restart the
website: that needs the website installer's service access. Setup provides exact
settings and restart/check commands as downloadable instructions. **Check website
connection** requires the website and dispatcher's reported connection to match
and their cached readiness to pass. A missing progress service is reported as an
unfinished website requirement. The browser never claims that saving settings
alone finishes deployment.
The browser adapter's contract/receipt checks remain required; a sample test does
not certify every codec or replace browser workflow qualification.

`setup.py` owns revision-checked `setup.json` drafts and read-only SSH discovery.
Setup CLI commands bypass installed-controller dispatch to repair absent/invalid
registries. `setup_execution.py` invokes the installed controller, compiles deployed
profiles and retains configuration generations under `setup-builds/`.
`setup_storage.py` verifies shared storage and recovers relocated drafts.
`setup_runtime.py` checks node installations over SSH using each node’s installed
controller and a shared-file challenge. Preparation completes and the new
generation validates before revision-checked draft repairs are committed. Tests use
ordinary Controller plans, game ownership, reservations, receipts and retirement.
`setup-test-only.json` prevents normal profile selection and publication in test
registries. Activation requires every node's successful retired test, unchanged
draft/configuration revisions, valid runtime choices and an idle registry.
The controller holds a service lock for its lifetime so activation cannot race
startup. Older services must have a verifiably retired process on the main server;
a stale heartbeat alone never permits replacing their configuration. Interrupted
builds reuse only matching immutable generation inputs.

Qt runs the helper asynchronously. Browser GETs are cache-only; explicit admin
POSTs call the installed setup CLI. Set the local web request timeout above the
baseline setup timeout for long inventories. After response loss, resume status
before retrying. Both editors retain log scroll position and follow at the tail.
Desktop setup actions and messages are also recorded in the normal HStream
diagnostic log; browser setup failures appear in the website's server log.
