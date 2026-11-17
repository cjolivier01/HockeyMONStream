"""Build and qualify a private registry using the ordinary native workflow graph.

Unqualified profiles exist only in a setup generation. Activation requires every
node's runtime/GPU probe to succeed, including native retirement proof.
Closing the editor does not revoke ownership or erase test evidence.
"""

import copy
from concurrent.futures import ThreadPoolExecutor
import json
import os
import platform
import re
import shlex
import socket
import tempfile
import time
from pathlib import Path
from uuid import uuid4

from .files import (
    file_digest,
    locked,
    private_directory,
    read_bytes,
    read_json,
    sync_directory,
    write_json,
)
from .profiles import DEFAULT_FIELDS, controller_files, load_config
from .schema import WorkflowError, digest, positive, require, validate_plan
from .setup import SetupStore, absolute, run_readonly, validate_draft
from .setup_progress import failure_detail
from .setup_progress import summary as test_summary

CPU = ["experiment-create", "experiment-reuse-selection", "experiment-complete"]
GPU = [
    "experiment-probe",
    "experiment-baseline",
    "experiment-scan",
    "experiment-materialize",
    "experiment-run",
]


def publish_file(path, data, mode):
    """Install complete bytes exclusively; interruptions never publish a prefix."""
    descriptor, temporary = tempfile.mkstemp(prefix=".setup-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            os.fchmod(output.fileno(), mode)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path, follow_symlinks=False)
        sync_directory(path.parent)
    finally:
        os.unlink(temporary)


def deploy_controller(registry):
    """Copy this client's controller to an immutable, content-addressed release."""
    sources = controller_files()
    content = {
        str(path.relative_to(sources[0].parent)): read_bytes(path) for path in sources
    }
    identity = digest(
        {
            name: __import__("hashlib").sha256(data).hexdigest()
            for name, data in content.items()
        }
    )
    destination = private_directory(
        Path(registry) / "setup-releases" / identity / "scripts", create=True
    )
    with locked(Path(registry) / "setup-release.lock"):
        for name, data in content.items():
            target = destination / name
            private_directory(target.parent, create=True)
            if target.exists():
                require(
                    read_bytes(target) == data,
                    "The saved controller release was changed",
                )
            else:
                publish_file(
                    target, data, 0o700 if name == "hstream-workflow" else 0o600
                )
    return destination / "hstream-workflow"


def remote(request):
    root = private_directory(
        absolute(request["registry"], "Settings folder"), create=True
    )
    draft = SetupStore(root).read()["draft"]
    require(draft is not None, "Save your setup choices first")
    require(draft["server"], "Enter your main server name and save it first")
    if request["action"] in {"build", "start-test", "activate"}:
        require(
            validate_draft(request["draft"]) == draft,
            "Save the edited choices before building, testing or applying them",
        )
    timeout = request["timeout_seconds"]
    positive(timeout, "Setup timeout", 300)
    storage_message = ""
    if request["action"] == "build":
        from .setup_storage import prepare

        started = time.monotonic()
        request, storage_message = prepare(request)
        timeout -= int(time.monotonic() - started)
        require(
            timeout > 0,
            "Checking shared storage exceeded the setup timeout. Check the computer connections and build again.",
        )
        request = {**request, "timeout_seconds": timeout}
        root = private_directory(
            absolute(request["registry"], "Shared settings folder")
        )
        draft = request["draft"]
    launcher = deploy_controller(root)
    path = root / ("setup-request-" + uuid4().hex + ".json")
    # The random challenge also proves that the server sees the same storage.
    challenge = uuid4().hex
    write_json(
        path, {**request, "on_server": True, "challenge": challenge}, exclusive=True
    )
    try:
        argv = [
            "ssh",
            "-oBatchMode=yes",
            "-oStrictHostKeyChecking=yes",
            "-oConnectTimeout=" + str(timeout),
            "--",
            draft["server"],
            shlex.join([str(launcher), "setup", "--request", str(path)]),
        ]
        result = json.loads(run_readonly(argv, timeout))
        require(
            result.pop("challenge", None) == challenge,
            "The server cannot see the same settings folder",
        )
        if storage_message:
            result["message"] = storage_message + "\n" + result.get("message", "")
        return result
    finally:
        path.unlink(missing_ok=True)


def inventory(root, *, cpu, paths=None):
    """Pin an installed shared runtime, including libraries and configuration.

    The native worker rechecks every pin on each allocated node. Differing
    installations fail qualification instead of borrowing dispatcher evidence.
    """
    root = absolute(root, "HStream installation").resolve()
    location = "main server" if cpu else "compute profile"
    next_step = (
        "Click Find computers / test connection, then Use installed HStream for main server."
        if cpu
        else "Open Advanced profile settings and choose the current HStream installation folder for these computers."
    )
    repair = (
        "If make deploy just succeeded, the saved folder may refer to an older installation. "
        + next_step
        + " Then Save and build registry again."
    )
    require(
        root.is_dir() and root != Path("/"),
        "The "
        + location
        + " has an unavailable HStream installation folder: "
        + str(root)
        + ".\n"
        + repair,
    )
    binary = root / "bin" if (root / "bin").is_dir() else root
    task = binary / "hstream-experiment-task"
    worker = binary / "hstream-runtime-worker"
    for path in (task, worker):
        require(
            path.is_file() and not path.is_symlink() and os.access(path, os.X_OK),
            "The "
            + location
            + " still points to a folder without the required HStream job programs.\n"
            "Saved folder: "
            + str(root)
            + "\nMissing program: "
            + path.name
            + "\n"
            + repair,
        )
    # ELF machine identity works for cross-architecture installations without
    # executing a foreign binary on the controller.
    with task.open("rb") as source:
        header = source.read(20)
    require(
        header[:4] == b"\x7fELF" and header[5] in {1, 2},
        "The experiment task must be the installed native binary",
    )
    machine = int.from_bytes(header[18:20], "little" if header[5] == 1 else "big")
    architecture = {62: "x86_64", 183: "aarch64"}.get(machine)
    require(architecture is not None, "Unsupported HStream processor architecture")
    if cpu:
        require(
            architecture == platform.machine(),
            "The main-server installation is for another processor",
        )
    files = {}
    # Installed desktop packages also bundle a Python controller. Setup deploys
    # its own immutable controller release; pinning both makes client dispatch
    # ambiguous. Native tasks do not execute the bundled controller copy.
    entries = []
    for index, path in enumerate(root.rglob("*") if paths is None else paths):
        require(
            index < 20000,
            "Choose an installed HStream folder, not a source checkout or home directory",
        )
        entries.append(path)
    bundled_controllers = {
        path.parent
        for path in entries
        if path.name == "hstream-workflow"
        and (path.parent / "hstream_workflow" / "cli.py").is_file()
    }
    for path in entries:
        if "__pycache__" in path.parts or path.suffix in {".pyc", ".pyo"}:
            continue
        if any(
            path == directory / "hstream-workflow"
            or path.is_relative_to(directory / "hstream_workflow")
            for directory in bundled_controllers
        ):
            continue
        if path.is_file():
            canonical = path.resolve(strict=True)
            require(
                canonical.is_relative_to(root),
                "Runtime links must stay inside the installed folder: " + str(path),
            )
            require(len(files) < 4000, "The HStream installation has too many files")
            files[str(canonical)] = file_digest(canonical, 8 * 1024**3)["sha256"]
    runtime = {
        "root": str(root),
        "worker": str(worker),
        "files": files,
        "architecture": architecture,
        "backend": "cpu" if cpu else "nvidia",
    }
    if not cpu:
        for key, path in {
            "cli": binary / "hstream-cli",
            "exporter": binary / "hstream-job",
            "config": root / "configs/ds_hockey_app_config.yaml",
        }.items():
            require(str(path) in files, "Missing HStream runtime file: " + str(path))
            runtime[key] = str(path)
    return str(task), runtime


def compile_configuration(
    draft, defaults, metadata_resources, inspect=inventory, prepared=None
):
    draft = validate_draft(draft)
    require(draft["profiles"], "Choose at least one computer profile")
    require(draft["game_roots"], "Choose the shared folder containing your recordings")
    require(
        set(defaults) == DEFAULT_FIELDS,
        "Resolve workflow defaults from the HStream baseline",
    )
    for key, value in defaults.items():
        positive(value, key)
    require(
        set(metadata_resources) == {"cpus", "memory_mb", "time_seconds"},
        "Resolve CPU resources from the HStream baseline",
    )
    for key, value in metadata_resources.items():
        positive(value, key)
    namespace = "setup-" + digest(draft["registry"])[:24]
    if prepared is None:
        task, runtime = inspect(draft["controller_runtime"], cpu=True)
    else:
        task, runtime = prepared["metadata"]["task"], copy.deepcopy(
            prepared["metadata"]["runtime"]
        )
    runtime["storage_namespace"] = namespace
    runtime["files"].update(
        {
            str(path): file_digest(path, 8 * 1024 * 1024)["sha256"]
            for path in controller_files()
        }
    )
    profiles = {
        "metadata": {
            "label": "Prepare experiment files",
            "executor": "local",
            "pool": "metadata",
            "task_command": [task],
            "runtime": runtime,
            "capabilities": CPU,
            "qualified_nodes": {},
            "resources": {
                **metadata_resources,
                "gpus": 0,
                "nodes": [],
                "devices": [],
                **{
                    key: ""
                    for key in ("partition", "gres", "account", "qos", "constraint")
                },
            },
            "limits": {
                "concurrency": defaults["max_parallel"],
                "per_user": defaults["max_parallel"],
            },
        }
    }
    # Overlapping aliases share a counter. Never silently multiply capacity.
    groups = []
    expanded = []
    for key, source in draft["profiles"].items():
        if prepared is None:
            expanded.append((key, source, None, source["nodes"]))
            continue
        variants = prepared["profiles"][key]
        require(variants, source["label"] + ": choose at least one computer")
        for variant in variants:
            actual = copy.deepcopy(source)
            actual["nodes"] = sorted(variant["nodes"])
            actual["gres"] = variant.get("gres", source["gres"])
            profile_id = key
            if len(variants) > 1:
                profile_id = key[:80] + "-" + digest(actual["nodes"])[:12]
                actual["label"] = (
                    source["label"] + " — " + ", ".join(actual["nodes"])
                )[:256]
            expanded.append((profile_id, actual, variant, source["nodes"]))
    for key, source, variant, pool_nodes in expanded:
        require(
            key not in profiles,
            "Prepared profile IDs overlap; rename the duplicate profile",
        )
        require(
            key != "metadata",
            "The profile ID metadata is reserved for file preparation",
        )
        require(
            source["nodes"] and source["partition"] and source["gres"],
            source["label"] + ": choose computers, a queue and a GPU type",
        )
        nodes = set(pool_nodes)
        matching = [group for group in groups if group[1] & nodes]
        identifiers = {key}
        for group in matching:
            groups.remove(group)
            identifiers |= group[0]
            nodes |= group[1]
        groups.append((identifiers, nodes))
        try:
            if variant is None:
                task, runtime = inspect(source["runtime_root"], cpu=False)
            else:
                task, runtime = variant["task"], copy.deepcopy(variant["runtime"])
        except WorkflowError as error:
            raise WorkflowError(source["label"] + ": " + str(error)) from error
        runtime["storage_namespace"] = namespace
        profiles[key] = {
            "label": source["label"],
            "executor": "slurm",
            "pool": key,
            "task_command": [task],
            "runtime": runtime,
            "capabilities": CPU + GPU,
            "resources": {
                **{
                    field: copy.deepcopy(source[field])
                    for field in (
                        "nodes",
                        "partition",
                        "gres",
                        "cpus",
                        "memory_mb",
                        "time_seconds",
                        "account",
                        "qos",
                        "constraint",
                    )
                },
                "devices": [],
                "gpus": 1,
            },
            "limits": {field: source[field] for field in ("concurrency", "per_user")},
            # These declarations are confined to a test registry. They become
            # production evidence only after qualification_status verifies runs.
            "qualified_nodes": {
                node: {
                    "runtime_identity": copy.deepcopy(runtime),
                    "capabilities": CPU + GPU,
                }
                for node in source["nodes"]
            },
        }
    for identifiers, _ in groups:
        pool = sorted(identifiers)[0]
        for key in identifiers:
            require(
                profiles[key]["limits"] == profiles[pool]["limits"],
                "Profiles sharing computers must use the same parallel-job limits",
            )
            profiles[key]["pool"] = pool
    return {
        "schema": 1,
        "defaults": copy.deepcopy(defaults),
        "profiles": profiles,
        "admission_profile": "metadata",
        "allowed_game_roots": draft["game_roots"],
        "observer_registry": draft["observer_registry"],
    }


def build(root, request, prepared=None, repaired_draft=None):
    from .dispatch import installed_launcher

    store = SetupStore(root).read()
    require(
        store["revision"] == request["revision"],
        "Setup choices changed. Save and build them again.",
    )
    if (root / "setup-build.json").exists():
        previous = qualification_status(root, tick=False)
        require(
            all(row.get("finished", True) for row in previous["tests"]),
            "Resume or stop the previous test before rebuilding",
        )
    expected_revision = store["revision"]
    if repaired_draft is not None:
        store = {"draft": repaired_draft, "revision": digest(repaired_draft)}
    if prepared is None:
        configuration = compile_configuration(
            store["draft"], request["defaults"], request["metadata_resources"]
        )
    else:
        configuration = compile_configuration(
            store["draft"],
            request["defaults"],
            request["metadata_resources"],
            prepared=prepared,
        )
    installed_launcher(configuration)
    configuration_revision = current_revision(root)
    setup_features = prepared.get("features", []) if prepared else []
    gpu_supported = bool(prepared and prepared.get("gpu_supported") is True)
    generation = digest(
        {
            "draft": store,
            "configuration": configuration,
            "configuration_revision": configuration_revision,
            "setup_features": setup_features,
            "gpu_supported": gpu_supported,
        }
    )
    directory = private_directory(root / "setup-builds" / generation, create=True)
    with locked(directory / "build.lock"):
        if not (directory / "build.json").exists():
            # A lost response or interrupted write can leave part of this
            # deterministic generation. Reuse only matching immutable inputs.
            for name, value in {
                "setup-test-only.json": {"schema": 1},
                "config.json": {**configuration, "observer_registry": None},
            }.items():
                path = directory / name
                if path.exists():
                    require(
                        read_json(path) == value,
                        "Interrupted setup inputs changed: " + str(path),
                    )
                else:
                    write_json(path, value, exclusive=True)
            load_config(directory)
            write_json(
                directory / "build.json",
                {
                    "schema": 1,
                    "revision": store["revision"],
                    "configuration": configuration,
                    "setup_features": setup_features,
                    "gpu_supported": gpu_supported,
                    "configuration_revision": configuration_revision,
                    "tests": {},
                    "host": socket.gethostname(),
                },
                exclusive=True,
            )
        # Validate the complete generation before committing repaired choices.
        # Concurrent edits win over a slow runtime preparation.
        with locked(root / "setup.lock"):
            require(
                SetupStore(root).read()["revision"] == expected_revision,
                "These settings changed in another window. Recover them before building again.",
            )
            if repaired_draft is not None:
                write_json(root / "setup.json", repaired_draft)
            write_json(root / "setup-build.json", {"generation": generation})
    return {
        "generation": generation,
        **store,
        "message": "Computers prepared. Run the quick HStream and GPU checks, then enable your computers.",
    }


def generation(root):
    require(
        (root / "setup-build.json").exists(),
        "Build the setup before testing or applying it. Click Save and build registry. If that step reported a problem, correct it first.",
    )
    identity = read_json(root / "setup-build.json")["generation"]
    require(
        isinstance(identity, str) and re.fullmatch("[a-f0-9]{64}", identity),
        "Invalid setup generation",
    )
    path = private_directory(root / "setup-builds" / identity)
    return path, read_json(path / "build.json")


def qualification_checks(saved):
    """Share proof only for identical node, runtime and allocation requirements.

    Preparation resolves generic GPUs only when Slurm reports one exact type.
    Legacy generations keep their original rows so their jobs can still retire.
    """
    grouped = {}
    quick = "cluster-readiness-v1" in saved.get("setup_features", [])
    for key, profile in saved["configuration"]["profiles"].items():
        if key == "metadata":
            continue
        for node in profile["resources"]["nodes"]:
            identity = (
                digest(
                    {
                        "node": node,
                        "executor": profile["executor"],
                        "task_command": profile["task_command"],
                        "runtime": profile["runtime"],
                        "capabilities": profile["capabilities"],
                        "resources": {**profile["resources"], "nodes": [node]},
                    }
                )
                if quick
                else key + "/" + node
            )
            grouped.setdefault(identity, []).append((key, node))
    return [
        (members[0][0], members[0][1], [key for key, _ in members])
        for members in grouped.values()
    ]


def test_plan(configuration, profile_id, node, game, snapshot, scan_seconds):
    # scan_seconds remains accepted for older desktop/browser request schemas.
    # Setup validates deployment readiness. Recording processing belongs to an
    # ordinary stitching experiment, not a mandatory cluster configuration test.
    gpu = copy.deepcopy(configuration["profiles"][profile_id])
    gpu["resources"]["nodes"] = [node]
    gpu["resources"]["time_seconds"] = min(
        gpu["resources"]["time_seconds"],
        configuration["defaults"]["command_timeout_seconds"],
    )
    plan = {
        "schema": 1,
        "workflow_id": str(uuid4()),
        "kind": "cluster-qualification",
        "source_identity": {"game_directory": str(game)},
        "configuration_digest": digest(configuration),
        "runtime_identity": {**gpu["runtime"], "user_config_snapshot": snapshot},
        "profiles": {
            profile_id: gpu,
            "metadata": copy.deepcopy(configuration["profiles"]["metadata"]),
        },
        "stages": [
            {
                "id": "probe",
                "kind": "experiment-probe",
                "label": "Check HStream and the GPU",
                "candidate_id": "setup",
                "dependencies": [],
                "inputs": {},
                "profile_id": profile_id,
            }
        ],
        "limits": {"max_parallel": 1},
    }
    return validate_plan(plan)


def start_test(root, request):
    from .profiles import validate_game

    directory, saved = generation(root)
    require(
        saved["revision"] == SetupStore(root).read()["revision"],
        "Build the changed setup before testing",
    )
    key, node = request["profile"], request["node"]
    config = saved["configuration"]
    require(
        key != "metadata" and key in config["profiles"], "Choose a built GPU profile"
    )
    require(
        node in config["profiles"][key]["resources"]["nodes"],
        "Choose a computer in this profile",
    )
    for representative, computer, aliases in qualification_checks(saved):
        if node == computer and key in aliases:
            key = representative
            break
    game = validate_game(config, request["game"])
    parallel = request.get("all_computers", False)
    require(type(parallel) is bool, "Choose whether to check all computers")
    with locked(directory / "build.lock"):
        saved = read_json(directory / "build.json")
        active, complete = {}, set()
        from .setup_checks import is_deferred

        for identity, test in saved["tests"].items():
            _, status, rejection = restore_test(test)
            if rejection is None:
                if not status["owner_released"]:
                    active[identity] = test
                elif status["state"] == "succeeded" or (
                    saved.get("gpu_supported") and is_deferred(test, status)
                ):
                    complete.add(identity)
        require(
            parallel or not active,
            "A previous test is still running or needs cleanup. Resume or stop it first.",
        )
        targets = [(key, node)]
        if parallel:
            targets += [
                (k, n)
                for k, n, _ in qualification_checks(saved)
                if (k, n) != (key, node)
            ]
        for key, node in targets:
            identity = key + "/" + node
            if identity in complete or identity in active:
                continue
            if len(active) >= config["defaults"]["max_parallel"]:
                break
            if any(test["node"] == node for test in active.values()):
                continue
            profile = config["profiles"][key]
            in_pool = sum(
                config["profiles"][t["profile"]]["pool"] == profile["pool"]
                for t in active.values()
            )
            if in_pool >= min(profile["limits"].values()):
                continue
            # The probe reads no recording inputs. Give each computer a private
            # empty game for ordinary native ownership and verified receipts.
            probe_game = (
                private_directory(
                    game
                    / "_hockeymon/cluster-checks"
                    / directory.name
                    / digest(identity),
                    create=True,
                )
                if parallel
                else game
            )
            plan = test_plan(
                config,
                key,
                node,
                probe_game,
                request["user_config_snapshot"],
                request["scan_seconds"],
            )
            test_root = private_directory(
                directory / "tests" / plan["workflow_id"], create=True
            )
            write_json(
                test_root / "setup-test-only.json", {"schema": 1}, exclusive=True
            )
            write_json(
                test_root / "config.json",
                {**config, "profiles": plan["profiles"], "observer_registry": None},
                exclusive=True,
            )
            write_json(test_root / "plan.json", plan, exclusive=True)
            test = {
                "registry": str(test_root),
                "workflow": plan["workflow_id"],
                "profile": key,
                "node": node,
                "sample": str(game),
            }
            saved["tests"][identity] = test
            # Seal each resumable intent before admission or a lost response.
            write_json(directory / "build.json", saved)
            restore_test(test)
            active[identity] = test
    return qualification_status(root, tick=False)


def restore_test(test):
    from .controller import Controller, SubmissionRejected

    root = Path(test["registry"])
    plan = read_json(root / "plan.json")
    rejected = root / "rejected.json"
    if rejected.exists():
        record = read_json(rejected)
        require(
            record["workflow"] == test["workflow"]
            and record["plan_digest"] == digest(plan),
            "The saved test rejection belongs to another plan",
        )
        return None, None, record["reason"]
    controller = Controller(root)
    try:
        status = controller.register(
            plan, plan["source_identity"]["game_directory"], defer_admission=True
        )
    except SubmissionRejected as error:
        # This exception proves absence under the registry lock. Never infer
        # rejection from response loss, an unknown status or a scheduler outage.
        write_json(
            rejected,
            {
                "workflow": test["workflow"],
                "plan_digest": digest(plan),
                "reason": str(error),
            },
            exclusive=True,
        )
        return None, None, str(error)
    return controller, status, None


def qualification_status(root, *, tick):
    from .setup_runtime import has_current_features

    directory, saved = generation(root)
    current_programs = has_current_features(saved.get("setup_features"))
    from .setup_checks import defer_busy, is_deferred

    def check(target):
        key, node, aliases = target
        profile = saved["configuration"]["profiles"][key]
        test = saved["tests"].get(key + "/" + node)
        row = {
            "profile": key,
            "label": (
                "HStream and GPU"
                if "cluster-readiness-v1" in saved.get("setup_features", [])
                else profile.get("label", key)
            ),
            "profiles": aliases,
            "node": node,
            "state": "not-tested",
            "passed": False,
        }
        if test:
            row["game"] = (
                test.get("sample")
                or read_json(Path(test["registry"]) / "plan.json")["source_identity"][
                    "game_directory"
                ]
            )
            controller, status, rejection = restore_test(test)
            if rejection is not None:
                explanation = (
                    rejection
                    + "\nCorrect the setup, then save and build again. No job was admitted."
                )
                row.update(
                    state="rejected",
                    finished=True,
                    log=explanation,
                    next=explanation,
                )
                return row
            if tick and not status["owner_released"]:
                if not current_programs:
                    # Older clients may have saved intent before their first
                    # poll, or lost an acquire/submission response. Cancel
                    # through the normal fenced path before any tick: owner
                    # inspection and cleanup remain available, but neither
                    # ownership acquisition nor a new stage can be admitted.
                    status = controller.cancel(test["workflow"], status["epoch"])
                status = defer_busy(
                    controller,
                    test,
                    status,
                    enabled=current_programs and saved.get("gpu_supported") is True,
                )
                controller.tick(test["workflow"])
                status = controller.status(test["workflow"])
            row.update(
                state=status["state"],
                passed=status["state"] == "succeeded" and status["owner_released"],
                finished=status["owner_released"],
                status=status,
            )
            # The controller owns bounded live and failed-stage log reads.
            if status["state"] in {"failed", "recovery", "running"}:
                # A later running/cleanup stage must not hide the failed
                # attempt or substitute an earlier, recovered diagnostic.
                stages = [
                    stage
                    for state in ("failed", "recovery", "running")
                    for stage in status["nodes"]
                    if stage["state"] == state
                ]
                if stages:
                    stage = stages[0]
                    if stage["attempts"]:
                        try:
                            row["log"] = controller.log(test["workflow"], stage["id"])[
                                "text"
                            ][-32768:]
                        except (OSError, ValueError):
                            pass
                    if stage["state"] in {"failed", "recovery"}:
                        row["next"] = failure_detail(
                            stage, row.get("log", ""), finished=row["finished"]
                        )
            if not current_programs and (
                not row["finished"] or status.get("cancel_requested")
            ):
                if row["finished"]:
                    instruction = "The old test has stopped. Go back to Prepare computers before testing again."
                elif status.get("cancel_requested") or status.get(
                    "cancellation_pending"
                ):
                    instruction = "The old test is stopping before any more stages can start. Wait for cleanup, then go back to Prepare computers."
                else:
                    instruction = (
                        "Stop this old test before going back to Prepare computers."
                    )
                row["next"] = (
                    "This test uses HStream programs that have not been checked for current stitching settings. "
                    + instruction
                )
            if saved.get("gpu_supported") is True and is_deferred(test, status):
                row.update(
                    state="deferred",
                    ready=True,
                    finished=True,
                    next="Slurm resources are busy. Connection, storage and installed HStream are ready; the GPU check is deferred. Workflows will wait for a free GPU.",
                )
        row["ready"] = row.get("ready", row["passed"])
        return row

    targets = qualification_checks(saved)
    if tick:
        with ThreadPoolExecutor(
            max_workers=saved["configuration"]["defaults"]["max_parallel"]
        ) as workers:
            rows = list(workers.map(check, targets))
    else:
        rows = list(map(check, targets))
    return {
        "tests": rows,
        "test_progress": test_summary(rows),
        "tested": bool(rows) and all(row.get("ready", row["passed"]) for row in rows),
        "current": saved["revision"] == SetupStore(root).read()["revision"],
        "message": "Quick checks start HStream on each computer and verify its GPU and shared storage. No video is processed. Passed checks are kept.",
    }


def current_revision(root):
    return (
        digest(read_json(root / "config.json"))
        if (root / "config.json").exists()
        else None
    )


def idle(root):
    from .state import Registry

    ledger = Registry(root).read()
    require(
        not ledger["reservations"],
        "Wait for running experiments and cleanup before applying cluster changes",
    )
    for entry in ledger["workflows"].values():
        owner_path = Path(entry["directory"]) / "owner.json"
        try:
            owner = read_json(owner_path)
        except FileNotFoundError as error:
            raise WorkflowError(
                "An older experiment's history is missing for "
                + Path(entry["game"]).name
                + ". Setup cannot verify that it finished. Recover the existing shared setup, "
                "or restore this experiment's history before replacing its registry. Missing file: "
                + str(owner_path)
            ) from error
        require(
            owner["released"],
            "An experiment still owns a recording. Finish or stop it before applying cluster changes.",
        )


def require_current_setup(saved):
    from .setup_runtime import has_current_features

    require(
        has_current_features(saved.get("setup_features")),
        "These prepared computers have not been checked for current stitching settings. "
        "Go back to Prepare computers before testing or enabling them. "
        "If HStream is outdated, update the main server and all selected computers.",
    )


def activate(root):
    # The service holds this lock from before reading configuration until exit.
    # Nonblocking acquisition also excludes a concurrently starting controller.
    try:
        with locked(root / "controller-service.lock", blocking=False):
            return activate_stopped(root)
    except BlockingIOError as error:
        raise ValueError(
            "Stop the HStream controller before applying settings"
        ) from error


def activate_stopped(root):
    from .state import Registry
    from .profiles import preflight_runtime

    directory, saved = generation(root)
    with locked(root / "setup.lock"), locked(directory / "build.lock"), Registry(
        root
    ).lock():
        saved = read_json(directory / "build.json")
        require_current_setup(saved)
        require(
            saved["revision"] == SetupStore(root).read()["revision"],
            "Test the changed setup before applying it",
        )
        status = qualification_status(root, tick=False)
        require(
            status["tested"],
            "Every computer needs a finished check or a safely deferred busy GPU check before it can be enabled",
        )
        target_revision = digest(saved["configuration"])
        require(
            current_revision(root)
            in {saved["configuration_revision"], target_revision},
            "The installed settings changed. Rebuild before applying.",
        )
        idle(root)
        # One activation verifies shared files once across equivalent aliases.
        # This cache lasts only for this operation; every worker still checks its pins.
        verified = set()
        for profile in saved["configuration"]["profiles"].values():
            preflight_runtime(profile, verified)
        # Older services do not hold controller-service.lock. Heartbeat age is
        # not retirement proof: an idle or paused process can still resume.
        heartbeat = root / "heartbeat.json"
        if heartbeat.exists():
            from .processes import host_identity, same_process

            identity = read_json(heartbeat)
            require(
                identity.get("host") == host_identity()["host"],
                "Apply settings on the previous controller's server after stopping it: "
                + str(identity.get("host", "unknown")),
            )
            require(
                identity.get("boot_id")
                and isinstance(identity.get("pid"), int)
                and isinstance(identity.get("start_time"), int),
                "The previous controller has no verifiable process identity",
            )
            require(
                not same_process(identity),
                "Stop the existing HStream controller service on the main server before applying. A stale heartbeat does not mean the process stopped.",
            )
        if (root / "config.json").exists() and current_revision(
            root
        ) != target_revision:
            backup = root / (
                "config-before-" + saved["configuration_revision"] + ".json"
            )
            if not backup.exists():
                write_json(backup, read_json(root / "config.json"), exclusive=True)
        write_json(root / "config.json", saved["configuration"])
        write_json(
            root / "web-profiles.json",
            {
                "schema": 1,
                "profiles": {
                    key: value
                    for key, value in saved["configuration"]["profiles"].items()
                    if value["executor"] == "slurm"
                },
            },
        )
        write_json(
            root / "setup-active.json",
            {
                "revision": saved["revision"],
                "configuration_revision": current_revision(root),
            },
        )
    return {
        "applied": True,
        "message": "Cluster settings saved. Start the controller on the main server to use them.",
    }


def start_service(root, timeout):
    from .dispatch import installed_launcher

    require(
        (root / "setup-active.json").exists(),
        "This setup has not been applied yet. First Save and build registry, test each computer, and Apply tested settings. Then Start controller.",
    )
    active = read_json(root / "setup-active.json")
    require(
        active["configuration_revision"] == current_revision(root),
        "Apply tested settings before starting the managed controller",
    )
    config = load_config(root)
    launcher = installed_launcher(config)
    unit = "hstream-workflow-" + digest(str(root))[:16] + ".service"
    directory = private_directory(Path.home() / ".config/systemd/user", create=True)

    # systemd uses percent specifiers even inside quoted arguments.
    def quote(value):
        return (
            '"'
            + str(value)
            .replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("%", "%%")
            .replace("$", "$$")
            + '"'
        )

    content = (
        "# HStream registry: " + json.dumps(str(root)) + "\n"
        "[Unit]\nDescription=HStream experiment controller\nAfter=network-online.target\n\n[Service]\n"
        "Type=simple\nUMask=0077\nKillMode=process\nRestart=on-failure\nExecStart="
        + quote(launcher)
        + " serve --registry "
        + quote(root)
        + "\nWorkingDirectory="
        + str(launcher.parent).replace("%", "%%")
        + "\n\n[Install]\nWantedBy=default.target\n"
    )
    path = directory / unit
    if path.exists():
        require(
            read_bytes(path).decode() == content,
            "Stop and remove the old managed user service file before starting the updated controller: "
            + str(path),
        )
    else:
        publish_file(path, content.encode(), 0o600)
    run_readonly(["systemctl", "--user", "daemon-reload"], timeout)
    run_readonly(["systemctl", "--user", "enable", "--now", unit], timeout)
    return {
        "service": unit,
        "message": "Controller started. Check installed setup to verify its heartbeat. For operation after logout, enable user lingering on the main server.",
    }


def stop_service(root, timeout):
    from .state import Registry

    with Registry(root).lock():
        idle(root)
        unit = "hstream-workflow-" + digest(str(root))[:16] + ".service"
        managed = Path.home() / ".config/systemd/user" / unit
        if managed.exists():
            run_readonly(["systemctl", "--user", "disable", "--now", unit], timeout)
            managed.unlink()
        # The existing system service is outside this editor's ownership. Show
        # the exact action rather than replacing an administrator's unit.
    return {
        "message": "Managed controller stopped. If the older system service still uses this registry, run sudo systemctl stop hstream-workflow-controller on the main server after its jobs finish. Apply settings, then Start controller."
    }


def execute(request):
    root = private_directory(absolute(request["registry"], "Settings folder"))
    # A second browser or desktop may use the same registry. Keep generation
    # changes, test admission and activation in one order across those clients.
    with locked(root / "setup-execution.lock"):
        return execute_locked(root, request)


def execute_locked(root, request):
    action = request["action"]
    if action in {"build", "start-test", "activate"}:
        require(
            validate_draft(request["draft"]) == SetupStore(root).read()["draft"],
            "Saved setup choices changed before the server received this action",
        )
    if action == "build":
        from .setup_runtime import prepare
        from .setup_storage import recovered

        store = SetupStore(root).read()
        require(
            recovered(root) == store,
            "This setup moved while preparing. Recover the saved setup and try Prepare computers again.",
        )
        require(
            store["revision"] == request["revision"],
            "Setup choices changed. Recover the saved setup before building again.",
        )
        if (root / "setup-build.json").exists():
            status = qualification_status(root, tick=False)
            require(
                all(row.get("finished", True) for row in status["tests"]),
                "Resume or stop the previous test before rebuilding",
            )
        parallel = request["defaults"]["max_parallel"]
        positive(parallel, "Runtime preparation parallelism", 256)
        draft, prepared, messages = prepare(
            root, store["draft"], request["timeout_seconds"], parallel
        )
        result = build(root, request, prepared=prepared, repaired_draft=draft)
        result["message"] = "\n".join(
            [
                *messages,
                "HStream is prepared on shared storage for every selected computer.",
                result["message"],
            ]
        )
    elif action == "start-test":
        _, saved = generation(root)
        require_current_setup(saved)
        check_scheduler(request["timeout_seconds"])
        result = start_test(root, request)
    elif action == "test-status":
        result = qualification_status(root, tick=request.get("tick", True))
    elif action == "cancel-test":
        _, saved = generation(root)
        for test in saved["tests"].values():
            controller, status, rejection = restore_test(test)
            if rejection is None and not status["owner_released"]:
                controller.cancel(test["workflow"], status["epoch"])
        result = qualification_status(root, tick=True)
    elif action == "activate":
        result = activate(root)
    elif action == "start-service":
        result = start_service(root, request["timeout_seconds"])
    elif action == "stop-service":
        result = stop_service(root, request["timeout_seconds"])
    else:
        raise ValueError("Unknown setup execution step")
    return {**result, "challenge": request["challenge"]}


def check_scheduler(timeout):
    """Fail before recording submission intent when this is not a usable client."""
    config = run_readonly(["scontrol", "show", "config"], timeout)
    cluster = re.search(
        r"^ClusterName\s*=\s*([A-Za-z0-9_.-]+)\s*$", config, re.MULTILINE
    )
    require(cluster is not None, "Slurm did not report a cluster name")
    require(
        re.search(r"^ProctrackType\s*=\s*proctrack/cgroup\s*$", config, re.MULTILINE)
        and re.search(r"^TaskPlugin\s*=.*task/cgroup", config, re.MULTILINE),
        "Slurm needs proctrack/cgroup and task/cgroup before tests can run",
    )
    try:
        run_readonly(
            ["squeue", "--noheader", "--format=%i", "--clusters=" + cluster.group(1)],
            timeout,
        )
        run_readonly(
            [
                "sacct",
                "--noheader",
                "--allocations",
                "--starttime=now",
                "--format=JobID",
                "--clusters=" + cluster.group(1),
            ],
            timeout,
        )
    except ValueError as error:
        raise ValueError(
            "This server cannot query the Slurm cluster. Choose the main submission server or repair its Slurm client connection before starting a test. "
            + str(error)
        ) from error
