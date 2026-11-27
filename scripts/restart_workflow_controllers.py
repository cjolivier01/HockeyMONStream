"""Restart running systemd workflow controllers that use this HStream package."""

import os
import pwd
import re
import subprocess
import sys
import time
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
# Deploy invokes this file with isolated Python, including for the heartbeat check.
sys.path.insert(0, str(SCRIPTS))
from hstream_workflow.files import read_json
from hstream_workflow.processes import host_identity, process_identity, same_process
from hstream_workflow.schema import WorkflowError, positive


def running_services(manager):
    output = subprocess.check_output(
        [
            *manager,
            "show",
            "--type=service",
            "--state=running",
            "--property=Id,MainPID,KillMode",
            "*.service",
        ],
        text=True,
    )
    return [
        dict(line.split("=", 1) for line in block.splitlines())
        for block in output.strip().split("\n\n")
        if block
    ]


def controller_registry(process):
    argv = os.fsdecode((process / "cmdline").read_bytes()).rstrip("\0").split("\0")
    if argv[1:4] == ["-m", "hstream_workflow.cli", "serve"]:
        if (process / "cwd").resolve() != SCRIPTS:
            return None
        args = argv[4:]
    elif (
        len(argv) >= 3
        and argv[2] == "serve"
        and Path(argv[1]).name == "hstream-workflow"
    ):
        if (process / "cwd" / argv[1]).resolve() != SCRIPTS / "hstream-workflow":
            return None
        args = argv[3:]
    else:
        return None
    for index, arg in enumerate(args):
        if arg == "--registry" and index + 1 < len(args):
            return (process / "cwd").resolve() / args[index + 1]
        if arg.startswith("--registry="):
            return (process / "cwd").resolve() / arg.split("=", 1)[1]
    raise ValueError(f"Missing registry in {process}/cmdline")


def wait_for_heartbeat(registry, pid):
    identity = process_identity(pid)
    if not identity:
        raise RuntimeError(f"Replacement PID {pid} exited")
    identity.update(host_identity())
    timeout = positive(
        read_json(registry / "config.json")["defaults"]["command_timeout_seconds"],
        "command_timeout_seconds",
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not same_process(identity):
            raise RuntimeError(f"Replacement PID {pid} exited")
        try:
            heartbeat = read_json(registry / "heartbeat.json")
            if heartbeat.get("pid") == pid and same_process(heartbeat):
                return
        except (FileNotFoundError, WorkflowError):
            # The controller atomically replaces the heartbeat while we read it.
            pass
        time.sleep(0.2)
    raise RuntimeError(f"No heartbeat from PID {pid} in {registry} after {timeout}s")


def main_pid(manager, unit):
    return int(
        subprocess.check_output(
            [*manager, "show", "-p", "MainPID", "--value", "--", unit], text=True
        )
    )


def startup_timeout():
    # Bound even a blocked registry read by the host system manager's startup budget.
    output = subprocess.check_output(
        [
            "busctl",
            "--system",
            "get-property",
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "DefaultTimeoutStartUSec",
        ],
        text=True,
    )
    return int(output.split()[1]) / 1_000_000


def restart_controller(manager, service, manager_uid=None):
    unit, pid = service["Id"], int(service["MainPID"])
    if not pid:
        return
    process = Path("/proc") / str(pid)
    try:
        if manager_uid is not None and process.stat().st_uid != manager_uid:
            return
        registry = controller_registry(process)
    except (FileNotFoundError, ProcessLookupError):
        return
    if registry is None or main_pid(manager, unit) != pid:
        return
    if service["KillMode"] != "process":
        raise RuntimeError(
            f"{unit}: restarting with KillMode={service['KillMode']} could stop its workers"
        )
    print(f"[deploy] Restarting {unit} for {registry} (PID {pid})", flush=True)
    subprocess.run([*manager, "try-restart", "--", unit], check=True)
    new_pid = main_pid(manager, unit)
    if not new_pid or new_pid == pid:
        raise RuntimeError(f"{unit}: no new MainPID after restart")
    # The registry can be private on root-squashed NFS. Read it as the controller.
    process_uid = (Path("/proc") / str(new_pid)).stat().st_uid
    if manager_uid is not None and process_uid != manager_uid:
        raise RuntimeError(f"{unit}: replacement PID belongs to another user")
    owner = pwd.getpwuid(process_uid)
    subprocess.run(
        [sys.executable, "-I", __file__, "--heartbeat", str(registry), str(new_pid)],
        user=owner.pw_uid,
        group=owner.pw_gid,
        extra_groups=os.getgrouplist(owner.pw_name, owner.pw_gid),
        timeout=startup_timeout(),
        check=True,
    )
    if main_pid(manager, unit) != new_pid:
        raise RuntimeError(f"{unit}: MainPID changed during startup")
    print(f"[deploy] {unit}: verified heartbeat from PID {new_pid}", flush=True)


def restart_services():
    if not Path("/run/systemd/system").is_dir():
        return
    system = ["systemctl", "--system"]
    services = running_services(system)
    managers = [(system, None)]
    for service in services:
        match = re.fullmatch(r"user@(\d+)\.service", service["Id"])
        if match:
            managers.append(
                (
                    ["systemctl", "--user", "--machine=" + match[1] + "@.host"],
                    int(match[1]),
                )
            )
    errors = []
    for manager, manager_uid in managers:
        try:
            units = services if manager == system else running_services(manager)
        except subprocess.CalledProcessError as exc:
            errors.append(str(exc))
            continue
        for service in units:
            try:
                restart_controller(manager, service, manager_uid)
            except (
                OSError,
                ValueError,
                RuntimeError,
                subprocess.SubprocessError,
            ) as exc:
                errors.append(f"{service['Id']}: {exc}")
    if errors:
        raise RuntimeError("\n".join(errors))


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--heartbeat":
        wait_for_heartbeat(Path(sys.argv[2]), int(sys.argv[3]))
    elif len(sys.argv) == 1:
        restart_services()
    else:
        sys.exit("Unexpected arguments")
