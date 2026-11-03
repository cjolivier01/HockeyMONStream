"""Read-only Dagster graphs projected from durable HStream workflow snapshots.

Dagster is imported only when loading this optional code location. Re-execution,
failure or cancellation here has no effect on native compute ownership.
"""

import hashlib
import os
import re
import time

from .dagster_registry import configuration, observation_ids, read_observation
from .files import locked
from .schema import require


def operation_name(stage_id, observation_id=""):
    # Dagster requires node-definition names to be unique across every job in
    # a code location, even when each graph has its own scope. Epoch/restart
    # observations capture different snapshot readers and cannot share names.
    suffix = hashlib.sha256((observation_id + "\0" + stage_id).encode()).hexdigest()[
        :12
    ]
    readable = re.sub(r"[^A-Za-z0-9_]", "_", stage_id)[:40]
    return f"stage_{readable}_{suffix}"


def make_job(registry, observation_id):
    from dagster import (
        DependencyDefinition,
        Failure,
        GraphDefinition,
        In,
        Nothing,
        OpDefinition,
        Out,
        Output,
    )

    root, config = configuration(registry)
    registration, _ = read_observation(root, observation_id)
    require(not registration.get("retired"), "Cannot load a retired observation")
    stages = registration["stages"]
    node_names = {
        stage["id"]: operation_name(stage["id"], observation_id) for stage in stages
    }
    dependencies = {}
    operations = []

    def observe(stage_id):
        def compute(context, _inputs):
            deadline = time.monotonic() + config["observer_timeout_seconds"]
            previous = None
            while True:
                current_registration, snapshot = read_observation(root, observation_id)
                require(
                    current_registration == registration,
                    "Observation graph changed during execution",
                )
                node = next(
                    item for item in snapshot["nodes"] if item["id"] == stage_id
                )
                if "superseded_by_epoch" in snapshot and node["state"] not in {
                    "succeeded",
                    "reused",
                    "failed",
                    "unavailable",
                    "blocked",
                    "cancelled",
                }:
                    raise Failure(
                        description="This observation epoch was superseded. Open the latest HStream workflow observation; native work is unchanged.",
                        allow_retries=False,
                    )
                metadata = {
                    "workflow_id": registration["workflow_id"],
                    "stage_id": stage_id,
                    "epoch": registration["epoch"],
                    "native_state": node["state"],
                    "observation_id": observation_id,
                    "read_only": True,
                }
                if node["attempts"]:
                    attempt = node["attempts"][-1]
                    metadata.update(attempt_id=attempt["id"], **attempt["details"])
                if metadata != previous:
                    context.log.info(
                        "Observing native stage %s: %s", stage_id, node["state"]
                    )
                    previous = metadata
                if node["state"] in {"succeeded", "reused"}:
                    yield Output(None, metadata=metadata)
                    return
                if node["state"] in {"failed", "unavailable", "blocked", "cancelled"}:
                    raise Failure(
                        description=f"Native stage {stage_id} is {node['state']}. Inspect the HStream workflow for diagnostics; Dagster actions do not retry compute.",
                        metadata=metadata,
                        allow_retries=False,
                    )
                if time.monotonic() >= deadline:
                    raise Failure(
                        description="Observation deadline elapsed. Native work is unchanged; attach a new observation to this epoch.",
                        metadata=metadata,
                        allow_retries=False,
                    )
                time.sleep(config["poll_interval_ms"] / 1000)

        return compute

    for stage in stages:
        name = node_names[stage["id"]]
        inputs = {
            f"after_{index}": In(Nothing)
            for index, _ in enumerate(stage["dependencies"])
        }
        dependencies[name] = {
            f"after_{index}": DependencyDefinition(node_names[parent])
            for index, parent in enumerate(stage["dependencies"])
        }
        operations.append(
            OpDefinition(
                name=name,
                compute_fn=observe(stage["id"]),
                ins=inputs,
                outs={"result": Out(Nothing)},
                description=f"{stage['label']} — observes HStream stage {stage['id']}; no compute submission or retry authority.",
                tags={"hstream/stage_id": stage["id"]},
            )
        )
    graph = GraphDefinition(
        name=registration["job_name"],
        node_defs=operations,
        dependencies=dependencies,
        description="Read-only HStream workflow observation. Use HStream to cancel or retry native work.",
    )
    return graph.to_job(
        config={
            "execution": {
                "config": {
                    "multiprocess": {"max_concurrent": config["max_concurrent_steps"]}
                }
            }
        }
    )


def load_definitions(registry=None):
    from dagster import Definitions

    root, _ = configuration(registry or os.environ["HSTREAM_DAGSTER_REGISTRY"])
    with locked(root / "registry.lock"):
        jobs = [
            make_job(root, observation_id) for observation_id in observation_ids(root)
        ]
    return Definitions(jobs=jobs)
