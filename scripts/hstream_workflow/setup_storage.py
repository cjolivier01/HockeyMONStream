"""Verify shared storage before sending an installed helper to another node."""

import json
import shlex
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from uuid import uuid4

from .files import locked, private_directory, read_json, write_json
from .setup_progress import emit
from .schema import WorkflowError, positive, require
from .setup import SetupStore, absolute, run_readonly

# This bootstrap needs only Python, not a launcher in the folder being tested.
# A reply file proves writes are shared too; matching path names are insufficient.
PROBE = r"""
import json, os, stat, sys
from pathlib import Path
marker, reply, token = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
try:
    root = marker.parent
    info = root.lstat()
    assert root == root.resolve() and stat.S_ISDIR(info.st_mode)
    assert info.st_uid == os.getuid() and not info.st_mode & 0o022
    assert json.loads(marker.read_text()) == {'token': token}
    fd = os.open(reply, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump({'token': token}, output)
        output.flush()
        os.fsync(output.fileno())
    print(json.dumps({'shared': True}))
except (OSError, ValueError, AssertionError):
    print(json.dumps({'shared': False}))
"""


def check(root, computers, timeout, parallel):
    root = private_directory(root, create=True)
    positive(timeout, "Shared storage timeout", 300)
    positive(parallel, "Shared storage parallelism", 256)
    deadline = time.monotonic() + timeout
    token = uuid4().hex
    marker = root / ("setup-storage-check-" + token + ".json")
    replies = {
        node: root / ("setup-storage-reply-" + token + "-" + node + ".json")
        for node in computers
    }
    write_json(marker, {"token": token}, exclusive=True)

    def probe(node):
        emit(node + ": checking access to shared settings…")
        try:
            remaining = int(deadline - time.monotonic())
            require(remaining > 0, "Shared storage checks exceeded the setup timeout")
            output = run_readonly(
                [
                    "ssh",
                    "-oBatchMode=yes",
                    "-oStrictHostKeyChecking=yes",
                    "-oConnectTimeout=" + str(remaining),
                    "--",
                    node,
                    shlex.join(
                        ["python3", "-c", PROBE, str(marker), str(replies[node]), token]
                    ),
                ],
                remaining,
            )
            shared = json.loads(output).get("shared") is True
            if shared:
                shared = replies[node].exists() and read_json(replies[node]) == {
                    "token": token
                }
            emit(
                node
                + (
                    ": shared settings are accessible."
                    if shared
                    else ": this settings folder is not shared; looking for shared storage."
                )
            )
            return None if shared else node
        except (OSError, ValueError) as error:
            raise WorkflowError(
                "Could not check shared storage on " + node + ": " + str(error)
            ) from error

    try:
        with ThreadPoolExecutor(max_workers=parallel) as workers:
            return [node for node in workers.map(probe, computers) if node is not None]
    finally:
        marker.unlink(missing_ok=True)
        for reply in replies.values():
            reply.unlink(missing_ok=True)


def recovered(root):
    """Recover a moved draft, without redirecting any execution/history reads."""
    visited = set()
    while True:
        root = Path(root)
        require(
            root not in visited and len(visited) < 16,
            "The saved setup locations form a loop",
        )
        visited.add(root)
        saved = SetupStore(root).read()
        location = root / "setup-location.json"
        if not location.exists():
            return saved
        value = read_json(location)
        if value["source_revision"] != saved["revision"]:
            return saved
        root = absolute(value["registry"], "Shared settings folder")
        require(
            SetupStore(root).read()["draft"] is not None,
            "The relocated setup draft is missing: " + str(root),
        )


def require_retired_controller(root, server, timeout):
    """Older controllers lack the service lock; check their PID on their host."""
    heartbeat = root / "heartbeat.json"
    if not heartbeat.exists():
        return
    identity = read_json(heartbeat)
    require(
        identity.get("boot_id")
        and type(identity.get("pid")) is int
        and type(identity.get("start_time")) is int,
        "The previous controller has no verifiable process identity",
    )
    # Reuse the controller's exact host/boot/PID check without depending on a
    # launcher or Python package in the folder that is being relocated.
    script = Path(__file__).with_name("processes.py").read_text() + """
import json, sys
saved = json.loads(sys.argv[1])
print(json.dumps({'retired': saved.get('host') == host_identity()['host'] and not same_process(saved)}))
"""
    result = run_readonly(
        [
            "ssh",
            "-oBatchMode=yes",
            "-oStrictHostKeyChecking=yes",
            "-oConnectTimeout=" + str(timeout),
            "--",
            server,
            shlex.join(["python3", "-c", script, json.dumps(identity)]),
        ],
        timeout,
    )
    require(
        json.loads(result).get("retired") is True and read_json(heartbeat) == identity,
        "Stop the previous HStream controller on "
        + str(identity.get("host", server))
        + " before moving setup. Check its process on that server; an old heartbeat alone does not mean it stopped.",
    )


def prepare(request):
    """Move only editable choices; never move active registries or test history."""
    from .setup_execution import generation, idle, remote

    root = private_directory(absolute(request["registry"], "Settings folder"))
    saved = SetupStore(root).read()
    require(
        saved["revision"] == request["revision"],
        "Setup choices changed. Recover the saved setup before building again.",
    )
    restored = recovered(root)
    if restored != saved:
        return prepare(
            {**request, **restored, "registry": restored["draft"]["registry"]}
        )
    computers = sorted(
        {
            saved["draft"]["server"],
            *(
                node
                for profile in saved["draft"]["profiles"].values()
                for node in profile["nodes"]
            ),
        }
    )
    timeout, parallel = request["timeout_seconds"], request["defaults"]["max_parallel"]
    missing = check(root, computers, timeout, parallel)
    if not missing:
        return request, ""

    target = absolute(request["shared_registry"], "Suggested shared settings folder")
    require(
        target != root,
        "The settings folder is not shared with "
        + ", ".join(missing)
        + ". Choose a shared storage folder; machine-local .local folders cannot hold the cluster registry.",
    )
    require(
        not check(target, computers, timeout, parallel),
        "The suggested settings folder is not shared by every selected computer. Choose storage mounted at the same path across the cluster.",
    )
    previous = generation(root)[1] if (root / "setup-build.json").exists() else None
    if previous and previous["tests"]:
        status = remote({**request, "action": "test-status", "tick": False})
        require(
            all(row.get("finished", True) for row in status["tests"]),
            "Resume or stop the unfinished sample tests before moving setup to shared storage.",
        )
    with locked(root / "setup-execution.lock"), locked(root / "setup.lock"):
        require(
            SetupStore(root).read() == saved,
            "Setup choices changed while checking storage. Recover the saved setup.",
        )
        require(
            recovered(root) == saved,
            "Setup moved while checking storage. Recover the saved setup before building again.",
        )
        current = generation(root)[1] if (root / "setup-build.json").exists() else None
        require(
            current == previous,
            "Sample tests changed while checking storage. Resume/check tests before rebuilding.",
        )
        # A previous setup may already have prepared these exact choices on
        # shared storage. Resume that independent draft without migrating or
        # retiring the old registry, whose historical files may be unavailable.
        # Normal qualification and activation still operate on the destination.
        with locked(target / "setup.lock", blocking=False):
            existing = SetupStore(target).read()
            if not (target / "setup-location.json").exists() and existing["draft"] == {
                **saved["draft"],
                "registry": str(target),
            }:
                return {
                    **request,
                    **existing,
                    "registry": str(target),
                }, "Recovered your existing shared setup in " + str(target) + "."
        try:
            service_lock = locked(root / "controller-service.lock", blocking=False)
            with service_lock:
                idle(root)
                require_retired_controller(root, saved["draft"]["server"], timeout)

                def publish(target):
                    draft = {**saved["draft"], "registry": str(target)}
                    # Hold the destination lock through pointer publication so
                    # another relocation cannot create a cycle between checks.
                    with locked(target / "setup.lock", blocking=False):
                        existing = SetupStore(target).read()
                        if (target / "setup-location.json").exists() or (
                            existing["draft"] is not None and existing["draft"] != draft
                        ):
                            return None
                        write_json(target / "setup.json", draft)
                        write_json(
                            root / "setup-location.json",
                            {
                                "source_revision": saved["revision"],
                                "registry": str(target),
                            },
                        )
                        return SetupStore(target).read()

                moved = publish(target)
                if moved is None:
                    # Existing choices or relocation history belong to another
                    # setup. Use a fresh verified sibling without changing them.
                    target = private_directory(
                        target.with_name(target.name + "-" + uuid4().hex[:12]),
                        create=True,
                    )
                    require(
                        not check(target, computers, timeout, parallel),
                        "The new settings folder could not be verified on every computer",
                    )
                    moved = publish(target)
                    require(
                        moved is not None,
                        "The shared settings folder changed; rebuild setup",
                    )
        except BlockingIOError as error:
            raise WorkflowError(
                "Stop the controller before moving its setup to shared storage. Running jobs and history stay in the original folder."
            ) from error
    return {
        **request,
        **moved,
        "registry": str(target),
    }, "Created shared settings in " + str(
        target
    ) + ". Previous settings and history remain in " + str(
        root
    ) + "."
