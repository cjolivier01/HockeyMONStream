"""Pure workflow state transitions shared by standalone and web controllers."""

import copy

from .schema import MAX_ATTEMPTS, canonical, plan_digest, require, uuid_value

ACTIVE = frozenset({"submitting", "queued", "running", "cancelling", "recovery"})
SUCCESS = frozenset({"succeeded", "reused"})
UNSUCCESSFUL = frozenset({"failed", "unavailable", "blocked", "cancelled"})
TERMINAL = SUCCESS | UNSUCCESSFUL
STATES = ACTIVE | TERMINAL | {"waiting", "ready"}


def initial_state(plan):
    return {
        "schema": 1,
        "workflow_id": plan["workflow_id"],
        "plan_digest": plan_digest(plan),
        "revision": 0,
        "epoch": 0,
        "cancel_requested": False,
        "nodes": {
            stage["id"]: {
                "state": "waiting",
                "reason": "",
                "attempts": [],
                "result": None,
            }
            for stage in plan["stages"]
        },
    }


def validate_state(plan, state):
    require(isinstance(state, dict), "Expected workflow state")
    canonical(state)
    require(state.get("schema") == 1, "Unsupported workflow state schema")
    require(
        state.get("workflow_id") == plan["workflow_id"], "Workflow state owner changed"
    )
    require(state.get("plan_digest") == plan_digest(plan), "Workflow plan changed")
    require(
        type(state.get("revision")) is int and state["revision"] >= 0,
        "Invalid state revision",
    )
    require(
        type(state.get("epoch")) is int and state["epoch"] >= 0,
        "Invalid execution epoch",
    )
    require(type(state.get("cancel_requested")) is bool, "Invalid cancellation intent")
    require(isinstance(state.get("nodes"), dict), "Missing stage states")
    require(
        set(state["nodes"]) == {stage["id"] for stage in plan["stages"]},
        "Stage state membership changed",
    )
    attempt_ids = set()
    for node in state["nodes"].values():
        require(
            isinstance(node, dict) and node.get("state") in STATES,
            "Invalid stage state",
        )
        require(
            isinstance(node.get("reason"), str) and len(node["reason"]) <= 8192,
            "Invalid stage reason",
        )
        attempts = node.get("attempts")
        require(
            isinstance(attempts, list) and len(attempts) <= MAX_ATTEMPTS,
            "Invalid attempt history",
        )
        for index, attempt in enumerate(attempts):
            require(isinstance(attempt, dict), "Invalid attempt")
            key = uuid_value(attempt.get("id"), "attempt ID")
            require(key not in attempt_ids, "Duplicate attempt identity")
            attempt_ids.add(key)
            require(attempt.get("state") in ACTIVE | TERMINAL, "Invalid attempt state")
            require(
                type(attempt.get("epoch")) is int
                and 0 <= attempt["epoch"] <= state["epoch"],
                "Invalid attempt epoch",
            )
            require(
                index == len(attempts) - 1 or attempt["state"] not in ACTIVE,
                "An older attempt remains active",
            )
        if node["state"] in ACTIVE:
            require(
                attempts and attempts[-1]["state"] == node["state"],
                "Active stage has no matching attempt",
            )
    return state


def _copy(plan, state):
    return copy.deepcopy(validate_state(plan, state))


def _commit(previous, current):
    if current != previous:
        current["revision"] = previous["revision"] + 1
    return current


def refresh(plan, state):
    current = _copy(plan, state)
    changed = True
    while changed:
        changed = False
        for stage in plan["stages"]:
            node = current["nodes"][stage["id"]]
            if node["state"] not in {"waiting", "ready"}:
                continue
            before = (node["state"], node["reason"])
            if current["cancel_requested"]:
                node.update(state="cancelled", reason="Cancelled before admission")
            else:
                failed = [
                    parent
                    for parent in stage["dependencies"]
                    if current["nodes"][parent]["state"] in UNSUCCESSFUL
                ]
                waiting = [
                    parent
                    for parent in stage["dependencies"]
                    if current["nodes"][parent]["state"] not in SUCCESS
                ]
                if failed:
                    node.update(
                        state="blocked",
                        reason="Prerequisite did not succeed: " + ", ".join(failed),
                    )
                elif waiting:
                    node.update(
                        state="waiting", reason="Waiting for: " + ", ".join(waiting)
                    )
                else:
                    node.update(state="ready", reason="Waiting for admission")
            changed = changed or before != (node["state"], node["reason"])
    return _commit(state, current)


def ready_stages(plan, state):
    current = refresh(plan, state)
    if current["cancel_requested"]:
        return []
    active = sum(node["state"] in ACTIVE for node in current["nodes"].values())
    available = max(0, plan["limits"]["max_parallel"] - active)
    # Admission may skip a saturated resource pool and still run a later CPU or
    # other-GPU stage. begin_attempt rechecks the per-workflow limit each time.
    if available == 0:
        return []
    return [
        copy.deepcopy(stage)
        for stage in plan["stages"]
        if current["nodes"][stage["id"]]["state"] == "ready"
    ]


def begin_attempt(plan, state, stage_id, attempt_id, request_digest, backend):
    current = refresh(plan, state)
    require(
        stage_id in {stage["id"] for stage in ready_stages(plan, current)},
        "Stage is not ready for admission",
    )
    uuid_value(attempt_id, "attempt ID")
    require(backend in {"local", "slurm"}, "Invalid attempt backend")
    require(
        isinstance(request_digest, str)
        and len(request_digest) == 64
        and all(c in "0123456789abcdef" for c in request_digest),
        "Invalid request digest",
    )
    require(
        not any(
            attempt["id"] == attempt_id
            for node in current["nodes"].values()
            for attempt in node["attempts"]
        ),
        "Attempt ID was already used",
    )
    node = current["nodes"][stage_id]
    require(len(node["attempts"]) < MAX_ATTEMPTS, "Attempt history limit reached")
    node["attempts"].append(
        {
            "id": attempt_id,
            "epoch": current["epoch"],
            "state": "submitting",
            "request_digest": request_digest,
            "backend": backend,
            "details": {},
        }
    )
    node.update(state="submitting", reason="Submitting task", result=None)
    return _commit(state, current)


def update_attempt(plan, state, stage_id, attempt_id, outcome, details=None):
    current = _copy(plan, state)
    require(stage_id in current["nodes"], "Unknown stage")
    node = current["nodes"][stage_id]
    require(
        node["attempts"] and node["attempts"][-1]["id"] == attempt_id,
        "Stale attempt result",
    )
    attempt = node["attempts"][-1]
    require(outcome in ACTIVE | TERMINAL - {"blocked"}, "Invalid attempt outcome")
    details = copy.deepcopy(details or {})
    require(isinstance(details, dict), "Invalid attempt details")
    canonical(details)
    if attempt["state"] in TERMINAL:
        require(
            outcome == attempt["state"]
            and all(attempt["details"].get(k) == v for k, v in details.items()),
            "Terminal attempt cannot change",
        )
        return current
    if outcome in TERMINAL:
        require(
            details.get("stopped") is True and details.get("cleanup_complete") is True,
            "Terminal attempt needs shutdown and cleanup proof",
        )
        if current["cancel_requested"]:
            outcome = "cancelled"
        elif outcome in SUCCESS:
            require(
                details.get("verified") is True
                and isinstance(details.get("outputs"), dict),
                "Success requires verified native outputs",
            )
    if current["cancel_requested"] and outcome in ACTIVE and outcome != "recovery":
        outcome = "cancelling"
    # Reconciliation may move recovery back to an observed active state. It may
    # never downgrade a known running task to a new submission/queue intent.
    if attempt["state"] in {"running", "cancelling"}:
        require(
            outcome not in {"submitting", "queued"}, "Cannot rewind a running attempt"
        )
    attempt["details"].update(details)
    attempt["state"] = outcome
    node["state"] = outcome
    node["reason"] = str(details.get("reason", outcome))[:8192]
    if outcome in SUCCESS:
        node["result"] = copy.deepcopy(details["outputs"])
    elif outcome in TERMINAL:
        node["result"] = None
    return refresh(plan, _commit(state, current))


def cancel(plan, state):
    current = _copy(plan, state)
    current["cancel_requested"] = True
    for node in current["nodes"].values():
        if node["state"] in ACTIVE and node["state"] != "recovery":
            node["state"] = "cancelling"
            node["attempts"][-1]["state"] = "cancelling"
            node["reason"] = "Cancellation requested; waiting for shutdown"
    return refresh(plan, _commit(state, current))


def retry(plan, state, stage_ids):
    current = _copy(plan, state)
    require(
        not any(node["state"] in ACTIVE for node in current["nodes"].values()),
        "Wait for every attempt to stop before retrying",
    )
    require(
        isinstance(stage_ids, list)
        and stage_ids
        and len(stage_ids) == len(set(stage_ids)),
        "Choose distinct failed stages to retry",
    )
    for key in stage_ids:
        require(
            key in current["nodes"] and current["nodes"][key]["state"] in UNSUCCESSFUL,
            "Only unsuccessful stages can be retried",
        )
    reset = set(stage_ids)
    changed = True
    while changed:
        changed = False
        for stage in plan["stages"]:
            if (
                stage["id"] not in reset
                and current["nodes"][stage["id"]]["state"] in {"blocked", "cancelled"}
                and any(parent in reset for parent in stage["dependencies"])
            ):
                reset.add(stage["id"])
                changed = True
    current["epoch"] += 1
    current["cancel_requested"] = False
    for key in reset:
        current["nodes"][key].update(
            state="waiting", reason="Retry requested", result=None
        )
    return refresh(plan, _commit(state, current))


def snapshot(plan, state):
    current = _copy(plan, state)
    states = [node["state"] for node in current["nodes"].values()]
    if "recovery" in states:
        outcome = "recovery"
    elif any(item in ACTIVE for item in states):
        outcome = "cancelling" if current["cancel_requested"] else "running"
    elif any(item not in TERMINAL for item in states):
        outcome = "queued"
    elif all(item in SUCCESS for item in states):
        outcome = "succeeded"
    elif current["cancel_requested"]:
        outcome = "cancelled"
    else:
        outcome = "failed"
    return {
        **{
            key: copy.deepcopy(value)
            for key, value in current.items()
            if key != "nodes"
        },
        "state": outcome,
        "nodes": [
            {
                **{
                    key: copy.deepcopy(stage[key])
                    for key in ("id", "kind", "label", "candidate_id", "dependencies")
                },
                **copy.deepcopy(current["nodes"][stage["id"]]),
            }
            for stage in plan["stages"]
        ],
    }
