"""Controller-side admission and completion for the generic native batch worker.

This module never executes on compute. Scheduler jobs contain only the pinned
C++ worker command; no controller process must stay alive for a batch to finish.
"""

import shutil
from pathlib import Path
from uuid import uuid4

from .files import file_digest, private_directory, read_json, write_json
from .schema import require

SCHEMA = "hstream-runtime-worker-v1"
LOG_SEGMENT_BYTES = 8 * 1024 * 1024


def prepare(root, request, profile, settings, intent):
    runtime = profile["runtime"]
    worker = runtime.get("worker")
    require(
        worker in runtime["files"], "Standalone execution needs a pinned runtime.worker"
    )
    directory = private_directory(root / "worker", create=True)
    scratch = private_directory(root / "runtime", create=True)
    environment = {
        "HSTREAM_WORKFLOW_ATTEMPT": intent["attempt_id"],
        "HSTREAM_RUNTIME_CACHE_DIR": str(scratch),
        "HSTREAM_JOB_MANAGED_RUNTIME": "1",
    }
    files = dict(runtime["files"])
    for name in ("request.json", "profile.json", "settings.json"):
        path = root / name
        files[str(path)] = file_digest(path, 8 * 1024 * 1024)["sha256"]
    snapshot = request["runtime"].get("user_config_snapshot")
    if snapshot is not None:
        path = root / "user-config-snapshot.json"
        write_json(path, snapshot, exclusive=True)
        environment["HSTREAM_USER_CONFIG_SNAPSHOT"] = str(path)
        files[str(path)] = file_digest(path, 8 * 1024 * 1024)["sha256"]
    device = intent.get("device")
    if device:
        backend = runtime["backend"]
        require(
            backend in {"nvidia", "amd"}, "Local GPU profile has no recognized backend"
        )
        if backend == "nvidia":
            environment["CUDA_VISIBLE_DEVICES"] = device
        else:
            environment.update(ROCR_VISIBLE_DEVICES=device, HIP_VISIBLE_DEVICES="0")
    workflow_cancel = root.parent.parent / "cancel-intent.json"
    poll_ms = min(settings["poll_interval_ms"], 1000)
    retire_ms = settings["shutdown_timeout_seconds"] * 1000
    task_ms = profile["resources"]["time_seconds"] * 1000
    spec = {
        "id": request["attempt_id"].replace("-", ""),
        "argv": [*profile["task_command"], "--request", str(root / "request.json")],
        "cwd": str(root),
        "stdout": str(root / "task.log"),
        "stderr": str(root / "task.log"),
        "environment": environment,
        "unset_environment": [
            "HSTREAM_UI_PARENT_PID",
            "HSTREAM_ARCHIVE_RUN_ID",
            "HSTREAM_JOB_MANIFEST",
            "HSTREAM_USER_CONFIG_SNAPSHOT",
        ],
        "timeout_ms": task_ms,
        "rotate_log": True,
    }
    value = {
        "schema": SCHEMA,
        "id": request["attempt_id"],
        "token": uuid4().hex,
        "workspace": str(root),
        "architecture": runtime["architecture"],
        "files": files,
        "read_files": [str(workflow_cancel)],
        "cancel_files": [
            {
                "path": str(root / "cancel.json"),
                "match": {"attempt_id": request["attempt_id"]},
            },
            {
                "path": str(workflow_cancel),
                "match": {
                    "workflow_id": request["workflow_id"],
                    "epoch": request["epoch"],
                },
            },
        ],
        "limits": {
            "poll_ms": poll_ms,
            "retire_ms": retire_ms,
            "timeout_ms": task_ms
            + 2 * retire_ms
            + settings["command_timeout_seconds"] * 1000,
            "client_timeout_ms": 0,
            "max_bytes": 0,
            "max_log_bytes": LOG_SEGMENT_BYTES,
            "max_tasks": 1,
        },
        "batch": {
            "spec": spec,
            "artifacts": [
                {"path": str(root / "native-result.json"), "maximum": 4 * 1024 * 1024}
            ],
        },
    }
    if profile["executor"] == "slurm":
        value["nodes"] = profile["resources"]["nodes"]
    path = directory / "request.json"
    write_json(path, value, exclusive=True)
    intent["worker_request_sha256"] = file_digest(path, 4 * 1024 * 1024)["sha256"]
    return [worker, "--request", str(path)]


def completion(root):
    """Adopt native retirement proof before deleting controller-owned scratch."""
    root = Path(root)
    if (root / "completion.json").exists():
        return read_json(root / "completion.json")
    result_path = root / "worker/result.json"
    if not result_path.exists():
        return None
    intent = read_json(root / "submission.json")
    admitted = read_json(root / "worker/request.json")
    result = read_json(result_path)
    require(
        result.get("schema") == admitted["schema"] == SCHEMA
        and result.get("id") == admitted["id"] == intent["attempt_id"]
        and result.get("token") == admitted["token"]
        and result.get("request_sha256")
        == intent.get("worker_request_sha256")
        == file_digest(root / "worker/request.json", 4 * 1024 * 1024)["sha256"],
        "Native worker completion binding changed",
    )
    require(
        result.get("stopped") is True, "Native worker descendant retirement is unproved"
    )
    task_id = intent["attempt_id"].replace("-", "")
    tasks = result.get("tasks", {})
    require(set(tasks) <= {task_id}, "Native worker completed unexpected tasks")
    task = tasks.get(task_id)
    require(
        task is None or task.get("stopped") is True,
        "Native task retirement is unproved",
    )
    error = result.get("error", "")
    require(task is not None or error, "Native worker omitted its task result")
    receipt = result.get("artifacts", {}).get(str(root / "native-result.json"))
    value = {
        "schema": 1,
        "attempt_id": intent["attempt_id"],
        "request_digest": intent["request_digest"],
        "profile_digest": intent["profile_digest"],
        "stopped": True,
        "cleanup_complete": False,
        "cancelled": result.get("cancelled") is True,
        "exit_code": task.get("returncode") if task else None,
        "error": error,
    }
    if receipt is not None:
        value["receipt"] = {key: receipt[key] for key in ("size", "sha256")}
    scratch = root / "runtime"
    require(not scratch.is_symlink(), "Runtime cleanup path was replaced")
    if scratch.exists():
        shutil.rmtree(scratch)
    value["cleanup_complete"] = True
    write_json(root / "completion.json", value, exclusive=True)
    return value
