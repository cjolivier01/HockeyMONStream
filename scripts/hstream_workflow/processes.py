"""Linux process identity and bounded descendant retirement for local tasks."""

import ctypes
import os
import signal
import time
from pathlib import Path

from .schema import require

_libc = ctypes.CDLL(None, use_errno=True)


def pidfd_open(pid):
    if hasattr(os, "pidfd_open"):
        return os.pidfd_open(pid)
    function = getattr(_libc, "pidfd_open", None)
    require(
        function is not None, "Task descendant signaling requires Linux pidfd support"
    )
    function.argtypes = [ctypes.c_int, ctypes.c_uint]
    function.restype = ctypes.c_int
    descriptor = function(pid, 0)
    if descriptor < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))
    return descriptor


def pidfd_send_signal(descriptor, number):
    if hasattr(signal, "pidfd_send_signal"):
        return signal.pidfd_send_signal(descriptor, number)
    function = getattr(_libc, "pidfd_send_signal", None)
    require(
        function is not None, "Task descendant signaling requires Linux pidfd support"
    )
    function.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
    function.restype = ctypes.c_int
    if function(descriptor, number, None, 0) < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))


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
    # Check the running kernel and interpreter/libc boundary before launching
    # work. Some Python builds omit pidfd wrappers despite system support.
    descriptor = pidfd_open(os.getpid())
    try:
        pidfd_send_signal(descriptor, 0)
    finally:
        os.close(descriptor)
    require(
        _libc.prctl(36, 1, 0, 0, 0) == 0, "Cannot establish task descendant ownership"
    )


def _within_deadline(deadline):
    return deadline is None or time.monotonic() < deadline


def descendants(*, deadline=None):
    processes = {}
    for path in Path("/proc").iterdir():
        if not _within_deadline(deadline):
            break
        if path.name.isdigit():
            current = process_identity(int(path.name))
            if current:
                processes[current["pid"]] = current
    parents, result = {os.getpid()}, {}
    changed = True
    while changed and _within_deadline(deadline):
        changed = False
        for pid, current in processes.items():
            if not _within_deadline(deadline):
                break
            if pid not in parents and current["parent"] in parents:
                parents.add(pid)
                result[pid] = current
                changed = True
    return result


def signal_descendants(number, *, deadline=None):
    for pid, saved in descendants(deadline=deadline).items():
        if not _within_deadline(deadline):
            break
        # Direct children remain ours until reaped. Detached grandchildren
        # become direct children when their parent exits; later passes retire
        # them. Pin before checking identity so PID reuse cannot redirect a kill.
        if saved["parent"] != os.getpid():
            continue
        try:
            descriptor = pidfd_open(pid)
        except ProcessLookupError:
            continue
        try:
            if not _within_deadline(deadline):
                break
            current = process_identity(pid)
            if (
                current is not None
                and current["start_time"] == saved["start_time"]
                and current["parent"] == os.getpid()
                and _within_deadline(deadline)
            ):
                try:
                    pidfd_send_signal(descriptor, number)
                except ProcessLookupError:
                    pass
        finally:
            os.close(descriptor)


def reap(*, deadline=None):
    """Reap exited children and return whether the kernel proves none remain.

    The task supervisor becomes a subreaper before launching any task. A live
    descendant therefore retains a direct child, including after reparenting.
    An empty /proc scan cannot establish this: a child can fork and exit between
    directory enumeration and its stat read, hiding the new adopted child.
    Even an expired deadline permits one nonblocking kernel check, but no retry.
    """
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid == 0:
                return False
        except ChildProcessError:
            return True
        except InterruptedError:
            pass
        if not _within_deadline(deadline):
            return False


def retire(grace_seconds):
    if grace_seconds <= 0:
        return reap(deadline=time.monotonic())
    for number in (signal.SIGTERM, signal.SIGKILL):
        deadline = time.monotonic() + grace_seconds
        if reap(deadline=deadline):
            return True
        while _within_deadline(deadline):
            try:
                signal_descendants(number, deadline=deadline)
            except (OSError, ValueError):
                # Observation/signaling failure is not shutdown proof. Still
                # ask the kernel: all children may have exited independently.
                pass
            if reap(deadline=deadline):
                return True
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.05, remaining))
    # Killing a process in uninterruptible sleep does not prove it stopped.
    return False
