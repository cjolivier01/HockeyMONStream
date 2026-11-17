"""Resumable cluster setup, separate from the installed execution controller.

Drafts may be incomplete. They never authorize execution. Discovery does not
submit work or change services, and successful discovery is not qualification.
"""

import copy
import json
import os
import re
import shlex
import subprocess
import tempfile
import time
from pathlib import Path

from .files import locked, private_directory, read_json, write_json
from .profiles import load_config
from .setup_progress import forward
from .schema import WorkflowError, digest, identifier, positive, require, text


def absolute(value, field):
    text(value, field)
    require(
        not any(ord(char) < 32 or ord(char) == 127 for char in value),
        field + " cannot contain control characters",
    )
    path = Path(value)
    require(
        path.is_absolute() and ".." not in path.parts,
        field + " must be an absolute path",
    )
    return path


def validate_draft(value):
    require(
        isinstance(value, dict) and value.get("schema") == 1, "Unsupported setup draft"
    )
    require(
        set(value)
        == {
            "schema",
            "server",
            "registry",
            "game_roots",
            "profiles",
            "observer_registry",
            "controller_runtime",
        },
        "Unknown or missing setup fields",
    )
    server = value["server"]
    require(
        isinstance(server, str)
        and (not server or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,252}", server)),
        "Enter a server name, such as ripper, without a command or URL",
    )
    absolute(value["registry"], "Settings folder")
    absolute(value["controller_runtime"], "Main server HStream installation")
    require(
        isinstance(value["game_roots"], list) and len(value["game_roots"]) <= 64,
        "Choose at most 64 recording folders",
    )
    for root in value["game_roots"]:
        absolute(root, "Recording folder")
    if value["observer_registry"] is not None:
        absolute(value["observer_registry"], "Progress dashboard folder")
    profiles = value["profiles"]
    require(
        isinstance(profiles, dict) and len(profiles) <= 64,
        "Choose at most 64 compute profiles",
    )
    for key, profile in profiles.items():
        identifier(key, "Profile ID")
        require(
            isinstance(profile, dict)
            and set(profile)
            == {
                "label",
                "nodes",
                "partition",
                "gres",
                "cpus",
                "memory_mb",
                "time_seconds",
                "concurrency",
                "per_user",
                "account",
                "qos",
                "constraint",
                "runtime_root",
            },
            "Unknown or missing compute profile fields",
        )
        text(profile["label"], "Profile name", 256)
        require(
            isinstance(profile["nodes"], list) and len(profile["nodes"]) <= 64,
            "Choose at most 64 machines for a profile",
        )
        require(
            len(set(profile["nodes"])) == len(profile["nodes"]),
            "A machine was selected twice",
        )
        for node in profile["nodes"]:
            identifier(node, "Machine name")
        for field in ("partition", "gres", "account", "qos", "constraint"):
            require(
                isinstance(profile[field], str)
                and re.fullmatch(r"[A-Za-z0-9_:,.+\-]*", profile[field]),
                "Invalid " + field,
            )
        for field in ("cpus", "memory_mb", "time_seconds", "concurrency", "per_user"):
            positive(
                profile[field],
                field,
                256 if field in {"concurrency", "per_user"} else 2147483647,
            )
        if profile["runtime_root"]:
            absolute(profile["runtime_root"], "HStream installation")
    return copy.deepcopy(value)


class SetupStore:
    """Compare-and-swap drafts: an old dialog cannot overwrite a newer edit."""

    def __init__(self, registry):
        self.root = absolute(str(registry), "Settings folder")

    def read(self):
        private_directory(self.root)
        path = self.root / "setup.json"
        if not path.exists():
            return {"draft": None, "revision": None}
        value = read_json(path)
        return {"draft": validate_draft(value), "revision": digest(value)}

    def save(self, value, expected):
        value = validate_draft(value)
        require(
            Path(value["registry"]) == self.root,
            "The setup belongs to a different settings folder",
        )
        private_directory(self.root, create=True)
        with locked(self.root / "setup.lock"):
            require(
                self.read()["revision"] == expected,
                "These settings changed in another window. Reload them before saving.",
            )
            write_json(self.root / "setup.json", value)
            return {"draft": value, "revision": digest(value)}


def import_configuration(registry, server):
    """Expose editable fields even when a legacy runtime no longer validates."""
    root = private_directory(registry)
    config = read_json(root / "config.json")
    if not server and (root / "heartbeat.json").exists():
        server = read_json(root / "heartbeat.json").get("host", "")
    require(
        config.get("schema") == 1 and isinstance(config.get("profiles"), dict),
        "This folder does not contain HStream cluster settings",
    )
    profiles = {}
    issues = []
    for key, profile in config["profiles"].items():
        if profile.get("executor") != "slurm":
            continue
        resource, runtime = profile["resources"], profile["runtime"]
        profiles[key] = {
            "label": profile.get("label", key),
            **{
                field: resource[field]
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
            **profile["limits"],
            "runtime_root": runtime.get("root", ""),
        }
        if not runtime.get("worker"):
            issues.append(
                {
                    "profile": key,
                    "message": "This profile was saved by an older HStream version. Deployment updates the programs, but saved profiles keep their installation folders. Choose the current folder in Advanced profile settings, then build and test the setup again.",
                }
            )
    admission = config["profiles"].get(config.get("admission_profile"), {})
    command = admission.get("task_command", [])
    controller_root = Path(command[0]).parent if command else None
    if controller_root and controller_root.name == "bin":
        controller_root = controller_root.parent
    require(
        controller_root is not None,
        "The old settings have no main-server HStream installation",
    )
    value = validate_draft(
        {
            "schema": 1,
            "server": server,
            "registry": str(root),
            "game_roots": config["allowed_game_roots"],
            "profiles": profiles,
            "observer_registry": config["observer_registry"],
            "controller_runtime": str(controller_root),
        }
    )
    return {"draft": value, "issues": issues, "configuration_revision": digest(config)}


def readable_error(value):
    """Unwrap transported CLI failures without displaying JSON as the message."""
    message = str(value).strip()
    for _ in range(4):
        try:
            decoded = json.loads(message)
        except ValueError:
            break
        if not isinstance(decoded, dict) or not isinstance(decoded.get("error"), str):
            break
        message = decoded["error"].strip()
    if message.startswith("Slurm profiles need a pinned runtime.worker"):
        return (
            "These saved computer profiles need to be updated for the current HStream job worker. "
            "Running make deploy installs the programs; it does not rebuild saved profiles.\n"
            "Recover the saved setup, check the installation folders, then use Save and build registry and Test next computer. "
            "Apply tested settings before starting the controller."
        )
    return message


def run_readonly(argv, timeout):
    """Time-limited inspection with bounded returned output and no prompts."""
    positive(timeout, "Connection timeout", 300)
    with tempfile.TemporaryFile() as out, tempfile.NamedTemporaryFile() as err:
        process = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err
        )
        deadline = time.monotonic() + timeout
        pending = b""
        diagnostics = []
        try:
            with open(err.name, "rb") as reader:
                while True:
                    finished = process.poll() is not None
                    require(
                        os.fstat(out.fileno()).st_size <= 1024 * 1024
                        and os.fstat(err.fileno()).st_size <= 1024 * 1024,
                        "Connection response is too large",
                    )
                    pending += reader.read(1024 * 1024 + 1)
                    require(
                        reader.tell() <= 1024 * 1024, "Connection response is too large"
                    )
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        if not forward(line.decode(errors="replace")):
                            diagnostics.append(line)
                    if finished:
                        break
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise WorkflowError(
                            "The server did not respond in time. Check its name and network connection."
                        )
                    try:
                        process.wait(timeout=min(0.1, remaining))
                    except subprocess.TimeoutExpired:
                        pass
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        diagnostics.append(pending)
        out.seek(0)
        output = out.read(1024 * 1024 + 1)
        require(len(output) <= 1024 * 1024, "Connection response is too large")
        stdout, stderr = output.decode(errors="replace"), b"\n".join(
            diagnostics
        ).decode(errors="replace")
    if process.returncode:
        raise WorkflowError(
            readable_error(
                stderr.strip() or stdout.strip() or "The connection test failed"
            )[:4096]
        )
    return stdout


# Executed on the named submission host. It only runs fixed inspection commands.
# Paths and server names are never interpolated into its source.
PROBE = r"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path
deadline = time.monotonic() + int(sys.argv[1])
def command(args):
    try:
        p = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=max(0, deadline-time.monotonic()))
        return {"ok": p.returncode == 0, "text": (p.stdout if p.returncode == 0 else p.stderr)[:131072]}
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"ok": False, "text": str(e)}
configuration = command(["scontrol", "show", "config"])
cluster = re.search(r"^ClusterName\s*=\s*([A-Za-z0-9_.-]+)\s*$", configuration["text"], re.MULTILINE)
cluster_args = ["--clusters=" + cluster.group(1)] if cluster else []
units = command(["systemctl", "--user", "list-unit-files", "--type=service", "--no-legend", "hstream-workflow-*.service"])
managed = []
if units["ok"]:
    names = re.findall(r"\bhstream-workflow-[a-f0-9]{16}\.service\b", units["text"])
    for name in sorted(set(names))[:64]:
        service = command(["systemctl", "--user", "show", name, "--property=ActiveState,ExecStart"])
        try:
            with (Path.home() / ".config/systemd/user" / name).open() as source:
                first = source.readline(8192)
            if first.startswith("# HStream registry: "):
                value = json.loads(first.removeprefix("# HStream registry: "))
                if isinstance(value, str) and value.startswith("/"):
                    service["registry"] = value
        except (OSError, ValueError):
            pass
        managed.append(service)
worker, task = shutil.which("hstream-runtime-worker"), shutil.which("hstream-experiment-task")
installed_runtime = None
if worker and task:
    try:
        worker, task = str(Path(worker).resolve(strict=True)), str(Path(task).resolve(strict=True))
        programs = [Path(worker), Path(task)]
        valid = all(path.is_file() and os.access(path, os.X_OK) for path in programs)
        if valid:
            for path in programs:
                with path.open("rb") as source:
                    valid = valid and source.read(4) == b"\x7fELF"
        if valid and programs[0].parent == programs[1].parent:
            directory = programs[0].parent
            installed_runtime = str(directory.parent if directory.name == "bin" else directory)
    except OSError:
        pass
print(json.dumps({
    "host": socket.gethostname(), "uid": os.getuid(), "home": os.path.expanduser("~"),
    "slurm": configuration,
    "machines": command(["sinfo", "--noheader", "--Node", "--format=%N|%P|%G|%c|%m|%T"]),
    "queue": command(["squeue", "--noheader", "--format=%i", *cluster_args]),
    "accounting": command(["sacct", "--noheader", "--allocations", "--starttime=now", "--format=JobID", *cluster_args]),
    "controller": command(["systemctl", "show", "hstream-workflow-controller", "--property=ActiveState,ExecStart"]),
    "managed_controllers": managed,
    "worker": worker, "task": task, "installed_runtime": installed_runtime
}))
"""


def discover(server, timeout):
    require(
        isinstance(server, str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,252}", server),
        "Enter a server name, such as ripper",
    )
    argv = [
        "ssh",
        "-oBatchMode=yes",
        "-oStrictHostKeyChecking=yes",
        "-oConnectTimeout=" + str(timeout),
        "--",
        server,
        shlex.join(["python3", "-c", PROBE, str(timeout)]),
    ]
    try:
        raw = json.loads(run_readonly(argv, timeout))
    except WorkflowError as error:
        return {
            "ready": False,
            "checks": [
                {
                    "name": "Server connection",
                    "ok": False,
                    "message": str(error),
                    "next": "Check the server name. Sign in to this server with SSH once to confirm its identity and set up access.",
                }
            ],
        }
    checks = [
        {
            "name": "Server connection",
            "ok": True,
            "message": "Connected to " + raw["host"],
        }
    ]
    installed_runtime = raw.get("installed_runtime")
    checks.append(
        {
            "name": "HStream on the main server",
            "ok": bool(installed_runtime),
            "message": (
                "Installed programs found in " + installed_runtime
                if installed_runtime
                else "Could not find the installed HStream job programs on this server."
            ),
            "next": "Check that make deploy succeeded for this server, then run this connection check again.",
        }
    )
    for key, name in (
        ("slurm", "Job scheduler"),
        ("queue", "Cluster connection"),
        ("accounting", "Job history"),
        ("machines", "Available computers"),
    ):
        result = raw[key]
        checks.append(
            {
                "name": name,
                "ok": result["ok"],
                "message": "Available" if result["ok"] else result["text"],
                "next": "Check the existing Slurm installation on this server.",
            }
        )
    config = raw["slurm"]["text"]
    containment = bool(
        re.search(r"^ProctrackType\s*=\s*proctrack/cgroup\s*$", config, re.MULTILINE)
        and re.search(r"^TaskPlugin\s*=.*task/cgroup", config, re.MULTILINE)
    )
    checks.append(
        {
            "name": "Job cleanup",
            "ok": containment,
            "message": (
                "Slurm contains each job's processes"
                if containment
                else "Slurm's cgroup process containment is missing"
            ),
            "next": "Ask the cluster administrator to enable proctrack/cgroup and task/cgroup before using HStream workflows.",
        }
    )
    machines = []
    if raw["machines"]["ok"]:
        for line in raw["machines"]["text"].splitlines():
            fields = line.split("|")
            if len(fields) == 6:
                machines.append(
                    dict(
                        zip(
                            ("name", "partition", "gpu", "cpus", "memory_mb", "state"),
                            fields,
                        )
                    )
                )
    services = [raw["controller"], *raw.get("managed_controllers", [])]
    paths = set()
    for service in services:
        if service.get("registry"):
            paths.add(service["registry"])
            continue
        if service["ok"]:
            match = re.search(
                r"""--registry\s+("(?:\\.|[^"\\])*"|'[^']*'|[^\s;]+)""", service["text"]
            )
            if match:
                paths.add(shlex.split(match.group(1))[0])
    paths = sorted(paths)
    return {
        "ready": False,
        "checks": checks,
        "host": raw["host"],
        "home": raw["home"],
        "uid": raw["uid"],
        "machines": machines,
        "service_registry": paths[0] if len(paths) == 1 else None,
        "service_registries": paths,
        "controller_active": any(
            "ActiveState=active" in service["text"] for service in services
        ),
        "worker": raw["worker"],
        "task": raw["task"],
        "installed_runtime": installed_runtime,
        "next": (
            "Found several saved setups. Choose a folder under Storage, software and dashboard settings, then Recover saved setup:\n"
            + "\n".join(paths)
            if len(paths) > 1
            else "Connection checked. Select the machines to use, then test HStream with a sample recording."
        ),
    }


def inspect_registry(registry, timeout, draft=None):
    from .dispatch import installed_launcher

    checks = []
    try:
        config = load_config(registry)
        require(
            not (Path(registry) / "setup-test-only.json").exists(),
            "This is a test folder. Use the main settings folder after applying the tested setup.",
        )
        if draft is not None:
            expected = import_configuration(registry, draft["server"])["draft"]
            chosen = validate_draft(draft)
            active_path = Path(registry) / "setup-active.json"
            active = read_json(active_path) if active_path.exists() else {}
            # An applied revision binds the user's original installation aliases
            # to the canonical, qualified runtime pins. Do not resolve aliases
            # anew on a possibly different desktop OS.
            applied = active.get("revision") == digest(chosen) and active.get(
                "configuration_revision"
            ) == digest(config)
            for value in (chosen, expected):
                for field in ("registry", "controller_runtime", "observer_registry"):
                    if value[field] is not None:
                        value[field] = str(Path(value[field]))
                value["game_roots"] = [str(Path(path)) for path in value["game_roots"]]
                for profile in value["profiles"].values():
                    profile["runtime_root"] = str(Path(profile["runtime_root"]))
            require(
                applied or chosen == expected,
                "The choices in this window differ from the installed settings. Build, test and apply them, or recover the installed setup.",
            )
        checks.append(
            {
                "name": "Saved settings",
                "ok": True,
                "message": "Settings use the current format",
            }
        )
        installed_launcher(config)
        checks.append(
            {
                "name": "HStream controller",
                "ok": True,
                "message": "Installed controller files match these settings",
            }
        )
        heartbeat = read_json(Path(registry) / "heartbeat.json")
        require(
            heartbeat.get("configuration_digest") == digest(config),
            "The controller has not loaded these settings. Restart it on the main server, then check again.",
        )
        from .processes import host_identity, same_process

        if heartbeat.get("host") == host_identity()["host"]:
            require(
                same_process(heartbeat),
                "The controller process stopped. Start the controller and check again.",
            )
        age = (time.time_ns() - heartbeat["time_ns"]) / 1e9
        require(
            0 <= age <= timeout,
            "The HStream controller is not reporting progress. Start its service on the main server.",
        )
        checks.append(
            {
                "name": "Controller service",
                "ok": True,
                "message": "Responding on "
                + str(heartbeat.get("host", "unknown server")),
            }
        )
    except (OSError, ValueError, KeyError) as error:
        checks.append(
            {"name": "Cluster readiness", "ok": False, "message": readable_error(error)}
        )
    return {
        "ready": bool(checks) and all(item["ok"] for item in checks),
        "checks": checks,
    }


def progress(root):
    """Advisory button prerequisites from saved files, never job admission."""
    from .setup_execution import current_revision, generation

    state = {
        "revision": None,
        "has_build": (root / "setup-build.json").exists(),
        "build_current": False,
        "applied": False,
        "installed": (root / "config.json").exists(),
    }
    if not root.exists():
        return state
    try:
        saved = SetupStore(root).read()
        state["revision"] = saved["revision"]
        built_revision = None
        if state["has_build"]:
            _, built = generation(root)
            state["build_current"] = (
                bool(saved["draft"]) and built["revision"] == saved["revision"]
            )
            if state["build_current"]:
                built_revision = digest(built["configuration"])
        if (root / "setup-active.json").exists():
            active = read_json(root / "setup-active.json")
            state["applied"] = (
                bool(saved["draft"])
                and active["revision"] == saved["revision"]
                and active["configuration_revision"] == current_revision(root)
                and (
                    built_revision is None
                    or active["configuration_revision"] == built_revision
                )
            )
    except (OSError, ValueError, KeyError, TypeError) as error:
        state["error"] = readable_error(error)
    return state


def handle_request(request):
    result = _handle_request(request)
    if request.get("action") != "discover":
        location = (
            result["draft"]["registry"] if result.get("draft") else request["registry"]
        )
        result["progress"] = progress(absolute(location, "Settings folder"))
    return result


def _handle_request(request):
    """The desktop and operator UI use the same typed setup operations."""
    require(isinstance(request, dict), "Expected setup request")
    action = request.get("action")
    if action == "prepare-runtime":
        from .setup_runtime import on_node

        return on_node(request)
    if action == "discover":
        return discover(request["server"], request["timeout_seconds"])
    if action == "inspect":
        return inspect_registry(
            absolute(request["registry"], "Settings folder"),
            request["timeout_seconds"],
            request.get("draft"),
        )
    if action == "load":
        path = absolute(request["registry"], "Settings folder")
        if not path.exists():
            return {"draft": None, "revision": None}
        from .setup_storage import recovered

        return recovered(path)
    if action == "save":
        return SetupStore(request["registry"]).save(
            request["draft"], request["revision"]
        )
    if action == "import":
        return import_configuration(request["registry"], request["server"])
    if action in {
        "build",
        "start-test",
        "test-status",
        "cancel-test",
        "activate",
        "start-service",
        "stop-service",
    }:
        from .setup_execution import execute, remote

        if request.get("on_server") is True:
            return execute(request)
        return remote(request)
    raise WorkflowError("Unknown setup action")
