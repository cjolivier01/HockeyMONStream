"""Check the installed deployment on each selected computer."""

import json
import os
import shlex
import shutil
import socket
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from uuid import uuid4

from .files import private_directory, write_json
from .schema import WorkflowError, digest, identifier, require
from .setup import absolute, run_readonly, validate_draft
from .setup_progress import emit

# Protocol support, not a configurable processing default. Old readers reject
# catalogs written by the current desktop before any GPU stage can start.
REQUIRED_FEATURES = ["experiment-control-point-selection-v1", "cluster-readiness-v1"]


def has_current_features(features):
    return isinstance(features, list) and all(
        feature in features for feature in REQUIRED_FEATURES
    )


def require_current_task(prepared, timeout, deployment_nodes, *, gpu=False):
    from .executor import installation_environment

    try:
        capabilities = json.loads(
            run_readonly(
                [prepared["task"], "--capabilities"],
                timeout,
                env=installation_environment(prepared["runtime"], os.environ),
            )
        )
    except (OSError, ValueError) as error:
        raise WorkflowError(
            "Could not check the installed HStream helper on "
            + socket.gethostname()
            + ": "
            + str(error)
        ) from error
    require(
        isinstance(capabilities, dict)
        and has_current_features(capabilities.get("features")),
        "The installed HStream on "
        + socket.gethostname()
        + " is too old to read current stitching experiments. "
        "From the current HStream source folder, run "
        + shlex.join(["make", "deploy", "NODES=" + ",".join(deployment_nodes)])
        + ", then click Prepare computers again. Your saved experiments do not need to be deleted.",
    )
    if gpu:
        require(
            capabilities.get("gpu_backend") == "nvidia"
            and capabilities.get("gpu_operations_supported") is True,
            "This installed HStream does not support automatic stitching on this computer's GPU. "
            "Choose a computer with the NVIDIA stitching runtime before preparing again.",
        )
    return {**prepared, "features": REQUIRED_FEATURES, "gpu_supported": gpu}


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
    """Describe the deployment in place; never make a second installation."""
    from .setup_execution import inventory

    host = socket.gethostname()
    emit(host + ": checking installed HStream programs…")
    source = installed_root(preferred, cpu=cpu)
    task, runtime = inventory(str(source), cpu=cpu, paths=payload_paths(source))
    # Deployment paths are durable configuration. Bytes are resolved by the
    # allocated native worker for each attempt, after Slurm chooses its host.
    runtime.update(installation="deployed", files={})
    if not cpu:
        runtime["architecture"] = "native"
    emit(host + ": using installed HStream in " + str(source))
    return {"source": str(source), "task": task, "runtime": runtime, "host": host}


def on_node(request):
    require(
        request.get("on_node") is True and isinstance(request.get("challenge"), str),
        "Expected an internal runtime preparation request",
    )
    root = private_directory(absolute(request["registry"], "Settings folder"))
    return {
        **require_current_task(
            snapshot(root, request["runtime_root"], cpu=False),
            request["timeout_seconds"],
            request["deployment_nodes"],
            gpu=True,
        ),
        "challenge": request["challenge"],
    }


def from_node(registry, node, preferred, timeout, deployment_nodes):
    identifier(node, "Computer")
    path = Path(registry) / ("setup-runtime-request-" + uuid4().hex + ".json")
    challenge = uuid4().hex
    write_json(
        path,
        {
            "action": "prepare-installed-runtime",
            "on_node": True,
            "registry": str(registry),
            "runtime_root": preferred,
            "timeout_seconds": timeout,
            "deployment_nodes": deployment_nodes,
            "challenge": challenge,
        },
        exclusive=True,
    )
    launcher = "hstream-workflow"
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
    from .setup_gpus import needs_inventory, resolve_profiles

    draft = validate_draft(draft)
    require(
        draft["profiles"]
        and all(profile["nodes"] for profile in draft["profiles"].values()),
        "Choose computers for each profile before building",
    )
    deadline = time.monotonic() + timeout
    inventory = ""
    if needs_inventory(draft["profiles"]):
        emit("Checking the GPU choices reported by Slurm…")
        inventory = run_readonly(
            ["sinfo", "--noheader", "--Node", "--format=%N|%P|%G"], timeout
        )
    gpu_choices, messages = resolve_profiles(draft["profiles"], inventory)
    for message in messages:
        emit(message)
    deployment_nodes = sorted(
        {draft["server"]}
        | {node for profile in draft["profiles"].values() for node in profile["nodes"]}
    )
    cpu = require_current_task(
        snapshot(registry, draft["controller_runtime"], cpu=True),
        timeout,
        deployment_nodes,
    )
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
            "Preparing HStream took longer than the connection timeout. Try Prepare again to check the remaining computers.",
        )
        node, source = item
        emit(node + ": connecting to prepare HStream…")
        result = from_node(registry, node, source, remaining, deployment_nodes)
        require(
            has_current_features(result.get("features")),
            node
            + ": the setup helper did not verify support for current stitching experiments. Prepare computers again with the current HStream workflow tools.",
        )
        emit(node + ": preparation complete.")
        return item, result

    with ThreadPoolExecutor(max_workers=parallel) as workers:
        results = dict(workers.map(collect, requested))
    prepared = {
        "metadata": cpu,
        "profiles": {},
        "features": REQUIRED_FEATURES,
        "gpu_supported": all(
            result.get("gpu_supported") is True for result in results.values()
        ),
    }
    for key, profile in draft["profiles"].items():
        versions = {}
        sources = set()
        for node in profile["nodes"]:
            result = results[(node, profile["runtime_root"])]
            sources.add(result["source"])
            group = versions.setdefault(
                digest({"runtime": result["runtime"], "gres": gpu_choices[key][node]}),
                {
                    "nodes": [],
                    "task": result["task"],
                    "runtime": result["runtime"],
                    "gres": gpu_choices[key][node],
                },
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
