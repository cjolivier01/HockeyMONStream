"""Build and qualify a private registry using the ordinary native workflow graph.

Unqualified profiles exist only in a setup generation. Activation requires every
node's actual calibration graph to succeed, including native retirement proof.
Closing the editor does not revoke ownership or erase test evidence.
"""

import copy
import json
import os
import platform
import re
import shlex
import socket
from pathlib import Path
from uuid import uuid4

from .files import (
    file_digest,
    locked,
    private_directory,
    read_bytes,
    read_json,
    write_json,
)
from .profiles import DEFAULT_FIELDS, controller_files, load_config
from .schema import digest, positive, require, validate_plan
from .setup import SetupStore, absolute, run_readonly, validate_draft

CPU = ["experiment-create", "experiment-reuse-selection", "experiment-complete"]
GPU = [
    "experiment-baseline",
    "experiment-scan",
    "experiment-materialize",
    "experiment-run",
]


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
                descriptor = os.open(
                    target,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                    0o700 if name == "hstream-workflow" else 0o600,
                )
                with os.fdopen(descriptor, "wb") as output:
                    output.write(data)
                    output.flush()
                    os.fsync(output.fileno())
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
        return result
    finally:
        path.unlink(missing_ok=True)


def inventory(root, *, cpu):
    """Pin an installed shared runtime, including libraries and configuration.

    The native worker rechecks every pin on each allocated node. Differing
    installations fail qualification instead of borrowing dispatcher evidence.
    """
    root = absolute(root, "HStream installation").resolve(strict=True)
    require(
        root.is_dir() and root != Path("/"), "Choose an HStream installation folder"
    )
    binary = root / "bin" if (root / "bin").is_dir() else root
    task = binary / "hstream-experiment-task"
    worker = binary / "hstream-runtime-worker"
    for path in (task, worker):
        require(
            path.is_file() and not path.is_symlink() and os.access(path, os.X_OK),
            "Install the current HStream desktop runtime in "
            + str(root)
            + ". Missing executable: "
            + str(path),
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
    for index, path in enumerate(root.rglob("*")):
        require(
            index < 20000,
            "Choose an installed HStream folder, not a source checkout or home directory",
        )
        if "__pycache__" in path.parts or path.suffix in {".pyc", ".pyo"}:
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


def compile_configuration(draft, defaults, metadata_resources, inspect=inventory):
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
    task, runtime = inspect(draft["controller_runtime"], cpu=True)
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
    for key, source in draft["profiles"].items():
        require(
            key != "metadata",
            "The profile ID metadata is reserved for file preparation",
        )
        require(
            source["nodes"] and source["partition"] and source["gres"],
            source["label"] + ": choose computers, a queue and a GPU type",
        )
        nodes = set(source["nodes"])
        matching = [group for group in groups if group[1] & nodes]
        identifiers = {key}
        for group in matching:
            groups.remove(group)
            identifiers |= group[0]
            nodes |= group[1]
        groups.append((identifiers, nodes))
        task, runtime = inspect(source["runtime_root"], cpu=False)
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


def build(root, request):
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
    configuration = compile_configuration(
        store["draft"], request["defaults"], request["metadata_resources"]
    )
    generation = digest({"draft": store, "configuration": configuration})
    directory = private_directory(root / "setup-builds" / generation, create=True)
    with locked(directory / "build.lock"):
        if not (directory / "build.json").exists():
            write_json(
                directory / "setup-test-only.json", {"schema": 1}, exclusive=True
            )
            write_json(
                directory / "config.json",
                {**configuration, "observer_registry": None},
                exclusive=True,
            )
            load_config(directory)
            write_json(
                directory / "build.json",
                {
                    "schema": 1,
                    "revision": store["revision"],
                    "configuration": configuration,
                    "configuration_revision": current_revision(root),
                    "tests": {},
                    "host": socket.gethostname(),
                },
                exclusive=True,
            )
        write_json(root / "setup-build.json", {"generation": generation})
    return {
        "generation": generation,
        "message": "Registry built. Test a sample recording on each chosen computer before applying it.",
    }


def generation(root):
    identity = read_json(root / "setup-build.json")["generation"]
    require(
        isinstance(identity, str) and re.fullmatch("[a-f0-9]{64}", identity),
        "Invalid setup generation",
    )
    path = private_directory(root / "setup-builds" / identity)
    return path, read_json(path / "build.json")


def test_plan(configuration, profile_id, node, game, snapshot, scan_seconds):
    profiles = copy.deepcopy(configuration["profiles"])
    gpu = profiles[profile_id]
    gpu["resources"]["nodes"] = [node]
    # Store a separate per-node registry so admission checks use exact profiles.
    profiles = {profile_id: gpu, "metadata": profiles["metadata"]}
    stages = []

    def ref(stage):
        return {"stage": stage, "output": "workspace"}

    def add(name, kind, dependencies, inputs, cpu=False):
        stages.append(
            {
                "id": name,
                "kind": "experiment-" + kind,
                "label": name,
                "candidate_id": "setup",
                "dependencies": dependencies,
                "inputs": inputs,
                "profile_id": "metadata" if cpu else profile_id,
            }
        )

    add("prepare", "create", [], {"settings": {}}, True)
    add("prepare-target", "create", ["prepare"], {"workspace": ref("prepare")})
    add(
        "baseline", "baseline", ["prepare-target"], {"workspace": ref("prepare-target")}
    )
    positive(scan_seconds, "Player search seconds", 300)
    add(
        "scan",
        "scan",
        ["prepare-target", "baseline"],
        {
            "workspace": ref("prepare-target"),
            "baseline_workspace": ref("baseline"),
            "search_duration_seconds": scan_seconds,
        },
    )
    add("materialize", "materialize", ["scan"], {"workspace": ref("scan")})
    add(
        "reuse",
        "reuse-selection",
        ["prepare", "materialize"],
        {"workspace": ref("prepare"), "selection_workspace": ref("materialize")},
        True,
    )
    add(
        "reuse-target",
        "reuse-selection",
        ["prepare-target", "materialize", "reuse"],
        {"workspace": ref("prepare-target"), "selection_workspace": ref("materialize")},
    )
    add("calibrate", "run", ["reuse-target"], {"workspace": ref("reuse-target")})
    add("complete-target", "complete", ["calibrate"], {"workspace": ref("calibrate")})
    add(
        "complete",
        "complete",
        ["complete-target"],
        {"workspace": ref("complete-target")},
        True,
    )
    plan = {
        "schema": 1,
        "workflow_id": str(uuid4()),
        "kind": "cluster-qualification",
        "source_identity": {"game_directory": str(game)},
        "configuration_digest": digest(configuration),
        "runtime_identity": {**gpu["runtime"], "user_config_snapshot": snapshot},
        "profiles": profiles,
        "stages": stages,
        "limits": {"max_parallel": 1},
    }
    return validate_plan(plan)


def start_test(root, request):
    from .controller import Controller
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
    game = validate_game(config, request["game"])
    with locked(directory / "build.lock"):
        saved = read_json(directory / "build.json")
        # One test at a time also preserves a recording's single operation owner.
        for test in saved["tests"].values():
            status = Controller(test["registry"]).status(test["workflow"])
            require(
                status["owner_released"],
                "A previous test is still running or needs cleanup. Resume or stop it first.",
            )
        plan = test_plan(
            config,
            key,
            node,
            game,
            request["user_config_snapshot"],
            request["scan_seconds"],
        )
        test_root = private_directory(
            directory / "tests" / plan["workflow_id"], create=True
        )
        write_json(test_root / "setup-test-only.json", {"schema": 1}, exclusive=True)
        write_json(
            test_root / "config.json",
            {**config, "profiles": plan["profiles"], "observer_registry": None},
            exclusive=True,
        )
        # Record resumable intent before admission; a response lost after register
        # must never cause another independent test to be started.
        write_json(test_root / "plan.json", plan, exclusive=True)
        saved["tests"][key + "/" + node] = {
            "registry": str(test_root),
            "workflow": plan["workflow_id"],
            "profile": key,
            "node": node,
        }
        write_json(directory / "build.json", saved)
        Controller(test_root).register(plan, game, defer_admission=True)
    return qualification_status(root, tick=False)


def qualification_status(root, *, tick):
    from .controller import Controller

    directory, saved = generation(root)
    rows = []
    for key, profile in saved["configuration"]["profiles"].items():
        if key == "metadata":
            continue
        for node in profile["resources"]["nodes"]:
            test = saved["tests"].get(key + "/" + node)
            row = {"profile": key, "node": node, "state": "not-tested", "passed": False}
            if test:
                controller = Controller(test["registry"])
                plan = read_json(Path(test["registry"]) / "plan.json")
                controller.register(
                    plan,
                    plan["source_identity"]["game_directory"],
                    defer_admission=True,
                )
                if tick:
                    controller.tick(test["workflow"])
                status = controller.status(test["workflow"])
                row.update(
                    state=status["state"],
                    passed=status["state"] == "succeeded" and status["owner_released"],
                    finished=status["owner_released"],
                    status=status,
                )
                # Show the failed native stage's actual diagnostic, not an opaque
                # red badge. The normal controller owns bounded task log reads.
                if status["state"] in {"failed", "recovery"}:
                    row["next"] = (
                        "Inspect the test stage and its log. Correct the installation or sample recording, then test again."
                    )
                    for stage in status["nodes"]:
                        if (
                            stage["state"] in {"failed", "recovery"}
                            and stage["attempts"]
                        ):
                            try:
                                row["log"] = controller.log(
                                    test["workflow"], stage["id"]
                                )["text"][-32768:]
                            except (OSError, ValueError):
                                pass
            rows.append(row)
    return {
        "tests": rows,
        "tested": bool(rows) and all(row["passed"] for row in rows),
        "current": saved["revision"] == SetupStore(root).read()["revision"],
        "message": "Tests use private experiment copies. Keep this window open to schedule their next stages; reopen to resume.",
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
        require(
            read_json(Path(entry["directory"]) / "owner.json")["released"],
            "An experiment still owns a recording. Finish or stop it before applying cluster changes.",
        )


def activate(root):
    from .state import Registry
    from .profiles import preflight_runtime

    directory, saved = generation(root)
    with locked(root / "setup.lock"), locked(directory / "build.lock"), Registry(
        root
    ).lock():
        saved = read_json(directory / "build.json")
        require(
            saved["revision"] == SetupStore(root).read()["revision"],
            "Test the changed setup before applying it",
        )
        status = qualification_status(root, tick=False)
        require(
            status["tested"],
            "Every computer must pass the sample experiment before it can be enabled",
        )
        target_revision = digest(saved["configuration"])
        require(
            current_revision(root)
            in {saved["configuration_revision"], target_revision},
            "The installed settings changed. Rebuild before applying.",
        )
        idle(root)
        for profile in saved["configuration"]["profiles"].values():
            preflight_runtime(profile)
        # A running controller retains its startup configuration. Require it to
        # be stopped rather than allowing the UI and dispatcher to disagree.
        heartbeat = root / "heartbeat.json"
        if heartbeat.exists():
            import time

            require(
                time.time_ns() - read_json(heartbeat)["time_ns"]
                > saved["configuration"]["defaults"]["command_timeout_seconds"] * 1e9,
                "Stop the existing HStream controller service on the main server before applying. Running Slurm jobs must finish first.",
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

    config = load_config(root)
    active = read_json(root / "setup-active.json")
    require(
        active["configuration_revision"] == current_revision(root),
        "Apply tested settings before starting the managed controller",
    )
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
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600
        )
        with os.fdopen(descriptor, "w") as output:
            output.write(content)
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
        result = build(root, request)
    elif action == "start-test":
        check_scheduler(request["timeout_seconds"])
        result = start_test(root, request)
    elif action == "test-status":
        result = qualification_status(root, tick=True)
    elif action == "cancel-test":
        from .controller import Controller

        _, saved = generation(root)
        for test in saved["tests"].values():
            controller = Controller(test["registry"])
            status = controller.status(test["workflow"])
            if not status["owner_released"]:
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
