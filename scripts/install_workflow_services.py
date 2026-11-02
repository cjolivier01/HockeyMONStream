#!/usr/bin/env python3
"""Install independently supervised workflows without replacing an existing cluster.

The JSON deployment manifest supplies site values. `check` performs no writes;
`integrate` never writes Slurm/Munge configuration; `bootstrap` requires explicit,
hash-bound cluster configuration. This is an operator command, run as root for
changes. See docs/workflow-deployment.md.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import pwd
import re
import stat
import subprocess
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path

SUPPORTED_DAGSTER = "1.13.26"
MAX_CONFIG_BYTES = 4 * 1024 * 1024
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}\Z")
SLURM_FILES = {"slurm.conf", "gres.conf", "cgroup.conf", "slurmdbd.conf"}


class InstallError(RuntimeError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_regular(path: Path, limit=MAX_CONFIG_BYTES, *, private=False) -> bytes:
    """Pin a regular file and reject symlink parents, growth and concurrent writes."""
    path = path.absolute()
    descriptor = os.open(path.anchor, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for component in path.parts[1:-1]:
            child = os.open(
                component,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=descriptor,
            )
            os.close(descriptor)
            descriptor = child
        handle = os.open(
            path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=descriptor
        )
        try:
            before = os.fstat(handle)
            if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
                raise InstallError(f"Expected a bounded regular file: {path}")
            if private and before.st_mode & 0o077:
                raise InstallError(f"Secret file must be owner-only: {path}")
            chunks = []
            remaining = limit + 1
            while remaining:
                chunk = os.read(handle, min(remaining, 65536))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            after = os.fstat(handle)
            final = os.stat(path.name, dir_fd=descriptor, follow_symlinks=False)
            identity = lambda entry: (
                entry.st_dev,
                entry.st_ino,
                entry.st_size,
                entry.st_mtime_ns,
                entry.st_ctime_ns,
            )
            if (
                remaining == 0
                or identity(before) != identity(after)
                or identity(after) != identity(final)
            ):
                raise InstallError(f"File changed while reading: {path}")
            return b"".join(chunks)
        finally:
            os.close(handle)
    finally:
        os.close(descriptor)


def object_keys(value, required, optional=()):
    if (
        not isinstance(value, dict)
        or set(value) - set(required) - set(optional)
        or set(required) - set(value)
    ):
        raise InstallError(
            f"Expected object keys {sorted(required)}; optional {sorted(optional)}"
        )


def absolute(value):
    if (
        not isinstance(value, str)
        or not value.startswith("/")
        or any(c in value for c in "\n\r\0%")
    ):
        raise InstallError(
            "Deployment paths must be absolute, without control characters or systemd specifiers"
        )
    if ".." in Path(value).parts or len(value) > 4096:
        raise InstallError("Invalid deployment path")
    return value


def positive(value, name, maximum=2**31 - 1):
    if type(value) is not int or not 0 < value <= maximum:
        raise InstallError(
            f"{name} must be a positive integer no larger than {maximum}"
        )
    return value


def name(value):
    if not isinstance(value, str) or not SAFE_NAME.fullmatch(value):
        raise InstallError("Invalid deployment or service name")
    return value


def source_spec(value):
    object_keys(value, {"path", "sha256"})
    absolute(value["path"])
    if not isinstance(value["sha256"], str) or not re.fullmatch(
        r"[a-f0-9]{64}", value["sha256"]
    ):
        raise InstallError("Source files require an explicit SHA-256 digest")


def validate_manifest(manifest):
    object_keys(
        manifest,
        {
            "schema",
            "id",
            "identity",
            "state_dir",
            "shared_paths",
            "slurm",
            "workflow",
            "dagster",
            "adopt",
            "health_timeout_seconds",
        },
    )
    if manifest["schema"] != 1:
        raise InstallError("Unsupported deployment manifest schema")
    name(manifest["id"])
    positive(manifest["health_timeout_seconds"], "service health timeout", 3600)
    absolute(manifest["state_dir"])
    identity = manifest["identity"]
    object_keys(identity, {"user", "uid", "gid"})
    name(identity["user"])
    positive(identity["uid"], "service UID")
    positive(identity["gid"], "service GID")
    if not isinstance(manifest["shared_paths"], list) or not manifest["shared_paths"]:
        raise InstallError("Explicit shared paths are required")
    for path in manifest["shared_paths"]:
        absolute(path)
    slurm = manifest["slurm"]
    object_keys(slurm, {"cluster", "roles", "config_dir", "packages", "bootstrap"})
    name(slurm["cluster"])
    absolute(slurm["config_dir"])
    if (
        not isinstance(slurm["roles"], list)
        or not slurm["roles"]
        or set(slurm["roles"]) - {"controller", "compute", "submission"}
    ):
        raise InstallError(
            "Slurm roles must explicitly select controller, compute or submission"
        )
    if not isinstance(slurm["packages"], dict):
        raise InstallError("Slurm packages must map exact package names to versions")
    for package, version in slurm["packages"].items():
        if (
            not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", package)
            or not isinstance(version, str)
            or not re.fullmatch(r"[A-Za-z0-9.+:~_-]+", version)
        ):
            raise InstallError("Invalid pinned package specification")
    bootstrap = slurm["bootstrap"]
    if bootstrap is not None:
        object_keys(bootstrap, {"files", "munge_key", "identities"})
        if (
            not isinstance(bootstrap["files"], dict)
            or set(bootstrap["files"]) - SLURM_FILES
            or not {"slurm.conf", "cgroup.conf"} <= set(bootstrap["files"])
        ):
            raise InstallError("Bootstrap needs explicit slurm.conf and cgroup.conf")
        for value in bootstrap["files"].values():
            source_spec(value)
        source_spec(bootstrap["munge_key"])
        if not isinstance(bootstrap["identities"], dict) or set(
            bootstrap["identities"]
        ) != {"slurm", "munge"}:
            raise InstallError(
                "Bootstrap requires explicit Slurm and Munge UID/GID identity"
            )
        for value in bootstrap["identities"].values():
            object_keys(value, {"uid", "gid"})
            positive(value["uid"], "cluster UID")
            positive(value["gid"], "cluster GID")
    workflow = manifest["workflow"]
    if workflow is not None:
        object_keys(
            workflow,
            {
                "kind",
                "unit",
                "python",
                "code_dir",
                "registry",
                "registry_config",
                "environment_file",
                "handover",
            },
        )
        if workflow["kind"] not in {"standalone", "web"}:
            raise InstallError("Workflow kind must be standalone or web")
        name(workflow["unit"])
        for key in ("python", "code_dir", "registry"):
            absolute(workflow[key])
        if workflow["environment_file"] is not None:
            absolute(workflow["environment_file"])
        if workflow["kind"] == "standalone":
            source_spec(workflow["registry_config"])
        elif workflow["registry_config"] is not None:
            raise InstallError(
                "Web workers use their existing environment, not a standalone registry config"
            )
        if workflow["handover"] is not None:
            object_keys(
                workflow["handover"],
                {
                    "lock",
                    "receipt",
                    "location",
                    "repository",
                    "schedule",
                    "database_identity",
                },
            )
            if workflow["kind"] != "web":
                raise InstallError("Dispatcher handover is only valid for a web worker")
            for key in ("lock", "receipt"):
                absolute(workflow["handover"][key])
            for key in ("location", "repository", "schedule"):
                if not isinstance(workflow["handover"][key], str) or not re.fullmatch(
                    r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", workflow["handover"][key]
                ):
                    raise InstallError("Invalid exact Dagster schedule selector")
            if not re.fullmatch(
                r"[a-f0-9]{64}", workflow["handover"]["database_identity"]
            ):
                raise InstallError(
                    "Handover requires the exact web database identity digest"
                )
    dagster = manifest["dagster"]
    if dagster is not None:
        object_keys(
            dagster,
            {
                "version",
                "python",
                "venv",
                "home",
                "code_dir",
                "registry",
                "registry_config",
                "instance_config",
                "workspace_config",
                "port",
                "install_packages",
            },
        )
        if dagster["version"] != SUPPORTED_DAGSTER:
            raise InstallError(f"Qualified Dagster version is {SUPPORTED_DAGSTER}")
        for key in ("python", "venv", "home", "code_dir", "registry"):
            absolute(dagster[key])
        source_spec(dagster["registry_config"])
        source_spec(dagster["instance_config"])
        source_spec(dagster["workspace_config"])
        positive(dagster["port"], "Dagster port", 65535)
        if type(dagster["install_packages"]) is not bool:
            raise InstallError("install_packages must be boolean")
        if workflow is None:
            raise InstallError(
                "Dagster observation requires an independently supervised workflow owner"
            )
    if not isinstance(manifest["adopt"], dict):
        raise InstallError(
            "adopt must map existing file paths to their expected digests"
        )
    for path, sha in manifest["adopt"].items():
        source_spec({"path": path, "sha256": sha})
    return manifest


def command(argv, *, timeout=60):
    try:
        return subprocess.run(
            argv, check=True, text=True, capture_output=True, timeout=timeout
        )
    except subprocess.CalledProcessError as error:
        # Child diagnostics can contain environment or database secrets. The
        # operator can inspect protected service logs; never echo arbitrary output.
        raise InstallError(
            f"Command failed ({error.returncode}): {argv[0]} {argv[1] if len(argv) > 1 else ''}"
        ) from error
    except (OSError, subprocess.TimeoutExpired) as error:
        raise InstallError(f"Command unavailable or timed out: {argv[0]}") from error


def unit_argument(value):
    """systemd ExecStart quoting, not shell quoting."""
    if any(c in value for c in "\n\r\0"):
        raise InstallError("Invalid unit argument")
    return (
        '"'
        + value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("%", "%%")
        .replace("$", "$$")
        + '"'
    )


def render_unit(
    description,
    identity,
    directory,
    argv,
    environment=(),
    environment_file=None,
    *,
    independent_attempts=False,
):
    lines = [
        "[Unit]",
        f"Description={description}",
        "After=network-online.target",
        "Wants=network-online.target",
        "",
        "[Service]",
        "Type=simple",
        f"User={identity['user']}",
        f"Group={identity['gid']}",
        f"WorkingDirectory={unit_argument(directory)}",
        "UMask=0077",
        "Restart=on-failure",
        "RestartSec=5",
        "TimeoutStopSec=300",
        # Attempt supervisors own descendant shutdown and completion receipts.
        # A dispatcher restart must not kill their inherited service cgroup.
        "KillMode=process" if independent_attempts else "KillMode=mixed",
    ]
    lines += ["Environment=" + unit_argument(value) for value in environment]
    if environment_file:
        lines.append("EnvironmentFile=" + unit_argument(environment_file))
    lines += [
        "ExecStart=" + " ".join(unit_argument(value) for value in argv),
        "",
        "[Install]",
        "WantedBy=multi-user.target",
        "",
    ]
    return "\n".join(lines).encode()


def slurm_settings(data):
    settings = {}
    for line in data.decode().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        for match in re.finditer(
            r"(?:^|\s)([A-Za-z][A-Za-z0-9]*)\s*=\s*([^\s]+)", line
        ):
            settings.setdefault(match[1], []).append(match[2])
    return settings


def validate_slurm_configuration(files, cluster, roles):
    config = slurm_settings(files["slurm.conf"])
    required = {
        "ClusterName": cluster,
        "ProctrackType": "proctrack/cgroup",
        "AccountingStorageType": "accounting_storage/slurmdbd",
        "SelectType": "select/cons_tres",
    }
    for key, value in required.items():
        if config.get(key) != [value]:
            raise InstallError(f"slurm.conf must specify {key}={value}")
    if "task/cgroup" not in ",".join(config.get("TaskPlugin", [])).split(","):
        raise InstallError("Slurm task/cgroup containment is required")
    for key in (
        "SlurmctldHost",
        "AccountingStorageHost",
        "NodeName",
        "PartitionName",
        "StateSaveLocation",
        "SlurmdSpoolDir",
    ):
        if not config.get(key):
            raise InstallError(f"Explicit Slurm {key} is required")
    if any(
        not any(token.startswith("Gres=") for token in line.split())
        for line in files["slurm.conf"].decode().splitlines()
        if line.strip().startswith("NodeName=")
    ):
        raise InstallError("Each bootstrap node needs explicit GRES inventory")
    cgroup = slurm_settings(files["cgroup.conf"])
    for key in ("ConstrainDevices", "ConstrainCores", "ConstrainRAMSpace"):
        if cgroup.get(key) not in (["yes"], ["YES"], ["true"]):
            raise InstallError(f"cgroup.conf requires {key}=yes")
    if "compute" in roles and "gres.conf" not in files:
        raise InstallError("Compute bootstrap requires explicit local gres.conf")
    if "controller" in roles:
        if "slurmdbd.conf" not in files:
            raise InstallError(
                "Controller bootstrap requires protected slurmdbd.conf and existing accounting database"
            )
        accounting = slurm_settings(files["slurmdbd.conf"])
        for key in (
            "StorageType",
            "StorageHost",
            "StorageLoc",
            "StorageUser",
            "StoragePass",
        ):
            if not accounting.get(key):
                raise InstallError(f"Accounting configuration requires {key}")


class Installer:
    """Testable host adapter. The CLI always uses the real root directory."""

    def __init__(
        self, manifest, *, root=Path("/"), run=command, source=Path(__file__).parent
    ):
        self.manifest = validate_manifest(manifest)
        self.root = Path(root)
        self.run = run
        self.source = source
        self.state = self.path(manifest["state_dir"]) / manifest["id"]
        self.uid = manifest["identity"]["uid"]
        self.gid = manifest["identity"]["gid"]
        self.handover_token = None

    def path(self, value):
        return self.root / absolute(str(value)).lstrip("/")

    def sourced(self, spec, *, private=False):
        data = read_regular(self.path(spec["path"]), private=private)
        if digest(data) != spec["sha256"]:
            raise InstallError("Source configuration digest changed")
        return data

    def atomic(self, path, data, mode=0o600, uid=None, gid=None):
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
        # Refuse a symlink ancestor even for a new final name.
        if path.parent.resolve() != path.parent.absolute():
            raise InstallError(f"Managed directory contains a symlink: {path.parent}")
        fd, temporary = tempfile.mkstemp(prefix=".workflow-install-", dir=path.parent)
        try:
            os.fchmod(fd, mode)
            if self.root == Path("/"):
                os.fchown(fd, 0 if uid is None else uid, 0 if gid is None else gid)
            with os.fdopen(fd, "wb", closefd=True) as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, path)
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def managed_state(self):
        path = self.state / "files.json"
        if not path.exists():
            return {}
        value = json.loads(read_regular(path))
        if value.get("schema") != 1 or not isinstance(value.get("files"), dict):
            raise InstallError("Invalid managed installation state")
        return value["files"]

    def verify_identity(self):
        if self.root != Path("/"):
            return
        try:
            account = pwd.getpwnam(self.manifest["identity"]["user"])
        except KeyError as error:
            raise InstallError(
                "Create the explicit service identity before installation"
            ) from error
        if (account.pw_uid, account.pw_gid) != (self.uid, self.gid):
            raise InstallError("Service UID/GID differs from the deployment manifest")

    def check_slurm(self):
        slurm = self.manifest["slurm"]
        result = self.run(["scontrol", "show", "config"])
        settings = slurm_settings(result.stdout.encode())
        for key, expected in (
            ("ClusterName", slurm["cluster"]),
            ("ProctrackType", "proctrack/cgroup"),
            ("AccountingStorageType", "accounting_storage/slurmdbd"),
        ):
            if settings.get(key) != [expected]:
                raise InstallError(
                    f"Existing cluster does not satisfy {key}={expected}"
                )
        if "task/cgroup" not in ",".join(settings.get("TaskPlugin", [])).split(","):
            raise InstallError("Existing cluster lacks task/cgroup containment")
        ping = self.run(["scontrol", "ping"])
        if " is UP" not in ping.stdout:
            raise InstallError("Slurm controller is unavailable")
        self.run(
            [
                "sacct",
                "--noheader",
                "--parsable2",
                "--starttime=now-1hour",
                "--format=JobIDRaw,State",
                "--allocations",
            ]
        )
        self.run(["sinfo", "--noheader", "--Node", "--format=%N %P %t %G"])
        config_dir = self.path(slurm["config_dir"])
        cgroup = slurm_settings(read_regular(config_dir / "cgroup.conf"))
        if cgroup.get("ConstrainDevices") not in (["yes"], ["YES"], ["true"]):
            raise InstallError(
                "Cluster device containment requires ConstrainDevices=yes"
            )

    def check_shared(self):
        self.verify_identity()
        for value in self.manifest["shared_paths"]:
            path = self.path(value)
            if not path.is_dir() or path.resolve() != path.absolute():
                raise InstallError(f"Shared directory is missing or aliased: {value}")

    def check_dagster(self):
        dagster = self.manifest["dagster"]
        if dagster is None:
            return
        python = str(Path(dagster["venv"]) / "bin/python")
        found = self.run(
            [
                python,
                "-c",
                "import importlib.metadata as m; print(m.version('dagster')); print(m.version('dagster-webserver'))",
            ]
        )
        if found.stdout.splitlines() != [SUPPORTED_DAGSTER, SUPPORTED_DAGSTER]:
            raise InstallError("Dagster package versions differ from the qualified pin")
        # Validation that starts a code server belongs to activation preflight.
        # This read-only check asks the already-running webserver for its loaded
        # repository and daemon health rather than creating instance state.
        query = "{ repositoriesOrError { __typename ... on RepositoryConnection { nodes { location { name } } } } instance { daemonHealth { allDaemonStatuses { required healthy } } } }"
        request = urllib.request.Request(
            f"http://127.0.0.1:{dagster['port']}/graphql",
            data=json.dumps({"query": query}).encode(),
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(request, timeout=15) as response:
            payload = json.loads(response.read(MAX_CONFIG_BYTES + 1))
        if payload.get("errors"):
            raise InstallError("Dagster GraphQL health query failed")
        data = payload["data"]
        repositories = data["repositoriesOrError"]
        if repositories.get("__typename") != "RepositoryConnection" or not any(
            node["location"]["name"] == "hstream_workflows"
            for node in repositories["nodes"]
        ):
            raise InstallError("Dagster workflow observer code location is not loaded")
        statuses = data["instance"]["daemonHealth"]["allDaemonStatuses"]
        if not statuses or any(
            not item["healthy"] for item in statuses if item["required"]
        ):
            raise InstallError("Dagster required daemon heartbeat is unhealthy")

    def expected_files(self, mode):
        """Return bytes/mode/owner; nothing is written before the full set validates."""
        manifest = self.manifest
        files = {}
        owner = (self.uid, self.gid)
        add = lambda path, data, permissions=0o644, identity=(0, 0): files.__setitem__(
            absolute(str(path)), (data, permissions, *identity)
        )
        workflow = manifest["workflow"]
        if workflow:
            if workflow["kind"] == "standalone":
                config = self.sourced(workflow["registry_config"])
                value = json.loads(config)
                if value.get("schema") != 1 or not {
                    "defaults",
                    "profiles",
                    "admission_profile",
                    "allowed_game_roots",
                    "observer_registry",
                } <= set(value):
                    raise InstallError("Invalid standalone registry configuration")
                admission = value["profiles"].get(value["admission_profile"])
                if (
                    not isinstance(admission, dict)
                    or admission.get("executor") != "local"
                    or admission.get("resources", {}).get("gpus") != 0
                    or admission.get("runtime", {}).get("backend") != "cpu"
                ):
                    raise InstallError(
                        "admission_profile must select a local CPU helper qualified for this controller host"
                    )
                add(Path(workflow["registry"]) / "config.json", config, 0o600, owner)
                argv = [
                    workflow["python"],
                    "-m",
                    "hstream_workflow.cli",
                    "serve",
                    "--registry",
                    workflow["registry"],
                ]
            else:
                argv = [workflow["python"], "manage.py", "stream_worker"]
            environment = []
            if workflow["kind"] == "web" and manifest["dagster"]:
                environment.extend(
                    [
                        "PYTHONPATH=" + manifest["dagster"]["code_dir"],
                        "HSTREAM_DAGSTER_REGISTRY=" + manifest["dagster"]["registry"],
                    ]
                )
            if workflow["handover"]:
                intent_file = self.state / "handover.json"
                intent = (
                    json.loads(read_regular(intent_file))
                    if intent_file.exists()
                    else None
                )
                self.handover_token = intent["token"] if intent else uuid.uuid4().hex
                environment += [
                    "HSTREAM_DISPATCH_HANDOVER_RECEIPT="
                    + workflow["handover"]["receipt"],
                    "HSTREAM_DISPATCH_HANDOVER_TOKEN=" + self.handover_token,
                ]
            add(
                f"/etc/systemd/system/{workflow['unit']}.service",
                render_unit(
                    "HStream durable workflow controller",
                    manifest["identity"],
                    workflow["code_dir"],
                    argv,
                    environment,
                    workflow["environment_file"],
                    independent_attempts=True,
                ),
            )
        dagster = manifest["dagster"]
        if dagster:
            config = self.sourced(dagster["registry_config"])
            add(Path(dagster["registry"]) / "config.json", config, 0o600, owner)
            package_source = self.source / "hstream_workflow"
            if not package_source.is_dir():
                raise InstallError(
                    "Workflow Python package must be installed beside this script"
                )
            for source in sorted(package_source.glob("*.py")):
                add(
                    Path(dagster["code_dir"]) / "hstream_workflow" / source.name,
                    read_regular(source),
                )
            definition = "from hstream_workflow.dagster_observer import load_definitions\n\ndefs = load_definitions()\n"
            add(
                Path(dagster["code_dir"]) / "workflow_definitions.py",
                definition.encode(),
            )
            # Operator supplies the merged workspace and original instance
            # storage settings. Never silently switch an existing PostgreSQL
            # instance to empty SQLite history or drop unrelated code locations.
            add(
                Path(dagster["code_dir"]) / "workspace.yaml",
                self.sourced(dagster["workspace_config"]),
            )
            home = Path(dagster["home"])
            add(
                home / "dagster.yaml",
                self.sourced(dagster["instance_config"]),
                0o600,
                owner,
            )
            environment = [
                "DAGSTER_HOME=" + dagster["home"],
                "HSTREAM_DAGSTER_REGISTRY=" + dagster["registry"],
                "PYTHONUNBUFFERED=1",
            ]
            for unit, executable, args in (
                (
                    "dagster-webserver",
                    "dagster-webserver",
                    ["-h", "127.0.0.1", "-p", str(dagster["port"])],
                ),
                ("dagster-daemon", "dagster-daemon", ["run"]),
            ):
                argv = [
                    str(Path(dagster["venv"]) / "bin" / executable),
                    *args,
                    "-w",
                    str(Path(dagster["code_dir"]) / "workspace.yaml"),
                ]
                add(
                    f"/etc/systemd/system/{unit}.service",
                    render_unit(
                        "HStream Dagster read-only workflow observation",
                        manifest["identity"],
                        dagster["code_dir"],
                        argv,
                        environment,
                    ),
                )
            argv = [
                str(Path(dagster["venv"]) / "bin/python"),
                "-m",
                "hstream_workflow.dagster_registry",
                "serve",
                "--registry",
                dagster["registry"],
                "--url",
                f"http://127.0.0.1:{dagster['port']}",
            ]
            add(
                "/etc/systemd/system/hstream-dagster-observer.service",
                render_unit(
                    "HStream Dagster observation registration",
                    manifest["identity"],
                    dagster["code_dir"],
                    argv,
                    environment,
                ),
            )
        if mode == "bootstrap":
            bootstrap = manifest["slurm"]["bootstrap"]
            if bootstrap is None:
                raise InstallError(
                    "Bootstrap requires explicit topology, GPU inventory, Munge and accounting inputs"
                )
            configs = {
                key: self.sourced(value, private=key == "slurmdbd.conf")
                for key, value in bootstrap["files"].items()
            }
            validate_slurm_configuration(
                configs, manifest["slurm"]["cluster"], manifest["slurm"]["roles"]
            )
            for key, data in configs.items():
                account = bootstrap["identities"]["slurm"]
                add(
                    Path(manifest["slurm"]["config_dir"]) / key,
                    data,
                    0o600 if key == "slurmdbd.conf" else 0o644,
                    (account["uid"], account["gid"]),
                )
            key = self.sourced(bootstrap["munge_key"], private=True)
            if not 32 <= len(key) <= 4096:
                raise InstallError("Munge key must contain 32–4096 bytes")
            identity = bootstrap["identities"]["munge"]
            add("/etc/munge/munge.key", key, 0o400, (identity["uid"], identity["gid"]))
        return files

    def services(self, mode):
        names = []
        if mode == "bootstrap":
            names.append("munge")
            roles = self.manifest["slurm"]["roles"]
            if "controller" in roles:
                names += ["slurmdbd", "slurmctld"]
            if "compute" in roles:
                names.append("slurmd")
        if self.manifest["workflow"]:
            names.append(self.manifest["workflow"]["unit"])
        if self.manifest["dagster"]:
            names += ["dagster-webserver", "dagster-daemon", "hstream-dagster-observer"]
        return names

    def preflight_configuration(self, mode):
        dagster = self.manifest["dagster"]
        if dagster:
            executable = str(Path(dagster["venv"]) / "bin/dagster")
            environment = [
                "DAGSTER_HOME=" + dagster["home"],
                "HSTREAM_DAGSTER_REGISTRY=" + dagster["registry"],
            ]
            prefix = [
                "runuser",
                "-u",
                self.manifest["identity"]["user"],
                "--",
                "env",
                *environment,
            ]
            self.run([*prefix, executable, "instance", "info"])
            self.run(
                [
                    *prefix,
                    executable,
                    "definitions",
                    "validate",
                    "-w",
                    str(Path(dagster["code_dir"]) / "workspace.yaml"),
                ]
            )
        if mode == "bootstrap" and "compute" in self.manifest["slurm"]["roles"]:
            self.run(
                [
                    "slurmd",
                    "-G",
                    "-f",
                    str(Path(self.manifest["slurm"]["config_dir"]) / "slurm.conf"),
                ]
            )

    def ensure_packages(self, mode):
        if mode == "bootstrap":
            release = read_regular(self.path("/etc/os-release")).decode()
            if not re.search(r"^ID=ubuntu$", release, re.MULTILINE) or not re.search(
                r'^VERSION_ID="(?:22\.04|24\.04)"$', release, re.MULTILINE
            ):
                raise InstallError(
                    "Cluster bootstrap is qualified only on Ubuntu 22.04 and 24.04"
                )
            packages = self.manifest["slurm"]["packages"]
            if not packages:
                raise InstallError(
                    "Bootstrap requires pinned distribution package versions"
                )
            self.run(
                [
                    "apt-get",
                    "install",
                    "--yes",
                    "--no-install-recommends",
                    *[f"{key}={value}" for key, value in sorted(packages.items())],
                ],
                timeout=1800,
            )
            for user, expected in self.manifest["slurm"]["bootstrap"][
                "identities"
            ].items():
                if self.root == Path("/"):
                    account = pwd.getpwnam(user)
                    if (account.pw_uid, account.pw_gid) != (
                        expected["uid"],
                        expected["gid"],
                    ):
                        raise InstallError(
                            f"Provision cluster-wide UID/GID for {user} before bootstrap"
                        )
        dagster = self.manifest["dagster"]
        if dagster and dagster["install_packages"]:
            venv = self.path(dagster["venv"])
            if venv.exists():
                # Never pip-upgrade the live environment; the operator selects a
                # new versioned venv for an upgrade and retains the previous one.
                result = self.run(
                    [
                        str(venv / "bin/python"),
                        "-c",
                        "import importlib.metadata as m; print(m.version('dagster')); print(m.version('dagster-webserver'))",
                    ]
                )
                if result.stdout.splitlines() != [SUPPORTED_DAGSTER, SUPPORTED_DAGSTER]:
                    raise InstallError(
                        "Existing venv differs; select a new empty versioned venv"
                    )
            else:
                self.run([dagster["python"], "-m", "venv", str(venv)], timeout=120)
                self.run(
                    [
                        str(venv / "bin/python"),
                        "-m",
                        "pip",
                        "install",
                        "--disable-pip-version-check",
                        f"dagster=={SUPPORTED_DAGSTER}",
                        f"dagster-webserver=={SUPPORTED_DAGSTER}",
                    ],
                    timeout=1800,
                )

    def check(self):
        self.check_shared()
        self.check_slurm()
        workflow = self.manifest["workflow"]
        if workflow:
            self.run(["systemctl", "is-active", "--quiet", workflow["unit"]])
            filename = (
                "heartbeat.json"
                if workflow["kind"] == "standalone"
                else "dispatcher-heartbeat.json"
            )
            heartbeat = json.loads(
                read_regular(self.path(workflow["registry"]) / filename)
            )
            process = self.run(
                ["systemctl", "show", "--property=MainPID", "--value", workflow["unit"]]
            ).stdout.strip()
            if (
                not process.isdigit()
                or int(process) == 0
                or heartbeat.get("pid") != int(process)
            ):
                raise InstallError(
                    "Workflow heartbeat is not from the current service process"
                )
            if (
                type(heartbeat.get("time_ns")) is not int
                or not 0
                <= time.time_ns() - heartbeat["time_ns"]
                <= self.manifest["health_timeout_seconds"] * 10**9
            ):
                raise InstallError(
                    "Workflow controller has not reconciled within the configured health bound"
                )
        self.check_dagster()
        return {"state": "healthy", "deployment": self.manifest["id"]}

    def begin_handover(self):
        workflow = self.manifest["workflow"]
        if not workflow or not workflow["handover"]:
            return
        handover = workflow["handover"]
        lock_path = self.path(handover["lock"])
        # The same inode used by tick(), never a second install-specific lock.
        try:
            descriptor = os.open(
                lock_path,
                os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK,
                0o600,
            )
            if self.root == Path("/"):
                os.fchown(descriptor, self.uid, self.gid)
        except FileExistsError:
            descriptor = os.open(lock_path, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            metadata = os.fstat(descriptor)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_mode & 0o022
                or (self.root == Path("/") and metadata.st_uid != self.uid)
            ):
                raise InstallError("Invalid dispatcher exclusion lock")
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            if (metadata.st_dev, metadata.st_ino) != (
                lock_path.stat().st_dev,
                lock_path.stat().st_ino,
            ):
                raise InstallError("Dispatcher lock changed during handover")
            intent = {
                "schema": 1,
                "token": self.handover_token,
                "state": "pending",
                "database_identity": handover["database_identity"],
                "data_root": workflow["registry"],
                "schedule": {
                    key: handover[key] for key in ("location", "repository", "schedule")
                },
            }
            existing = self.state / "handover.json"
            if existing.exists():
                old = json.loads(read_regular(existing))
                if any(
                    old.get(key) != intent[key]
                    for key in ("token", "database_identity", "data_root", "schedule")
                ):
                    raise InstallError(
                        "Existing handover belongs to another database or schedule"
                    )
                if old.get("state") == "complete":
                    return
            self.atomic(existing, (json.dumps(intent, indent=2) + "\n").encode())
        finally:
            os.close(descriptor)

    def finish_handover(self):
        workflow = self.manifest["workflow"]
        if not workflow or not workflow["handover"]:
            return
        from hstream_workflow.dagster_registry import graphql

        intent_file = self.state / "handover.json"
        intent = json.loads(read_regular(intent_file))
        if intent.get("state") == "complete":
            return
        handover = workflow["handover"]
        # Do not hold dispatcher.lock here: the new worker needs it to reconcile.
        receipt = None
        for _ in range(self.manifest["health_timeout_seconds"]):
            try:
                candidate = json.loads(read_regular(self.path(handover["receipt"])))
                if (
                    candidate.get("schema") == 1
                    and candidate.get("token") == self.handover_token
                    and candidate.get("database_identity")
                    == handover["database_identity"]
                    and candidate.get("data_root") == workflow["registry"]
                    and candidate.get("reconciled") is True
                ):
                    receipt = candidate
                    break
            except FileNotFoundError:
                pass
            if self.root == Path("/"):
                time.sleep(1)
        if receipt is None:
            raise InstallError(
                "New worker has not confirmed reconciliation; old Dagster schedule remains enabled"
            )
        self.run(["systemctl", "is-active", "--quiet", workflow["unit"]])
        process = self.run(
            ["systemctl", "show", "--property=MainPID", "--value", workflow["unit"]]
        ).stdout.strip()
        if (
            not process.isdigit()
            or int(process) == 0
            or receipt.get("pid") != int(process)
        ):
            raise InstallError(
                "Reconciliation receipt is not from the current worker process"
            )
        dagster = self.manifest["dagster"]
        if dagster is None:
            raise InstallError(
                "Legacy schedule handover requires the explicit Dagster deployment"
            )
        url = f"http://127.0.0.1:{dagster['port']}"
        selector = {
            "repositoryLocationName": handover["location"],
            "repositoryName": handover["repository"],
            "scheduleName": handover["schedule"],
        }
        query = "query($selector: ScheduleSelector!) { scheduleOrError(scheduleSelector: $selector) { __typename ... on Schedule { name pipelineName scheduleState { id status } } } }"
        schedule = graphql(url, query, {"selector": selector})["scheduleOrError"]
        if (
            schedule.get("__typename") != "Schedule"
            or schedule.get("name") != handover["schedule"]
            or schedule.get("pipelineName") != "stream_dispatcher"
        ):
            raise InstallError(
                "Exact legacy Stream dispatcher schedule could not be verified"
            )
        if schedule["scheduleState"]["status"] != "STOPPED":
            result = graphql(
                url,
                "mutation($id: String!) { stopRunningSchedule(id: $id) { __typename ... on ScheduleStateResult { scheduleState { status } } } }",
                {"id": schedule["scheduleState"]["id"]},
            )["stopRunningSchedule"]
            if (
                result.get("__typename") != "ScheduleStateResult"
                or result["scheduleState"]["status"] != "STOPPED"
            ):
                raise InstallError(
                    "Legacy schedule stop is unconfirmed; independent worker remains authoritative through the shared lock"
                )
        intent["state"] = "complete"
        self.atomic(intent_file, (json.dumps(intent, indent=2) + "\n").encode())

    def install(self, mode):
        if mode not in {"integrate", "bootstrap"}:
            raise InstallError("Invalid installation mode")
        self.check_shared()
        if mode == "integrate":
            self.check_slurm()
        self.state.mkdir(parents=True, mode=0o700, exist_ok=True)
        if self.state.resolve() != self.state.absolute():
            raise InstallError("Installer state cannot contain symlink aliases")
        descriptor = os.open(
            self.state / "install.lock",
            os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK,
            0o600,
        )
        try:
            if not stat.S_ISREG(os.fstat(descriptor).st_mode):
                raise InstallError("Invalid installation lock")
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            self.recover_pending()
            return self._install_locked(mode, self.expected_files(mode))
        finally:
            os.close(descriptor)

    def rollback(self, transaction, journal):
        """Restore files and service ownership from a durable activation journal."""
        errors = []
        # Disable newly enabled units while their installed unit files still
        # exist, so an unsuccessful install cannot reappear at the next boot.
        for service, was_active in journal["active"].items():
            for action in (["stop"] if not was_active else []) + (
                ["disable"] if not journal["enabled"][service] else []
            ):
                try:
                    self.run(["systemctl", action, service])
                except InstallError as error:
                    errors.append(str(error))
        for path, entry in journal["files"].items():
            try:
                target = self.path(path)
                if target.exists() or target.is_symlink():
                    actual = digest(read_regular(target))
                    allowed = {journal["expected"][path]}
                    if entry:
                        allowed.add(digest(read_regular(transaction / entry["file"])))
                    if actual not in allowed:
                        raise InstallError(
                            "Managed file changed outside the interrupted installation"
                        )
                if entry is None:
                    target.unlink(missing_ok=True)
                else:
                    self.atomic(
                        target,
                        read_regular(transaction / entry["file"]),
                        entry["mode"],
                        entry["uid"],
                        entry["gid"],
                    )
            except (OSError, InstallError) as error:
                errors.append(str(error))
        try:
            state = self.state / "files.json"
            if journal["previous_managed"]:
                self.atomic(state, read_regular(transaction / "previous-files.json"))
            else:
                state.unlink(missing_ok=True)
            self.run(["systemctl", "daemon-reload"])
            for service, was_active in journal["active"].items():
                if journal["enabled"][service]:
                    self.run(["systemctl", "enable", service])
                if was_active:
                    self.run(["systemctl", "restart", service])
        except (OSError, InstallError) as error:
            errors.append(str(error))
        if errors:
            raise InstallError(
                f"rollback FAILED; inspect protected backup: {transaction}"
            )
        self.atomic(transaction / "rolled-back", b"restored\n")
        (self.state / "pending.json").unlink(missing_ok=True)

    def recover_pending(self):
        path = self.state / "pending.json"
        if not path.exists():
            return
        pending = json.loads(read_regular(path))
        key = pending.get("transaction")
        if not isinstance(key, str) or str(uuid.UUID(key)) != key:
            raise InstallError("Invalid pending installation identity")
        transaction = self.state / "backups" / key
        if (transaction / "complete").exists():
            path.unlink()
            return
        journal = json.loads(read_regular(transaction / "manifest.json"))
        self.rollback(transaction, journal)

    def _install_locked(self, mode, files):
        previous = self.managed_state()
        changes = []
        for path, expected in files.items():
            target = self.path(path)
            if target.exists() or target.is_symlink():
                actual = read_regular(target)
                current_digest = digest(actual)
                if (
                    mode == "bootstrap"
                    and path.startswith(self.manifest["slurm"]["config_dir"] + "/")
                    and path not in previous
                ):
                    raise InstallError(
                        "Bootstrap refuses an unmanaged existing cluster; use integrate"
                    )
                if path in previous and current_digest != previous[path]["sha256"]:
                    raise InstallError(
                        f"Managed file was edited; reconcile explicitly before replacing: {path}"
                    )
                if path not in previous and current_digest != self.manifest[
                    "adopt"
                ].get(path):
                    # Even identical bytes require explicit adoption: ownership
                    # cannot be inferred from content coincidence.
                    raise InstallError(f"Refusing to replace unmanaged file: {path}")
                metadata = target.stat()
                if (
                    actual == expected[0]
                    and stat.S_IMODE(metadata.st_mode) == expected[1]
                    and (
                        self.root != Path("/")
                        or (metadata.st_uid, metadata.st_gid) == expected[2:]
                    )
                ):
                    continue
            changes.append(path)
        self.ensure_packages(mode)
        if not changes:
            self.check()
            self.begin_handover()
            self.finish_handover()
            managed = {
                path: {"sha256": digest(value[0]), "mode": value[1]}
                for path, value in files.items()
            }
            self.atomic(
                self.state / "files.json",
                (json.dumps({"schema": 1, "files": managed}, indent=2) + "\n").encode(),
            )
            return {
                "state": "unchanged",
                "deployment": self.manifest["id"],
                "changed_files": [],
            }
        transaction = self.state / "backups" / str(uuid.uuid4())
        transaction.mkdir(parents=True, mode=0o700)
        backup = {}
        for index, path in enumerate(changes):
            target = self.path(path)
            if target.exists():
                metadata = target.stat()
                content = read_regular(target)
                saved = transaction / str(index)
                self.atomic(saved, content)
                backup[path] = {
                    "file": saved.name,
                    "mode": stat.S_IMODE(metadata.st_mode),
                    "uid": metadata.st_uid,
                    "gid": metadata.st_gid,
                }
            else:
                backup[path] = None
        services = self.services(mode)
        active = {}
        enabled = {}
        for service in services:
            try:
                self.run(["systemctl", "is-active", "--quiet", service])
                active[service] = True
            except InstallError:
                active[service] = False
            try:
                self.run(["systemctl", "is-enabled", "--quiet", service])
                enabled[service] = True
            except InstallError:
                enabled[service] = False
        old_state = self.state / "files.json"
        if old_state.exists():
            self.atomic(transaction / "previous-files.json", read_regular(old_state))
        journal = {
            "files": backup,
            "expected": {path: digest(files[path][0]) for path in changes},
            "active": active,
            "enabled": enabled,
            "previous_managed": old_state.exists(),
        }
        self.atomic(
            transaction / "manifest.json",
            (json.dumps(journal, indent=2) + "\n").encode(),
        )
        self.atomic(
            self.state / "pending.json",
            (
                json.dumps({"schema": 1, "transaction": transaction.name}) + "\n"
            ).encode(),
        )
        try:
            for path in changes:
                self.atomic(self.path(path), *files[path])
            for spec in (self.manifest["workflow"], self.manifest["dagster"]):
                if spec:
                    for key in ("registry", "home"):
                        if key in spec:
                            directory = self.path(spec[key])
                            directory.mkdir(parents=True, mode=0o700, exist_ok=True)
                            if self.root == Path("/"):
                                os.chown(directory, self.uid, self.gid)
            units = [
                str(self.path(path)) for path in changes if path.endswith(".service")
            ]
            if units:
                self.run(["systemd-analyze", "verify", *units])
            self.preflight_configuration(mode)
            self.begin_handover()
            self.run(["systemctl", "daemon-reload"])
            for service in services:
                self.run(["systemctl", "enable", service])
                self.run(
                    ["systemctl", "restart" if active[service] else "start", service]
                )
            # Heartbeats and gRPC code locations need a bounded startup allowance.
            last_error = None
            for _ in range(self.manifest["health_timeout_seconds"]):
                try:
                    self.check()
                    last_error = None
                    break
                except (InstallError, OSError, ValueError, KeyError) as error:
                    last_error = error
                    if self.root == Path("/"):
                        time.sleep(1)
            if last_error:
                raise InstallError(
                    "Post-activation health checks failed"
                ) from last_error
            managed = {
                path: {"sha256": digest(value[0]), "mode": value[1]}
                for path, value in files.items()
            }
            self.atomic(
                self.state / "files.json",
                (json.dumps({"schema": 1, "files": managed}, indent=2) + "\n").encode(),
            )
            self.atomic(transaction / "complete", b"verified\n")
            (self.state / "pending.json").unlink()
        except BaseException as error:
            try:
                self.rollback(transaction, journal)
            except InstallError as rollback_error:
                raise InstallError(f"Activation failed; {rollback_error}") from error
            raise InstallError(
                f"Activation failed; previous files and service activity restored: {transaction}"
            ) from error
        # Retirement is an external scheduler mutation. Once a healthy worker
        # is committed, an ambiguous stop response must never roll that worker
        # back and leave no dispatcher. A rerun reconciles the exact schedule.
        try:
            self.finish_handover()
        except (InstallError, OSError, ValueError, KeyError) as error:
            raise InstallError(
                "Installation is active; dispatcher handover remains pending. The independent worker stays running; rerun to reconcile."
            ) from error
        return {
            "state": "installed",
            "deployment": self.manifest["id"],
            "changed_files": changes,
            "backup": str(transaction),
        }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("check", "integrate", "bootstrap"))
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(read_regular(args.manifest))
        installer = Installer(manifest)
        if args.mode != "check" and os.geteuid() != 0:
            raise InstallError("Installation requires root; check remains read-only")
        report = (
            installer.check() if args.mode == "check" else installer.install(args.mode)
        )
        print(json.dumps(report, indent=2))
    except (InstallError, OSError, ValueError, KeyError) as error:
        print(f"workflow services: {error}", file=__import__("sys").stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
