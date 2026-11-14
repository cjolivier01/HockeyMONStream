"""Detached local supervisor launch with persisted process identities."""

import os
import subprocess
from pathlib import Path

from .files import file_digest, read_json, write_json
from .processes import host_identity, process_identity, same_process
from .schema import require

_children = {}


class _SupervisorProcess(subprocess.Popen):
    def __init__(self, root, intent, command, log):
        try:
            super().__init__(
                command,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                close_fds=True,
                env=dict(os.environ),
            )
        except OSError as exc:
            # CPython sets this flag immediately after fork/posix_spawn accepts
            # a child. A later constructor error cannot prove descendants are
            # absent, even when the supervisor never published its identity.
            if not self._child_created:
                write_json(
                    root / "launch-rejected.json",
                    {
                        "schema": 1,
                        "attempt_id": intent["attempt_id"],
                        "backend": "local",
                        **host_identity(),
                        "accepted": False,
                        "errno": exc.errno,
                        "reason": "Local supervisor launch was rejected: "
                        + str(exc)[:8000],
                    },
                    exclusive=True,
                )
            raise


def collect():
    for pid, child in list(_children.items()):
        if child.poll() is not None:
            del _children[pid]


def submit(root, supervisor_command):
    collect()
    root = Path(root)
    intent = read_json(root / "submission.json")
    require(intent["backend"] == "local", "Wrong submission backend")
    with (root / "supervisor.log").open("xb", buffering=0) as log:
        write_json(
            root / "dispatch.json",
            {"attempt_id": intent["attempt_id"], "backend": "local"},
            exclusive=True,
        )
        child = _SupervisorProcess(root, intent, supervisor_command, log)
    current = process_identity(child.pid)
    require(current is not None, "Local supervisor acceptance is unresolved")
    launch = {
        "schema": 1,
        "attempt_id": intent["attempt_id"],
        **host_identity(),
        **current,
    }
    write_json(root / "launch.json", launch, exclusive=True)
    _children[child.pid] = child
    return launch


def lookup(root):
    collect()
    root = Path(root)
    intent = read_json(root / "submission.json")
    rejected = root / "launch-rejected.json"
    if rejected.exists():
        proof = read_json(rejected)
        require(
            proof.get("schema") == 1
            and proof.get("attempt_id") == intent["attempt_id"]
            and proof.get("backend") == intent.get("backend") == "local"
            and proof.get("accepted") is False
            and isinstance(proof.get("reason"), str)
            and 0 < len(proof["reason"]) <= 8192,
            "Local launch rejection identity changed",
        )
        require(
            not (root / "supervisor.json").exists()
            and not (root / "launch.json").exists(),
            "Local launch rejection conflicts with an accepted supervisor",
        )
        return {
            "state": "REJECTED",
            "terminal": True,
            "stopped": True,
            "cleanup_complete": True,
            "attempt_id": intent["attempt_id"],
            "reason": proof["reason"],
        }
    worker_state = root / "worker/state.json"
    if worker_state.exists():
        state = read_json(worker_state)
        admitted = read_json(root / "worker/request.json")
        require(
            state.get("id") == intent["attempt_id"]
            and state.get("token") == admitted["token"]
            and state.get("request_sha256")
            == intent.get("worker_request_sha256")
            == file_digest(root / "worker/request.json", 4 * 1024 * 1024)["sha256"],
            "Native worker identity changed",
        )
        identity = state["identity"]
        if same_process(
            {**identity, "boot_id": identity["boot"], "start_time": identity["start"]}
        ):
            return {
                "state": "RUNNING",
                "terminal": False,
                "host": state["identity"]["host"],
                "reason": "Native worker is running",
            }
    # Read old accepted identities during a rolling controller upgrade.
    for name in ("supervisor.json", "launch.json"):
        path = root / name
        if not path.exists():
            continue
        saved = read_json(path)
        require(
            saved.get("attempt_id") == intent["attempt_id"],
            "Supervisor attempt identity changed",
        )
        if same_process(saved):
            return {
                "state": "RUNNING",
                "terminal": False,
                "host": saved["host"],
                "reason": "Native task supervisor is running",
            }
    # Missing/reused PID cannot establish that escaped/reparented descendants died.
    return {
        "state": "UNKNOWN",
        "terminal": False,
        "reason": "Local supervisor shutdown is unconfirmed",
    }
