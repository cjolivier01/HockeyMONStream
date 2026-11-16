"""Prepare immutable shared runtimes from programs already installed on nodes."""

import json
import os
import shlex
import shutil
import socket
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from uuid import uuid4

from .files import file_digest, locked, private_directory, sync_directory, write_json
from .schema import WorkflowError, digest, identifier, require
from .setup import absolute, run_readonly, validate_draft


def installed_root(preferred, *, cpu):
    """Keep usable explicit installations; recover missing ones from PATH."""
    if preferred:
        root = absolute(preferred, "HStream installation").resolve()
        binary = root / "bin" if (root / "bin").is_dir() else root
        names = ["hstream-runtime-worker", "hstream-experiment-task"]
        if not cpu:
            names.extend(["hstream-cli", "hstream-job"])
        if all(
            (binary / name).is_file() and os.access(binary / name, os.X_OK)
            for name in names
        ) and (cpu or (root / "configs/ds_hockey_app_config.yaml").is_file()):
            return root
    programs = [
        shutil.which(name)
        for name in ("hstream-runtime-worker", "hstream-experiment-task")
    ]
    require(
        all(programs),
        "HStream is not installed on "
        + socket.gethostname()
        + ". Run make deploy for this computer, then build the setup again.",
    )
    programs = [Path(path).resolve(strict=True) for path in programs]
    require(
        programs[0].parent == programs[1].parent,
        "The installed HStream job programs belong to different folders on "
        + socket.gethostname(),
    )
    binary = programs[0].parent
    return binary.parent if binary.name == "bin" else binary


def payload_paths(root):
    """Use the package manifest so local backups are never copied or pinned."""
    if shutil.which("dpkg-query"):
        result = subprocess.run(
            ["dpkg-query", "-L", "hstream"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        if result.returncode == 0:
            paths = [
                Path(line)
                for line in result.stdout.splitlines()
                if line.startswith(str(root) + "/")
            ]
            if root / "bin/hstream-runtime-worker" in paths:
                return paths
    return list(root.rglob("*"))


def snapshot(registry, preferred, *, cpu):
    from .setup_execution import inventory

    source = installed_root(preferred, cpu=cpu)
    paths = payload_paths(source)
    task, runtime = inventory(str(source), cpu=cpu, paths=paths)
    files = {
        str(Path(path).relative_to(source)): value
        for path, value in runtime["files"].items()
    }
    links = {}
    for path in paths:
        if path.is_symlink():
            target = path.resolve(strict=True)
            require(
                target.is_relative_to(source),
                "An installed HStream link leaves its installation: " + str(path),
            )
            if str(target) in runtime["files"] or target.is_dir():
                links[str(path.relative_to(source))] = str(target.relative_to(source))
    modes = {name: bool((source / name).stat().st_mode & 0o111) for name in files}
    identity = digest({"files": files, "links": links, "executable": modes})
    store = private_directory(Path(registry) / "setup-runtimes", create=True)

    def verify(root):
        present = {
            str(path.relative_to(root))
            for path in root.rglob("*")
            if path.is_file() or path.is_symlink()
        }
        require(present == set(files) | set(links), "Prepared HStream contents changed")
        for name, expected in files.items():
            path = root / name
            require(
                path.is_file() and not path.is_symlink(),
                "Prepared HStream file is missing: " + str(path),
            )
            require(
                file_digest(path, 8 * 1024**3)["sha256"] == expected,
                "Prepared HStream file changed: " + str(path),
            )
            require(
                bool(path.stat().st_mode & 0o111) == modes[name],
                "Prepared HStream permissions changed",
            )
        for name, target in links.items():
            require(
                (root / name).is_symlink()
                and (root / name).resolve(strict=True) == root / target,
                "Prepared HStream link changed",
            )

    with locked(store / (identity + ".lock")):
        destination = store / identity
        candidates = [destination, *sorted(store.glob(identity + "-*"))]
        for candidate in candidates:
            if not candidate.exists():
                continue
            try:
                private_directory(candidate)
                verify(candidate)
            except (OSError, ValueError):
                continue
            destination = candidate
            break
        else:
            if destination.exists():
                # Never replace a version retained by an admitted run.
                destination = store / (identity + "-" + uuid4().hex)
        if not destination.exists():
            with tempfile.TemporaryDirectory(
                prefix=".preparing-", dir=store
            ) as temporary:
                staging = Path(temporary) / "runtime"
                staging.mkdir(mode=0o700)
                for name in files:
                    target = staging / name
                    target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                    with (source / name).open("rb") as incoming, target.open(
                        "xb"
                    ) as output:
                        shutil.copyfileobj(incoming, output, 1024 * 1024)
                        output.flush()
                        os.fsync(output.fileno())
                    target.chmod(0o700 if modes[name] else 0o600)
                for name, target in links.items():
                    alias = staging / name
                    alias.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                    alias.symlink_to(os.path.relpath(staging / target, alias.parent))
                verify(staging)
                # Do not combine files from an installation that changed mid-copy.
                _, after = inventory(str(source), cpu=cpu, paths=payload_paths(source))
                require(
                    after == runtime,
                    "HStream was updated while setup was preparing it on "
                    + socket.gethostname()
                    + ". Build again to use the completed update.",
                )
                os.rename(staging, destination)
                sync_directory(store)

    def remap(path):
        return str(destination / Path(path).relative_to(source))

    prepared = {
        **runtime,
        "root": str(destination),
        "files": {remap(path): value for path, value in runtime["files"].items()},
    }
    for key in ("worker", "cli", "exporter", "config"):
        if key in prepared:
            prepared[key] = remap(prepared[key])
    return {
        "source": str(source),
        "task": remap(task),
        "runtime": prepared,
        "host": socket.gethostname(),
    }


def on_node(request):
    require(
        request.get("on_node") is True and isinstance(request.get("challenge"), str),
        "Expected an internal runtime preparation request",
    )
    root = private_directory(absolute(request["registry"], "Settings folder"))
    return {
        **snapshot(root, request["runtime_root"], cpu=False),
        "challenge": request["challenge"],
    }


def from_node(registry, node, preferred, timeout):
    identifier(node, "Computer")
    path = Path(registry) / ("setup-runtime-request-" + uuid4().hex + ".json")
    challenge = uuid4().hex
    write_json(
        path,
        {
            "action": "prepare-runtime",
            "on_node": True,
            "registry": str(registry),
            "runtime_root": preferred,
            "challenge": challenge,
        },
        exclusive=True,
    )
    launcher = Path(__file__).resolve().parents[1] / "hstream-workflow"
    try:
        output = run_readonly(
            [
                "ssh",
                "-oBatchMode=yes",
                "-oStrictHostKeyChecking=yes",
                "-oConnectTimeout=" + str(timeout),
                "--",
                node,
                shlex.join([str(launcher), "setup", "--request", str(path)]),
            ],
            timeout,
        )
        result = json.loads(output)
        require(
            result.pop("challenge", None) == challenge,
            node + " cannot see the shared settings folder",
        )
        result.pop("progress", None)
        return result
    except (OSError, ValueError) as error:
        raise WorkflowError(node + ": " + str(error)) from error
    finally:
        path.unlink(missing_ok=True)


def prepare(registry, draft, timeout, parallel):
    draft = validate_draft(draft)
    require(
        draft["profiles"]
        and all(profile["nodes"] for profile in draft["profiles"].values()),
        "Choose computers for each profile before building",
    )
    deadline = time.monotonic() + timeout
    messages = []
    cpu = snapshot(registry, draft["controller_runtime"], cpu=True)
    if draft["controller_runtime"] != cpu["source"]:
        messages.append(
            "Updated the main-server installation to " + cpu["source"] + "."
        )
        draft["controller_runtime"] = cpu["source"]
    requested = sorted(
        {
            (node, profile["runtime_root"])
            for profile in draft["profiles"].values()
            for node in profile["nodes"]
        }
    )

    def collect(item):
        remaining = int(deadline - time.monotonic())
        require(
            remaining > 0,
            "Preparing HStream took longer than the connection timeout. Build again to reuse the completed copies.",
        )
        node, source = item
        return item, from_node(registry, node, source, remaining)

    with ThreadPoolExecutor(max_workers=parallel) as workers:
        results = dict(workers.map(collect, requested))
    prepared = {"metadata": cpu, "profiles": {}}
    for key, profile in draft["profiles"].items():
        versions = {}
        sources = set()
        for node in profile["nodes"]:
            result = results[(node, profile["runtime_root"])]
            sources.add(result["source"])
            group = versions.setdefault(
                digest(result["runtime"]),
                {"nodes": [], "task": result["task"], "runtime": result["runtime"]},
            )
            group["nodes"].append(node)
        if len(sources) == 1 and profile["runtime_root"] not in sources:
            profile["runtime_root"] = next(iter(sources))
            messages.append(
                profile["label"]
                + ": recovered the installed programs in "
                + profile["runtime_root"]
                + "."
            )
        prepared["profiles"][key] = list(versions.values())
        if len(versions) > 1:
            messages.append(
                profile["label"]
                + ": prepared "
                + str(len(versions))
                + " compatible groups for the selected computers."
            )
    groups = []
    for key, profile in draft["profiles"].items():
        keys, nodes = {key}, set(profile["nodes"])
        for existing in list(groups):
            if nodes & existing[1]:
                groups.remove(existing)
                keys |= existing[0]
                nodes |= existing[1]
        groups.append((keys, nodes))
    for keys, _ in groups:
        for field in ("concurrency", "per_user"):
            limit = min(draft["profiles"][key][field] for key in keys)
            if any(draft["profiles"][key][field] != limit for key in keys):
                for key in keys:
                    draft["profiles"][key][field] = limit
                messages.append(
                    "Profiles sharing computers now use the existing lower "
                    + ("parallel-job" if field == "concurrency" else "per-user")
                    + " limit of "
                    + str(limit)
                    + "."
                )
    return draft, prepared, messages
