"""Private workflow registry, atomic state and shared capacity reservations."""

import os
from pathlib import Path

from .files import locked, private_directory, read_json, write_json
from .schema import digest, require, uuid_value


class Registry:
    def __init__(self, root):
        self.root = private_directory(root)

    def lock(self):
        return locked(self.root / "registry.lock")

    def read(self):
        path = self.root / "registry.json"
        if not path.exists():
            return {"schema": 1, "workflows": {}, "reservations": {}}
        value = read_json(path)
        require(
            set(value) == {"schema", "workflows", "reservations"}
            and value["schema"] == 1,
            "Invalid workflow registry",
        )
        require(
            isinstance(value["workflows"], dict) and len(value["workflows"]) <= 4096,
            "Invalid workflow registry members",
        )
        require(
            isinstance(value["reservations"], dict)
            and len(value["reservations"]) <= 4096,
            "Invalid resource reservation ledger",
        )
        return value

    def write(self, value):
        write_json(self.root / "registry.json", value)

    def lookup(self, workflow_id):
        uuid_value(workflow_id, "workflow ID")
        with self.lock():
            entry = self.read()["workflows"].get(workflow_id)
        require(entry is not None, "Unknown workflow")
        directory = private_directory(entry["directory"])
        require(
            directory == Path(entry["game"]) / "_hockeymon/workflows" / workflow_id,
            "Workflow registry directory changed",
        )
        return directory, entry

    def reserve(
        self, workflow_id, stage_id, attempt_id, profile_id, profile, request_digest
    ):
        """Call while holding this workflow's lock; never launch under registry lock."""
        with self.lock():
            ledger = self.read()
            existing = ledger["reservations"].get(attempt_id)
            if existing:
                require(
                    existing["workflow_id"] == workflow_id
                    and existing["stage_id"] == stage_id
                    and existing["request_digest"] == request_digest,
                    "Reservation identity changed",
                )
                return existing
            require(
                workflow_id in ledger["workflows"],
                "Cannot reserve an unregistered workflow",
            )
            active = [
                row
                for row in ledger["reservations"].values()
                if row["pool"] == profile["pool"]
            ]
            if (
                len(active) >= profile["limits"]["concurrency"]
                or sum(row["user"] == os.getuid() for row in active)
                >= profile["limits"]["per_user"]
            ):
                return None
            device = None
            if profile["executor"] == "local" and profile["resources"]["gpus"]:
                # Device exclusion spans pool aliases and even differently named
                # pools in this registry: one physical GPU cannot be leased twice.
                occupied = {
                    row["device"]
                    for row in ledger["reservations"].values()
                    if row["device"]
                }
                available = [
                    item
                    for item in profile["resources"]["devices"]
                    if item not in occupied
                ]
                if not available:
                    return None
                device = available[0]
            row = {
                "workflow_id": workflow_id,
                "stage_id": stage_id,
                "attempt_id": attempt_id,
                "profile_id": profile_id,
                "profile_digest": digest(profile),
                "pool": profile["pool"],
                "user": os.getuid(),
                "device": device,
                "request_digest": request_digest,
            }
            ledger["reservations"][attempt_id] = row
            self.write(ledger)
            return row

    def release(self, workflow_id, attempt_id, *, stopped, cleanup_complete):
        require(
            stopped is True and cleanup_complete is True,
            "Capacity release requires shutdown and cleanup proof",
        )
        with self.lock():
            ledger = self.read()
            reservation = ledger["reservations"].get(attempt_id)
            if reservation is None:
                return
            require(
                reservation["workflow_id"] == workflow_id,
                "Cannot release another workflow's reservation",
            )
            del ledger["reservations"][attempt_id]
            self.write(ledger)


def attempt_directory(directory, attempt_id):
    uuid_value(attempt_id, "attempt ID")
    return private_directory(Path(directory) / "attempts" / attempt_id, create=True)
