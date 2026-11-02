"""Linux process identity and bounded descendant retirement for local tasks."""

import ctypes
import os
import signal
import time
from pathlib import Path

from .schema import require


def host_identity():
    return {
        "host": os.uname().nodename,
        "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
    }


def process_identity(pid):
    try:
        value = Path(f"/proc/{int(pid)}/stat").read_text()
        fields = value[value.rfind(")") + 2 :].split()
        return {
            "pid": int(pid),
            "start_time": int(fields[19]),
            "parent": int(fields[1]),
            "state": fields[0],
        }
    except (FileNotFoundError, ProcessLookupError):
        return None


def same_process(saved):
    if any(saved.get(key) != value for key, value in host_identity().items()):
        return False
    current = process_identity(saved["pid"])
    return (
        current is not None
        and current["start_time"] == saved["start_time"]
        and current["state"] != "Z"
    )


def become_subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    require(
        libc.prctl(36, 1, 0, 0, 0) == 0, "Cannot establish task descendant ownership"
    )


def descendants():
    processes = {}
    for path in Path("/proc").iterdir():
        if path.name.isdigit():
            current = process_identity(int(path.name))
            if current:
                processes[current["pid"]] = current
    parents, result = {os.getpid()}, {}
    changed = True
    while changed:
        changed = False
        for pid, current in processes.items():
            if pid not in parents and current["parent"] in parents:
                parents.add(pid)
                result[pid] = current
                changed = True
    return result


def signal_descendants(number):
    for pid, saved in descendants().items():
        current = process_identity(pid)
        if current is not None and current["start_time"] == saved["start_time"]:
            try:
                os.kill(pid, number)
            except ProcessLookupError:
                continue


def reap():
    """Reap exited children and return whether the kernel proves none remain.

    The task supervisor becomes a subreaper before launching any task. A live
    descendant therefore retains a direct child, including after reparenting.
    An empty /proc scan cannot establish this: a child can fork and exit between
    directory enumeration and its stat read, hiding the new adopted child.
    """
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid == 0:
                return False
        except ChildProcessError:
            return True
        except InterruptedError:
            continue


def retire(grace_seconds):
    deadline = time.monotonic() + grace_seconds
    signal_descendants(signal.SIGTERM)
    while time.monotonic() < deadline:
        if reap():
            return True
        time.sleep(0.05)
    signal_descendants(signal.SIGKILL)
    # Killing a process in uninterruptible sleep does not prove it stopped.
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        if reap():
            return True
        signal_descendants(signal.SIGKILL)
        time.sleep(0.05)
    return False
