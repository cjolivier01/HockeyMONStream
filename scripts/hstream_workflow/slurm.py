"""Slurm argv adapter. Ambiguous acceptance is always retained for reconciliation."""

import os
import pwd
import re
import shlex
import subprocess
from pathlib import Path

from .files import read_json, sync_directory, write_json
from .schema import WorkflowError, require

JOB_ID = re.compile(r"[1-9][0-9]{0,19}\Z")
TERMINAL = {
    "COMPLETED",
    "CANCELLED",
    "FAILED",
    "TIMEOUT",
    "NODE_FAIL",
    "OUT_OF_MEMORY",
    "PREEMPTED",
    "BOOT_FAIL",
    "DEADLINE",
    "REVOKED",
}


def run_command(arguments, timeout):
    return subprocess.run(
        arguments,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=True,
        env={**os.environ, "LC_ALL": "C", "TZ": "UTC"},
    )


def submit(root, profile, supervisor_command, timeout):
    root = Path(root)
    intent = read_json(root / "submission.json")
    require(intent["backend"] == "slurm", "Submission intent has wrong backend")
    configuration = run_command(["scontrol", "show", "config"], timeout).stdout
    cluster = re.search(
        r"^ClusterName\s*=\s*([A-Za-z0-9_.-]+)\s*$", configuration, re.MULTILINE
    )
    require(cluster is not None, "Slurm cluster identity is unavailable")
    require(
        re.search(
            r"^ProctrackType\s*=\s*proctrack/cgroup\s*$", configuration, re.MULTILINE
        ),
        "Slurm needs cgroup process containment",
    )
    require(
        re.search(r"^TaskPlugin\s*=.*task/cgroup", configuration, re.MULTILINE),
        "Slurm needs cgroup task containment",
    )
    intent.update(cluster=cluster.group(1), user=pwd.getpwuid(os.getuid()).pw_name)
    write_json(root / "submission.json", intent)
    script = root / "job.sh"
    contents = (
        "#!/usr/bin/env bash\nset -euo pipefail\numask 077\nexec "
        + shlex.join(supervisor_command)
        + "\n"
    )
    # Intent has already been fsynced. A second submit for this intent is forbidden;
    # callers reconcile its unique name before considering any external effect.
    with script.open("x", encoding="utf-8") as output:
        output.write(contents)
        output.flush()
        os.fsync(output.fileno())
    os.chmod(script, 0o700)
    sync_directory(root)
    resource = profile["resources"]
    args = [
        "sbatch",
        "--parsable",
        "--clusters=" + intent["cluster"],
        "--no-requeue",
        "--nodes=1",
        "--ntasks=1",
        "--job-name=" + intent["name"],
        "--comment=" + intent["attempt_id"],
        "--output=" + str(root / "slurm.log"),
        "--error=" + str(root / "slurm.log"),
        "--chdir=" + str(root),
        "--cpus-per-task=" + str(resource["cpus"]),
        "--mem=" + str(resource["memory_mb"]) + "M",
        "--time=" + str((resource["time_seconds"] + 59) // 60),
        "--partition=" + resource["partition"],
        "--nodelist=" + ",".join(resource["nodes"]),
    ]
    for key in ("gres", "constraint", "account", "qos"):
        if resource[key]:
            args.append("--" + key + "=" + resource[key])
    write_json(
        root / "dispatch.json",
        {"attempt_id": intent["attempt_id"], "backend": "slurm"},
        exclusive=True,
    )
    result = run_command(args + [str(script)], timeout)
    fields = result.stdout.strip().split(";")
    require(
        len(fields) in {1, 2} and JOB_ID.fullmatch(fields[0]),
        "Slurm acceptance is unresolved: invalid submission response",
    )
    if len(fields) == 2:
        require(
            re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", fields[1]),
            "Invalid Slurm cluster response",
        )
    require(
        len(fields) == 1 or fields[1] == intent["cluster"],
        "Submission returned another Slurm cluster",
    )
    launch = {
        "schema": 1,
        "attempt_id": intent["attempt_id"],
        "scheduler_id": fields[0],
        "cluster": intent["cluster"],
    }
    write_json(root / "launch.json", launch, exclusive=True)
    return launch


def lookup(root, timeout):
    root = Path(root)
    intent = read_json(root / "submission.json")
    launch = (
        read_json(root / "launch.json") if (root / "launch.json").exists() else None
    )
    require(
        isinstance(intent.get("cluster"), str)
        and re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", intent["cluster"]),
        "Submission cluster is unresolved",
    )
    require(
        intent.get("user") == pwd.getpwuid(os.getuid()).pw_name,
        "Submission owner changed",
    )
    require(
        launch is None
        or (
            launch.get("attempt_id") == intent["attempt_id"]
            and launch.get("cluster") == intent["cluster"]
        ),
        "Launch identity changed",
    )
    cluster_args = ["--clusters=" + intent["cluster"], "--user=" + intent["user"]]
    matches, errors = {}, []
    try:
        output = run_command(
            [
                "squeue",
                "--noheader",
                "--name=" + intent["name"],
                "--format=%i|%j|%T|%R|%N|%u|%k",
                *cluster_args,
            ],
            timeout,
        ).stdout
        for line in output.splitlines():
            fields = line.strip().split("|")
            if (
                len(fields) != 7
                or fields[1] != intent["name"]
                or not JOB_ID.fullmatch(fields[0])
            ):
                continue
            require(
                fields[5] == intent["user"] and fields[6] == intent["attempt_id"],
                "Queue job identity changed",
            )
            matches[fields[0]] = {
                "scheduler_id": fields[0],
                "state": fields[2],
                "reason": fields[3],
                "host": (
                    ""
                    if fields[4] in {"(null)", "None", "Unknown", "N/A"}
                    else fields[4]
                ),
                "terminal": False,
            }
    except (OSError, subprocess.SubprocessError) as exc:
        errors.append("Queue lookup unavailable: " + str(exc))
    # Always query accounting, including when queue lookup failed or returned no row.
    try:
        output = run_command(
            [
                "sacct",
                "--noheader",
                "--parsable2",
                "--allocations",
                "--name=" + intent["name"],
                "--starttime=" + intent["submitted_after"],
                "--format=JobIDRaw,JobName%128,State,ExitCode,NodeList,User%128,Comment%128,Cluster%128",
                *cluster_args,
            ],
            timeout,
        ).stdout
        for line in output.splitlines():
            fields = line.strip().split("|")
            if (
                len(fields) < 8
                or fields[1] != intent["name"]
                or not JOB_ID.fullmatch(fields[0])
            ):
                continue
            require(
                fields[5] == intent["user"]
                and fields[6] in {"", intent["attempt_id"]}
                and fields[7] == intent["cluster"],
                "Accounting job identity changed",
            )
            state = fields[2].split()[0].rstrip("+")
            row = {
                "scheduler_id": fields[0],
                "state": state,
                "exit_code": fields[3],
                "host": (
                    ""
                    if fields[4] in {"(null)", "None", "Unknown", "N/A"}
                    else fields[4]
                ),
                "terminal": state in TERMINAL,
                "reason": state,
            }
            if row["terminal"] or fields[0] not in matches:
                matches[fields[0]] = row
    except (OSError, subprocess.SubprocessError) as exc:
        errors.append("Accounting lookup unavailable: " + str(exc))
    require(
        len(matches) <= 1,
        "Multiple scheduler jobs match one attempt; recovery is required",
    )
    if not matches:
        raise WorkflowError(
            "Submission acceptance remains unresolved. " + "; ".join(errors)
        )
    row = next(iter(matches.values()))
    require(
        launch is None or launch["scheduler_id"] == row["scheduler_id"],
        "Scheduler job identity changed",
    )
    row["cluster"] = intent["cluster"]
    if launch is None:
        write_json(
            root / "launch.json",
            {
                "schema": 1,
                "attempt_id": intent["attempt_id"],
                "scheduler_id": row["scheduler_id"],
                "cluster": intent["cluster"],
            },
            exclusive=True,
        )
    return row


def cancel(root, timeout):
    row = lookup(root, timeout)
    if not row["terminal"]:
        args = ["scancel"]
        if row["cluster"]:
            args.append("--clusters=" + row["cluster"])
        run_command([*args, row["scheduler_id"]], timeout)
    return row
