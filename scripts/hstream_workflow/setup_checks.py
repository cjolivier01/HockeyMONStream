"""Short setup checks: independent ownership and explicit busy-GPU deferral."""

from pathlib import Path

from .files import read_json, write_json


def defer_busy(controller, test, status, *, enabled):
    """Cancel an exactly identified pending allocation; never infer success."""
    if not enabled or status["owner_released"]:
        return status
    path = Path(test["registry"]) / "deferred-gpu.json"
    try:
        intent = read_json(path)
    except FileNotFoundError:
        stages = status["nodes"]
        if len(stages) != 1 or stages[0]["kind"] != "experiment-probe":
            return status
        stage = stages[0]
        if stage["state"] != "queued" or not stage["attempts"]:
            return status
        attempt = stage["attempts"][-1]
        details = attempt.get("details", {})
        if (
            attempt["backend"] != "slurm"
            or details.get("state") != "PENDING"
            # squeue's %R wraps pending reasons in parentheses. Retained
            # observations from other adapters may contain the bare reason.
            or details.get("reason")
            not in {"Resources", "Priority", "(Resources)", "(Priority)"}
            or not details.get("scheduler_id")
            or not details.get("cluster")
        ):
            return status
        intent = {
            "workflow": test["workflow"],
            "attempt": attempt["id"],
            "scheduler_id": details["scheduler_id"],
            "cluster": details["cluster"],
            "reason": details["reason"],
        }
        write_json(path, intent, exclusive=True)
    # A lost cancellation response must resume the same intent, even if the
    # allocation started meanwhile. That race cannot qualify as a deferred check.
    if intent["workflow"] == test["workflow"]:
        return controller.cancel(test["workflow"], status["epoch"])
    return status


def is_deferred(test, status):
    if not status["owner_released"] or len(status["nodes"]) != 1:
        return False
    try:
        intent = read_json(Path(test["registry"]) / "deferred-gpu.json")
    except FileNotFoundError:
        return False
    attempts = status["nodes"][0]["attempts"]
    if not attempts:
        return False
    attempt = attempts[-1]
    details = attempt.get("details", {})
    # No assigned host means this exact Slurm allocation never started. Native
    # failure, unknown acceptance, or a cancellation race remains a real failure.
    return (
        intent.get("workflow") == test["workflow"]
        and intent.get("attempt") == attempt["id"]
        and intent.get("scheduler_id") == details.get("scheduler_id")
        and intent.get("cluster") == details.get("cluster")
        and details.get("state") == "CANCELLED"
        and details.get("host") == ""
        and details.get("never_allocated") is True
        and details.get("terminal") is True
        and details.get("stopped") is True
        and details.get("cleanup_complete") is True
    )
