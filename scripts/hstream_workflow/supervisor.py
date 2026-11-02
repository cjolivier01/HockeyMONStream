"""Standalone compute task supervisor; no database or controller credentials."""

import argparse
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

from .bounded_log import Capture
from .files import file_digest, private_directory, read_json, write_json
from .processes import become_subreaper, host_identity, process_identity, retire
from .profiles import preflight_runtime
from .schema import digest, require


def execute(root):
    root = private_directory(root)
    intent = read_json(root / "submission.json")
    request = read_json(root / "request.json")
    profile = read_json(root / "profile.json")
    settings = read_json(root / "settings.json")
    require(
        intent["request_digest"] == digest(request),
        "Task request changed after admission",
    )
    require(
        intent["profile_digest"] == digest(profile),
        "Resource profile changed after admission",
    )
    require(
        request["attempt_id"] == intent["attempt_id"], "Task attempt identity changed"
    )
    become_subreaper()
    cancelled = False

    def cancel(_number, _frame):
        nonlocal cancelled
        cancelled = True

    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    owner = {
        **host_identity(),
        **process_identity(os.getpid()),
        "attempt_id": intent["attempt_id"],
    }
    write_json(root / "supervisor.json", owner, exclusive=True)
    runtime = private_directory(root / "runtime", create=True)
    result = {
        "schema": 1,
        "attempt_id": intent["attempt_id"],
        "request_digest": intent["request_digest"],
        "profile_digest": intent["profile_digest"],
        "stopped": False,
        "cleanup_complete": False,
        "cancelled": False,
        "exit_code": None,
        "error": "",
    }
    child = None
    capture = None
    try:
        preflight_runtime(profile)
        require(
            os.uname().machine == profile["runtime"]["architecture"],
            "Compute architecture differs from the frozen profile",
        )
        if profile["executor"] == "slurm":
            require(
                os.environ.get("SLURMD_NODENAME") in profile["resources"]["nodes"],
                "Slurm allocated an unqualified compute node",
            )
        environment = dict(os.environ)
        for key in (
            "HSTREAM_UI_PARENT_PID",
            "HSTREAM_ARCHIVE_RUN_ID",
            "HSTREAM_JOB_MANIFEST",
        ):
            environment.pop(key, None)
        environment.update(
            HSTREAM_WORKFLOW_ATTEMPT=intent["attempt_id"],
            HSTREAM_RUNTIME_CACHE_DIR=str(runtime),
            HSTREAM_JOB_MANAGED_RUNTIME="1",
        )
        snapshot = request["runtime"].get("user_config_snapshot")
        environment.pop("HSTREAM_USER_CONFIG_SNAPSHOT", None)
        if snapshot is not None:
            snapshot_path = root / "user-config-snapshot.json"
            write_json(snapshot_path, snapshot, exclusive=True)
            environment["HSTREAM_USER_CONFIG_SNAPSHOT"] = str(snapshot_path)
        device = intent.get("device")
        if device:
            backend = profile["runtime"].get("backend")
            if backend == "nvidia":
                environment["CUDA_VISIBLE_DEVICES"] = device
            elif backend == "amd":
                environment["ROCR_VISIBLE_DEVICES"] = device
                environment["HIP_VISIBLE_DEVICES"] = "0"
            else:
                raise ValueError("Local GPU profile has no recognized backend")

        def cancellation_requested():
            path = root / "cancel.json"
            if path.exists():
                require(
                    read_json(path).get("attempt_id") == intent["attempt_id"],
                    "Cancellation identity changed",
                )
                return True
            path = root.parent.parent / "cancel-intent.json"
            if path.exists():
                value = read_json(path)
                require(
                    value.get("workflow_id") == request["workflow_id"],
                    "Workflow cancellation identity changed",
                )
                return value.get("epoch") == request["epoch"]
            return False

        cancelled = cancelled or cancellation_requested()
        require(not cancelled, "Task cancelled before native launch")
        deadline = time.monotonic() + profile["resources"]["time_seconds"]
        child = subprocess.Popen(
            [*profile["task_command"], "--request", str(root / "request.json")],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            env=environment,
        )
        capture = Capture(root, child.stdout)
        write_json(
            root / "child.json",
            {**host_identity(), **process_identity(child.pid)},
            exclusive=True,
        )
        while child.poll() is None:
            cancelled = cancelled or cancellation_requested()
            if cancelled or time.monotonic() >= deadline:
                if not cancelled:
                    result["error"] = "Native task exceeded its time limit"
                break
            time.sleep(min(settings["poll_interval_ms"] / 1000, 1))
        result["exit_code"] = child.poll()
        result["stopped"] = retire(settings["shutdown_timeout_seconds"])
        if child.returncode is None and result["stopped"]:
            # The subreaper may have collected the leader; its successful exit
            # must never be invented after a timeout/cancel.
            result["exit_code"] = -signal.SIGTERM
        receipt = root / "native-result.json"
        if result["stopped"] and receipt.exists():
            result["receipt"] = file_digest(receipt, 4 * 1024 * 1024)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        result["error"] = str(exc)[:8192]
        result["stopped"] = retire(settings["shutdown_timeout_seconds"])
    finally:
        if capture is not None:
            capture_error = capture.finish(settings["shutdown_timeout_seconds"])
            if capture_error:
                result["error"] += "; " + capture_error
        result["cancelled"] = cancelled
        if result["stopped"]:
            try:
                shutil.rmtree(runtime)
                result["cleanup_complete"] = True
            except OSError as exc:
                result["error"] += "; runtime cleanup failed: " + str(exc)
        write_json(root / "completion.json", result, exclusive=True)
    return (
        0
        if result["stopped"]
        and result["cleanup_complete"]
        and not result["cancelled"]
        and result["exit_code"] == 0
        and not result["error"]
        else 1
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    arguments = parser.parse_args()
    return execute(arguments.root)


if __name__ == "__main__":
    raise SystemExit(main())
