"""Host and PID identities for controller recovery; native workers own retirement."""

import os
from pathlib import Path


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
