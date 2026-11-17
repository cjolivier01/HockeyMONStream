"""Human-readable setup activity; never qualification or execution evidence."""

import json
import re
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


def stage_label(stage):
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
    return labels.get(stage["id"], stage.get("label", stage["id"]))


def failure_detail(stage, log, *, finished):
    """Explain this attempt only; log hints never change qualification or cleanup."""
    lines = re.sub(r"\x1b\[[0-9;]*m", "", log[-32768:]).splitlines()
    cause = ""
    context = ""
    # A pipeline bus error is terminal. Earlier SDK errors may have recovered
    # (for example an ONNX CUDA allocation that successfully falls back to CPU).
    for index, line in enumerate(lines):
        if line.startswith("ERROR from "):
            cause = line
            context = "\n".join(lines[index : index + 4]).lower()
            break
    if not cause:
        for line in reversed(lines):
            try:
                error = json.loads(line).get("error", "")
            except (ValueError, AttributeError):
                continue
            if isinstance(error, str) and error and "inspect attempt log" not in error:
                cause = error
                context = error.lower()
                break
    if not cause:
        cause = stage.get("reason") or "The test stopped without a successful result."
        context = cause.lower()
    action = (
        "Open Test details for the full log, correct this problem, then test again."
    )
    allocation = "could not acquire an output buffer" in context
    oom = re.search(r"\bcuda failure: status=2\b", context) is not None or (
        any(marker in context for marker in ("cuda", "gpu", "hip"))
        and any(marker in context for marker in ("out of memory", "outofmemory"))
    )
    if oom:
        cause = "The GPU ran out of available memory."
        action = (
            "Wait for other GPU work on this computer to finish or choose a computer "
            "with more available GPU memory, then test again."
        )
    elif allocation:
        cause = "The GPU could not provide a video buffer. " + cause
    if not finished:
        action = "HStream has not confirmed that this test stopped. Wait for cleanup before testing again."
    # Keep arbitrary native diagnostics readable and bounded in the main banner.
    cause = " ".join(cause.split())[:600]
    outcome = "needs attention" if stage.get("state") == "recovery" else "failed"
    return f"{stage_label(stage)} {outcome}: {cause}\n{action}"


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
                detail = stage_label(active)
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
        if row.get("next"):
            state += "\n    " + row["next"].replace("\n", "\n    ")
            if not row.get("finished"):
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
