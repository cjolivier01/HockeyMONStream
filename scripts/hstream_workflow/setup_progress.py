"""Human-readable setup activity; never qualification or execution evidence."""

import json
import sys
import threading

PREFIX = "HSTREAM_SETUP_PROGRESS "
_lock = threading.Lock()


def emit(message):
    with _lock:
        print(
            PREFIX + json.dumps({"message": str(message)}), file=sys.stderr, flush=True
        )


def forward(line):
    if not line.startswith(PREFIX):
        return False
    try:
        value = json.loads(line[len(PREFIX) :])
    except ValueError:
        return False
    if not isinstance(value, dict) or not isinstance(value.get("message"), str):
        return False
    emit(value["message"])
    return True


def summary(rows):
    """One presentation for both clients, keeping computer and profile counts distinct."""
    computers = list(dict.fromkeys(row["node"] for row in rows))
    passed = sum(bool(row["passed"]) for row in rows)
    complete = sum(
        all(row["passed"] for row in rows if row["node"] == node) for node in computers
    )
    lines = [
        f"{complete} of {len(computers)} computers ready · {passed} of {len(rows)} checks passed · {len(rows) - passed} remaining."
    ]
    if len(rows) > len(computers):
        lines.append(
            "Some computers have several GPU choices. Each choice needs its own check."
        )
    current, next_check = "", ""
    for index, row in enumerate(rows, 1):
        name = f"{row['node']} · {row.get('label', row['profile'])}"
        title = f"Check {index} of {len(rows)} · computer {computers.index(row['node']) + 1} of {len(computers)} · {name}"
        status = row.get("status", {})
        stages = status.get("nodes", [])
        active = next(
            (
                stage
                for stage in stages
                if stage["state"]
                in {
                    "running",
                    "queued",
                    "submitting",
                    "recovery",
                    "cancelling",
                    "ready",
                }
            ),
            None,
        )
        if row["passed"]:
            state = "Passed"
        elif row["state"] == "not-tested":
            state = "Waiting for its turn"
        elif row["state"] == "cancelled" and row.get("finished"):
            state = "Stopped"
        elif row.get("finished"):
            state = "Needs attention"
        else:
            state = "Finishing cleanup"
        if status and not row.get("finished"):
            if active:
                done = sum(stage["state"] == "succeeded" for stage in stages)
                labels = {
                    "prepare": "Prepare a private recording",
                    "prepare-target": "Check the recording on this computer",
                    "baseline": "Read sample frames",
                    "scan": "Find matching frames",
                    "materialize": "Save the frame selection",
                    "reuse": "Prepare calibration",
                    "reuse-target": "Load calibration inputs",
                    "calibrate": "Test stitching",
                    "complete-target": "Verify stitching results",
                    "complete": "Finish and clean up",
                }
                detail = labels.get(active["id"], active.get("label", active["id"]))
                reason = active.get("reason", "")
                if active["state"] == "queued":
                    hosts = active.get("resources", {}).get("nodes", [])
                    state = (
                        "Waiting for Slurm to make " + ", ".join(hosts) + " available"
                        if hosts
                        else "Waiting for Slurm resources for: " + detail
                    )
                elif active["state"] == "ready":
                    state = "Waiting to start the next stage"
                else:
                    state = detail
                state += f" · {done} of {len(stages)} stages complete"
                if reason:
                    state += f"\n    {reason}"
                if active["state"] == "queued":
                    state += "\n    This stage has not started on the computer. You can wait or stop testing; passed checks are kept."
            current = title + "\n" + state
        if not row["passed"] and not next_check:
            next_check = title
        lines.append(f"{index}. {name}: {state}")
    return {
        "summary": "\n".join(lines),
        "current": current,
        "next": next_check,
        "passed": passed,
        "total": len(rows),
    }
