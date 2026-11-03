"""Sanitized, optional Dagster observation publication and registration.

Nothing in this module submits, cancels, retries or validates compute work.
Controllers publish snapshots; an independent registrar reconciles Dagster runs.
The stable observation UUID binds the Dagster run through exact tags, including
after a lost HTTP response. An observer-only restart uses a new observation UUID
in the same native epoch.
"""

import argparse
import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from uuid import NAMESPACE_URL, UUID, uuid5

from .files import locked, private_directory, read_json, sync_directory, write_json
from .schema import (
    WorkflowError,
    plan_digest,
    positive,
    require,
    uuid_value,
    validate_plan,
)

MAX_OBSERVATIONS = 128
MAX_REGISTRY_ENTRIES = 4096
MAX_HTTP_BYTES = 4 * 1024 * 1024
LOCATION_NAME = "hstream_workflows"
TERMINAL = frozenset(
    {"succeeded", "reused", "unavailable", "failed", "blocked", "cancelled"}
)
STATES = TERMINAL | {
    "waiting",
    "ready",
    "submitting",
    "queued",
    "running",
    "cancelling",
    "recovery",
}
DAGSTER_TERMINAL = frozenset({"SUCCESS", "FAILURE", "CANCELED"})


def configuration(registry):
    root = private_directory(registry)
    config = read_json(root / "config.json")
    require(
        set(config)
        == {
            "schema",
            "poll_interval_ms",
            "observer_timeout_seconds",
            "max_observations",
            "max_concurrent_steps",
            "public_url",
        },
        "Invalid observer registry configuration",
    )
    require(config["schema"] == 1, "Unsupported observer registry schema")
    positive(config["poll_interval_ms"], "observer polling interval", 60000)
    positive(config["observer_timeout_seconds"], "observer timeout", 31 * 86400)
    positive(config["max_observations"], "observer definition limit", MAX_OBSERVATIONS)
    positive(config["max_concurrent_steps"], "observer process limit", 32)
    public = config["public_url"]
    require(public is None or isinstance(public, str), "Invalid public Dagster URL")
    if public is not None:
        parsed = urllib.parse.urlsplit(public)
        require(
            parsed.scheme in {"http", "https"}
            and parsed.hostname
            and not parsed.username
            and not parsed.password
            and not parsed.query
            and not parsed.fragment,
            "Public Dagster URL must be an absolute HTTP(S) address without credentials",
        )
    return root, config


def observation_path(registry, observation_id):
    uuid_value(observation_id, "observation ID")
    root = private_directory(registry)
    observations = private_directory(root / "observations", create=False)
    return private_directory(observations / observation_id)


def observation_ids(registry):
    root, config = configuration(registry)
    observations = root / "observations"
    if not observations.exists():
        return []
    private_directory(observations)
    entries = []
    with os.scandir(observations) as iterator:
        for index, entry in enumerate(iterator):
            require(
                index < MAX_REGISTRY_ENTRIES, "Observer registry entry limit reached"
            )
            if entry.name.startswith(".pending-"):
                continue
            uuid_value(entry.name, "registered observation ID")
            require(
                entry.is_dir(follow_symlinks=False),
                "Invalid observer registry directory",
            )
            record = read_json(
                private_directory(Path(entry.path)) / "registration.json"
            )
            if not record.get("retired", False):
                entries.append(entry.name)
                require(
                    len(entries) <= config["max_observations"],
                    "Too many active observation definitions",
                )
    return sorted(entries)


def safe_job_name(workflow_id, epoch, observation_id):
    return f"observe_{UUID(workflow_id).hex}_e{epoch}_{UUID(observation_id).hex}"


def project_snapshot(plan, snapshot):
    plan = validate_plan(plan)
    require(
        snapshot.get("schema") == 1
        and snapshot.get("workflow_id") == plan["workflow_id"],
        "Observer snapshot workflow mismatch",
    )
    require(
        snapshot.get("plan_digest") == plan_digest(plan),
        "Observer snapshot plan digest mismatch",
    )
    require(
        type(snapshot.get("epoch")) is int and 0 <= snapshot["epoch"] <= 1000000,
        "Invalid execution epoch",
    )
    require(
        type(snapshot.get("revision")) is int and snapshot["revision"] >= 0,
        "Invalid snapshot revision",
    )
    require(snapshot.get("state") in STATES, "Invalid workflow observation state")
    require(isinstance(snapshot.get("nodes"), list), "Expected bounded node snapshots")
    source_nodes = {node["id"]: node for node in snapshot["nodes"]}
    require(
        len(source_nodes) == len(snapshot["nodes"]) == len(plan["stages"]),
        "Observer node membership mismatch",
    )
    stages, nodes = [], []
    for stage in plan["stages"]:
        node = source_nodes.get(stage["id"])
        require(
            node is not None and node.get("state") in STATES, "Invalid observer stage"
        )
        stages.append({key: stage[key] for key in ("id", "label", "dependencies")})
        attempts = []
        require(
            isinstance(node.get("attempts"), list) and len(node["attempts"]) <= 64,
            "Invalid observer attempt history",
        )
        for attempt in node["attempts"]:
            uuid_value(attempt.get("id"), "attempt ID")
            require(
                type(attempt.get("epoch")) is int
                and 0 <= attempt["epoch"] <= snapshot["epoch"],
                "Invalid attempt epoch",
            )
            require(attempt.get("state") in STATES, "Invalid observed attempt state")
            details = attempt.get("details", {})
            metadata = {}
            # Free-form reason/error strings can contain paths or credentials.
            # Keep only scheduler identifiers with a restricted alphabet.
            for key in ("scheduler_id", "cluster", "host"):
                value = details.get(key)
                if value is not None:
                    require(
                        isinstance(value, (int, str)) and not isinstance(value, bool),
                        "Invalid scheduler observation value",
                    )
                    value = str(value)
                    require(
                        len(value) <= 256
                        and re.fullmatch(r"[A-Za-z0-9_.:,\[\]-]*", value),
                        "Unsafe scheduler observation value",
                    )
                    metadata[key] = value
            attempts.append(
                {
                    "id": attempt["id"],
                    "epoch": attempt["epoch"],
                    "state": attempt["state"],
                    "details": metadata,
                }
            )
        nodes.append({"id": stage["id"], "state": node["state"], "attempts": attempts})
    return stages, {
        key: snapshot[key]
        for key in (
            "schema",
            "workflow_id",
            "plan_digest",
            "revision",
            "epoch",
            "state",
        )
    } | {"nodes": nodes}


def publish_snapshot(registry, plan, snapshot, observation_id=None, *, blocking=False):
    """Publish optional operator diagnostics; callers surface errors independently.

    No HTTP calls occur on the workflow coordinator's mutation path. Return the
    observation ID, deterministic for the workflow epoch unless explicitly given.
    """
    root, config = configuration(registry)
    stages, projection = project_snapshot(plan, snapshot)
    workflow_id, epoch = projection["workflow_id"], projection["epoch"]
    observation_id = observation_id or str(
        uuid5(NAMESPACE_URL, f"hstream-observer:{workflow_id}:{epoch}")
    )
    uuid_value(observation_id, "observation ID")
    registration = {
        "schema": 1,
        "workflow_id": workflow_id,
        "plan_digest": projection["plan_digest"],
        "epoch": epoch,
        "observation_id": observation_id,
        "job_name": safe_job_name(workflow_id, epoch, observation_id),
        "stages": stages,
        "retired": False,
    }
    with locked(root / "registry.lock", blocking=blocking):
        observations = private_directory(root / "observations", create=True)
        followers = []
        superseded = []
        for existing_id in observation_ids(root):
            previous_registration, previous_snapshot = read_observation(
                root, existing_id
            )
            if previous_registration["workflow_id"] != workflow_id:
                continue
            require(
                previous_registration["plan_digest"] == registration["plan_digest"],
                "Workflow observation plan identity changed",
            )
            require(
                previous_registration["epoch"] <= epoch,
                "Cannot publish an earlier workflow epoch",
            )
            if previous_registration["epoch"] < epoch:
                superseded.append((existing_id, previous_snapshot))
                continue
            require(
                projection["revision"] >= previous_snapshot["revision"],
                "Stale observation snapshot",
            )
            require(
                projection["revision"] != previous_snapshot["revision"]
                or projection == previous_snapshot,
                "Changed observation snapshot at the same revision",
            )
            followers.append(existing_id)
        directory = observations / observation_id
        retired = False
        if directory.exists():
            private_directory(directory)
            previous_registration = read_json(directory / "registration.json")
            retired = previous_registration.get("retired") is True
            require(
                {**previous_registration, "retired": False} == registration,
                "Observation identity was already registered for another graph",
            )
            previous = read_json(directory / "snapshot.json")
            require(
                projection["revision"] >= previous["revision"],
                "Stale observation snapshot",
            )
            require(
                projection["revision"] != previous["revision"]
                or projection == previous,
                "Changed observation snapshot at the same revision",
            )
        else:
            require(
                len(observation_ids(root)) < config["max_observations"],
                "Retire completed observations before registering more",
            )
            staging = Path(tempfile.mkdtemp(prefix=".pending-", dir=observations))
            try:
                write_json(staging / "registration.json", registration, exclusive=True)
                write_json(staging / "snapshot.json", projection, exclusive=True)
                os.rename(staging, directory)
                sync_directory(observations)
            finally:
                if staging.exists():
                    for filename in ("registration.json", "snapshot.json"):
                        (staging / filename).unlink(missing_ok=True)
                    staging.rmdir()
        if not retired:
            write_json(directory / "snapshot.json", projection)
        # Observer-only restarts have another ID in the same epoch. Every one
        # follows new native snapshots, rather than freezing at its attach time.
        for follower in followers:
            if follower != observation_id:
                write_json(
                    observation_path(root, follower) / "snapshot.json", projection
                )
        for previous_id, previous_snapshot in superseded:
            previous_snapshot["superseded_by_epoch"] = epoch
            write_json(
                observation_path(root, previous_id) / "snapshot.json", previous_snapshot
            )
    return observation_id


def read_observation(registry, observation_id):
    directory = observation_path(registry, observation_id)
    registration = read_json(directory / "registration.json")
    snapshot = read_json(directory / "snapshot.json")
    require(
        registration.get("schema") == snapshot.get("schema") == 1,
        "Unsupported observation schema",
    )
    require(
        registration.get("observation_id") == observation_id, "Observation ID mismatch"
    )
    uuid_value(registration.get("workflow_id"), "workflow ID")
    require(
        type(registration.get("epoch")) is int
        and 0 <= registration["epoch"] <= 1000000,
        "Invalid observation epoch",
    )
    for key in ("workflow_id", "epoch", "plan_digest"):
        require(
            snapshot.get(key) == registration[key],
            "Observation snapshot binding changed",
        )
    stages = registration.get("stages")
    require(
        isinstance(stages, list) and 0 < len(stages) <= 256, "Invalid observed graph"
    )
    require(
        isinstance(snapshot.get("nodes"), list)
        and len(snapshot["nodes"]) == len(stages),
        "Invalid observed graph status",
    )
    stage_ids = {stage["id"] for stage in stages}
    require(
        len(stage_ids) == len(stages)
        and stage_ids == {node["id"] for node in snapshot["nodes"]},
        "Observed graph membership changed",
    )
    for node in snapshot["nodes"]:
        require(
            node.get("state") in STATES
            and isinstance(node.get("attempts"), list)
            and len(node["attempts"]) <= 64,
            "Invalid observed node state",
        )
    return registration, snapshot


def graphql(url, query, variables=None):
    parsed = urllib.parse.urlsplit(url)
    require(
        parsed.scheme in {"http", "https"}
        and parsed.hostname
        and not parsed.username
        and not parsed.password
        and not parsed.query
        and not parsed.fragment,
        "Invalid Dagster endpoint",
    )
    payload = json.dumps({"query": query, "variables": variables or {}}).encode()
    request = urllib.request.Request(
        url.rstrip("/") + "/graphql",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        content = response.read(MAX_HTTP_BYTES + 1)
    require(len(content) <= MAX_HTTP_BYTES, "Dagster response exceeds its bound")
    result = json.loads(content)
    require(
        isinstance(result, dict)
        and not result.get("errors")
        and isinstance(result.get("data"), dict),
        "Dagster GraphQL request failed",
    )
    return result["data"]


RUN_QUERY = """query($id: ID!) { pipelineRunOrError(runId: $id) {
  __typename ... on Run { runId status tags { key value } }
} }"""
RUNS_QUERY = """query($filter: RunsFilter!) { runsOrError(filter: $filter, limit: 2) {
  __typename ... on Runs { results { runId status tags { key value } } }
} }"""
LAUNCH_QUERY = """mutation($params: ExecutionParams!) {
  launchPipelineExecution(executionParams: $params) {
    __typename ... on LaunchRunSuccess { run { runId status tags { key value } } }
} }"""


def observation_tags(registration):
    return {
        "hstream/workflow_id": registration["workflow_id"],
        "hstream/plan_digest": registration["plan_digest"],
        "hstream/epoch": str(registration["epoch"]),
        "hstream/observation_id": registration["observation_id"],
        "hstream/read_only": "true",
    }


def checked_run(registration, value):
    uuid_value(value.get("runId"), "Dagster run ID")
    tags = {item["key"]: item["value"] for item in value.get("tags", [])}
    require(
        all(
            tags.get(key) == expected
            for key, expected in observation_tags(registration).items()
        ),
        "Dagster run tags do not bind this observation",
    )
    return value


def find_run(registration, url, request):
    response = request(
        url,
        RUNS_QUERY,
        {
            "filter": {
                "tags": [
                    {
                        "key": "hstream/observation_id",
                        "value": registration["observation_id"],
                    }
                ]
            }
        },
    )["runsOrError"]
    require(
        response.get("__typename") == "Runs"
        and isinstance(response.get("results"), list),
        "Dagster observation lookup is unavailable",
    )
    require(
        len(response["results"]) <= 1,
        "Multiple Dagster runs claim one observation; reconcile explicitly",
    )
    return (
        checked_run(registration, response["results"][0])
        if response["results"]
        else None
    )


def reconcile_observation(registry, observation_id, url, *, request=graphql):
    """Idempotently attach the exact observation run, including response loss."""
    root, config = configuration(registry)
    with locked(root / "registrar.lock"):
        registration, _ = read_observation(root, observation_id)
        require(not registration.get("retired"), "Observation was retired")
        directory = observation_path(root, observation_id)
        previous = (
            read_json(directory / "launch.json")
            if (directory / "launch.json").exists()
            else None
        )
        if previous and previous.get("run_id"):
            value = request(url, RUN_QUERY, {"id": previous["run_id"]})[
                "pipelineRunOrError"
            ]
            require(
                value.get("__typename") == "Run",
                "Previously accepted Dagster run is unavailable",
            )
            run = checked_run(registration, value)
            require(
                previous["run_id"] == run["runId"],
                "Dagster observation run identity changed",
            )
        else:
            run = find_run(registration, url, request)
        if run is None:
            require(
                previous is None or previous.get("state") == "rejected",
                "Dagster acceptance is unresolved; absence does not authorize resubmission",
            )
            loaded = request(
                url,
                "mutation($name: String!) { reloadRepositoryLocation(repositoryLocationName: $name) { __typename } }",
                {"name": LOCATION_NAME},
            )["reloadRepositoryLocation"]
            require(
                loaded.get("__typename") == "WorkspaceLocationEntry",
                "Dagster observer location reload failed",
            )
            params = {
                "selector": {
                    "repositoryLocationName": LOCATION_NAME,
                    "repositoryName": "__repository__",
                    "jobName": registration["job_name"],
                },
                "runConfigData": {},
                "mode": "default",
                "executionMetadata": {
                    "tags": [
                        {"key": key, "value": value}
                        for key, value in observation_tags(registration).items()
                    ],
                },
            }
            # The pinned GraphQL API assigns its own run UUID. Persist intent
            # before launch and reconcile exact tags after response loss. If no
            # run can be found, keep recovery rather than blindly submitting.
            write_json(
                directory / "launch.json",
                {
                    "schema": 1,
                    "observation_id": observation_id,
                    "state": "submitting",
                    "url": url,
                },
            )
            launched = request(url, LAUNCH_QUERY, {"params": params})[
                "launchPipelineExecution"
            ]
            if launched.get("__typename") != "LaunchRunSuccess":
                run = find_run(registration, url, request)
                if run is None and launched.get("__typename") in {
                    "RunConfigValidationInvalid",
                    "PipelineNotFoundError",
                    "UnauthorizedError",
                }:
                    write_json(
                        directory / "launch.json",
                        {
                            "schema": 1,
                            "observation_id": observation_id,
                            "state": "rejected",
                            "url": url,
                        },
                    )
                require(
                    run is not None, "Dagster observation launch has not been confirmed"
                )
            else:
                run = checked_run(registration, launched["run"])
        record = {
            "schema": 1,
            "observation_id": observation_id,
            "run_id": run["runId"],
            "state": run["status"],
            "url": (
                config["public_url"].rstrip("/") + "/runs/" + run["runId"]
                if config["public_url"]
                else None
            ),
        }
        write_json(directory / "launch.json", record)
        return record


def retire_observation(registry, observation_id):
    """Remove a definition only after confirmed observation and native completion."""
    root, _ = configuration(registry)
    with locked(root / "registrar.lock"), locked(root / "registry.lock"):
        registration, snapshot = read_observation(root, observation_id)
        directory = observation_path(root, observation_id)
        launch = read_json(directory / "launch.json")
        require(
            launch.get("observation_id") == observation_id
            and launch.get("state") in DAGSTER_TERMINAL,
            "Observer shutdown is not confirmed",
        )
        require(
            all(node["state"] in TERMINAL for node in snapshot["nodes"]),
            "Native workflow is still active",
        )
        registration["retired"] = True
        write_json(directory / "registration.json", registration)


def serve(registry, url, *, once=False):
    root, config = configuration(registry)
    while True:
        for observation_id in observation_ids(root):
            try:
                reconcile_observation(root, observation_id, url)
            except (
                WorkflowError,
                OSError,
                ValueError,
                KeyError,
                urllib.error.URLError,
            ) as error:
                # Log identifiers and error type; remote response bodies may
                # contain privileged details and are never emitted here.
                print(
                    f"Observation {observation_id}: {type(error).__name__}; registration will retry",
                    file=sys.stderr,
                    flush=True,
                )
        if once:
            return
        time.sleep(config["poll_interval_ms"] / 1000)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    server = subparsers.add_parser("serve")
    server.add_argument("--registry", type=Path, required=True)
    server.add_argument("--url", required=True)
    server.add_argument("--once", action="store_true")
    retire = subparsers.add_parser("retire")
    retire.add_argument("--registry", type=Path, required=True)
    retire.add_argument("--observation-id", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "serve":
            serve(args.registry, args.url, once=args.once)
        else:
            retire_observation(args.registry, args.observation_id)
    except (WorkflowError, OSError, ValueError, KeyError) as error:
        print(
            f"Dagster observation unavailable: {type(error).__name__}", file=sys.stderr
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
