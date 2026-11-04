"""Bounded append-only diagnostic history; state.json remains authoritative."""

import json
import os
import time
from pathlib import Path

from .files import read_bytes, sync_directory
from .schema import canonical, require

SEGMENT_BYTES = 1024 * 1024


def _records(path):
    if not path.exists():
        return []
    contents = read_bytes(path, SEGMENT_BYTES)
    # An interrupted final append is not a committed event. Its complete prefix
    # remains readable and is repaired before the next append.
    return [json.loads(line) for line in contents.split(b"\n")[:-1] if line]


def record(directory, plan, state):
    directory = Path(directory)
    path = directory / "events.jsonl"
    rows = _records(path)
    if rows and rows[-1]["revision"] == state["revision"]:
        return
    previous = rows[-1] if rows else None
    require(
        previous is None or previous["revision"] < state["revision"],
        "Event history is ahead of authoritative state",
    )
    event = {
        "schema": 1,
        "workflow_id": plan["workflow_id"],
        "revision": state["revision"],
        "epoch": state["epoch"],
        "time_ns": time.time_ns(),
        "cancel_requested": state["cancel_requested"],
        "nodes": [
            {
                "id": stage_id,
                "state": node["state"],
                "reason": node["reason"][:512],
                "attempt_id": node["attempts"][-1]["id"] if node["attempts"] else None,
            }
            for stage_id, node in state["nodes"].items()
        ],
    }
    contents = canonical(event) + b"\n"
    require(len(contents) <= SEGMENT_BYTES, "Workflow event exceeds its bound")
    existing = read_bytes(path, SEGMENT_BYTES) if path.exists() else b""
    complete = existing.rfind(b"\n") + 1
    if complete + len(contents) > SEGMENT_BYTES:
        os.replace(path, directory / "events.1.jsonl")
        complete = 0
        existing = b""
    descriptor = os.open(
        path,
        os.O_CREAT | os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW | os.O_CLOEXEC,
        0o600,
    )
    try:
        if complete != len(existing):
            os.ftruncate(descriptor, complete)
        written = 0
        while written < len(contents):
            written += os.write(descriptor, contents[written:])
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    sync_directory(directory)


def read(directory, workflow_id, stage_id=None):
    directory = Path(directory)
    records = _records(directory / "events.1.jsonl") + _records(
        directory / "events.jsonl"
    )
    result = []
    for event in records[-512:]:
        require(
            event.get("workflow_id") == workflow_id, "Event belongs to another workflow"
        )
        if stage_id is not None:
            event["nodes"] = [node for node in event["nodes"] if node["id"] == stage_id]
        result.append(event)
    return {"workflow_id": workflow_id, "events": result}
