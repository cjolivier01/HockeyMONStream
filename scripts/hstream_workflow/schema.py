"""Portable, dependency-free workflow plan validation.

This file and graph.py are synchronized into the web adapter. Keep I/O out of both.
"""

import copy
import hashlib
import json
import re
from pathlib import Path
from uuid import UUID

MAX_STAGES = 256
# A qualified multi-profile catalog repeats the pinned runtime per compute node.
# Keep one bounded document size for catalogs, plans and controller responses.
MAX_PLAN_BYTES = 8 * 1024 * 1024
MAX_ATTEMPTS = 64
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")


class WorkflowError(ValueError):
    """The request cannot safely change this workflow."""


def require(condition, message):
    if not condition:
        raise WorkflowError(message)


def canonical(value):
    try:
        data = json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode("utf-8")
    except (ValueError, TypeError, RecursionError) as exc:
        raise WorkflowError("Expected bounded JSON values") from exc
    require(len(data) <= MAX_PLAN_BYTES, "Workflow document exceeds its byte bound")
    return data


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def identifier(value, field):
    require(isinstance(value, str) and IDENTIFIER.fullmatch(value), f"Invalid {field}")
    return value


def uuid_value(value, field):
    try:
        require(
            isinstance(value, str) and str(UUID(value)) == value, f"Invalid {field}"
        )
    except (ValueError, AttributeError) as exc:
        raise WorkflowError(f"Invalid {field}") from exc
    return value


def positive(value, field, maximum=1000000):
    require(type(value) is int and 1 <= value <= maximum, f"Invalid {field}")
    return value


def text(value, field, maximum=4096, empty=False):
    require(
        isinstance(value, str)
        and (empty or bool(value))
        and len(value) <= maximum
        and "\0" not in value,
        f"Invalid {field}",
    )
    return value


def references(value):
    """Yield only explicit predecessor-output references, without executing them."""
    if isinstance(value, dict):
        if set(value) == {"stage", "output"}:
            yield value
        else:
            for child in value.values():
                yield from references(child)
    elif isinstance(value, list):
        for child in value:
            yield from references(child)


def validate_plan(value):
    require(isinstance(value, dict), "Expected workflow plan object")
    canonical(value)
    required = {
        "schema",
        "workflow_id",
        "kind",
        "source_identity",
        "configuration_digest",
        "runtime_identity",
        "profiles",
        "stages",
        "limits",
    }
    require(set(value) == required, "Unknown or missing workflow plan fields")
    require(
        type(value["schema"]) is int and value["schema"] == 1,
        "Unsupported workflow plan schema",
    )
    uuid_value(value["workflow_id"], "workflow ID")
    identifier(value["kind"], "workflow kind")
    require(
        isinstance(value["source_identity"], dict), "Expected frozen source identity"
    )
    require(
        isinstance(value["runtime_identity"], dict), "Expected frozen runtime identity"
    )
    require(
        isinstance(value["configuration_digest"], str)
        and DIGEST.fullmatch(value["configuration_digest"]),
        "Invalid configuration digest",
    )
    profiles = value["profiles"]
    require(
        isinstance(profiles, dict) and 1 <= len(profiles) <= 64,
        "Invalid resource profiles",
    )
    for key, profile in profiles.items():
        identifier(key, "profile ID")
        require(isinstance(profile, dict), "Expected resource profile object")
        require(profile.get("executor") in {"local", "slurm"}, "Unsupported executor")
        capabilities = profile.get("capabilities")
        require(
            isinstance(capabilities, list) and 1 <= len(capabilities) <= 64,
            "Missing task capabilities",
        )
        for kind in capabilities:
            identifier(kind, "task capability")
    require(
        isinstance(value["limits"], dict) and set(value["limits"]) == {"max_parallel"},
        "Invalid workflow limits",
    )
    positive(value["limits"]["max_parallel"], "parallel stage limit", MAX_STAGES)
    stages = value["stages"]
    require(
        isinstance(stages, list) and 1 <= len(stages) <= MAX_STAGES,
        "Invalid stage count",
    )
    by_id = {}
    stage_keys = {
        "id",
        "kind",
        "label",
        "candidate_id",
        "dependencies",
        "inputs",
        "profile_id",
    }
    for stage in stages:
        require(
            isinstance(stage, dict) and set(stage) == stage_keys,
            "Unknown or missing stage fields",
        )
        key = identifier(stage["id"], "stage ID")
        require(key not in by_id, "Duplicate stage ID")
        identifier(stage["kind"], "task kind")
        text(stage["label"], "stage label", 256)
        text(stage["candidate_id"], "candidate ID", 256, empty=True)
        identifier(stage["profile_id"], "stage profile ID")
        require(stage["profile_id"] in profiles, "Unknown stage resource profile")
        require(
            stage["kind"] in profiles[stage["profile_id"]]["capabilities"],
            "Task is unsupported by its resource profile",
        )
        require(isinstance(stage["inputs"], dict), "Expected typed task inputs")
        dependencies = stage["dependencies"]
        require(
            isinstance(dependencies, list) and len(dependencies) <= MAX_STAGES,
            "Invalid dependencies",
        )
        for parent in dependencies:
            identifier(parent, "dependency ID")
        require(len(set(dependencies)) == len(dependencies), "Duplicate dependency")
        by_id[key] = stage
    visited, visiting = set(), set()

    def visit(key):
        require(key in by_id, "Unknown dependency")
        require(key not in visiting, "Workflow contains a dependency cycle")
        if key in visited:
            return
        visiting.add(key)
        for parent in by_id[key]["dependencies"]:
            visit(parent)
        visiting.remove(key)
        visited.add(key)

    for key, stage in by_id.items():
        visit(key)
        for reference in references(stage["inputs"]):
            identifier(reference["output"], "output reference")
            require(
                reference["stage"] in stage["dependencies"],
                "Input reference is not a direct prerequisite",
            )
    return copy.deepcopy(value)


def plan_digest(plan):
    return digest(validate_plan(plan))


RESOURCE_FIELDS = {
    "gpus",
    "cpus",
    "memory_mb",
    "time_seconds",
    "partition",
    "gres",
    "nodes",
    "constraint",
    "account",
    "qos",
    "devices",
}
SLURM_VALUE = re.compile(r"[A-Za-z0-9_.:/,+*=-]*\Z")


def validate_profile(profile):
    require(isinstance(profile, dict), "Expected profile object")
    required = {
        "executor",
        "pool",
        "task_command",
        "runtime",
        "capabilities",
        "resources",
        "limits",
        "qualified_nodes",
    }
    require(
        required <= set(profile) <= required | {"label"},
        "Unknown or missing profile fields",
    )
    require(profile["executor"] in {"local", "slurm"}, "Unsupported profile executor")
    identifier(profile["pool"], "resource pool")
    command = profile["task_command"]
    require(
        isinstance(command, list) and 1 <= len(command) <= 8,
        "Invalid native task command",
    )
    for arg in command:
        text(arg, "native task argument")
        require("\n" not in arg and "\r" not in arg, "Invalid native task argument")
    require(
        Path(command[0]).is_absolute(),
        "Native task command must be an installed absolute path",
    )
    capabilities = profile["capabilities"]
    require(
        isinstance(capabilities, list) and capabilities,
        "Profile has no qualified capabilities",
    )
    for kind in capabilities:
        identifier(kind, "task capability")
    resources = profile["resources"]
    require(
        isinstance(resources, dict) and set(resources) == RESOURCE_FIELDS,
        "Unknown or missing resource fields",
    )
    require(
        type(resources["gpus"]) is int and resources["gpus"] in {0, 1},
        "Each stage requests zero or one GPU",
    )
    for key in ("cpus", "memory_mb", "time_seconds"):
        positive(resources[key], key, 2147483647)
    for key in ("partition", "gres", "constraint", "account", "qos"):
        text(resources[key], key, 256, empty=True)
        require(SLURM_VALUE.fullmatch(resources[key]), f"Invalid Slurm {key}")
    for key in ("nodes", "devices"):
        values = resources[key]
        require(
            isinstance(values, list)
            and len(values) <= 256
            and all(isinstance(value, str) for value in values)
            and len(set(values)) == len(values),
            f"Invalid {key}",
        )
        for value in values:
            identifier(value, key)
    require(
        isinstance(profile["limits"], dict)
        and set(profile["limits"]) == {"concurrency", "per_user"},
        "Invalid pool limits",
    )
    for key, value in profile["limits"].items():
        positive(value, key, 256)
    runtime = profile["runtime"]
    require(
        isinstance(runtime, dict)
        and isinstance(runtime.get("files"), dict)
        and runtime["files"],
        "Runtime needs pinned executable/config digests",
    )
    require(command[0] in runtime["files"], "Native task executable is not pinned")
    for path, value in runtime["files"].items():
        require(
            isinstance(path, str) and Path(path).is_absolute(),
            "Runtime files need absolute paths",
        )
        require(
            isinstance(value, str) and DIGEST.fullmatch(value),
            "Invalid runtime file digest",
        )
    text(runtime.get("storage_namespace"), "storage namespace", 128)
    require(
        runtime.get("architecture") in {"x86_64", "aarch64"},
        "Unsupported runtime architecture",
    )
    require(
        runtime.get("backend") in {"nvidia", "amd", "cpu"},
        "Unsupported runtime backend",
    )
    if profile["executor"] == "slurm":
        executors = [runtime[key] for key in ("worker", "python") if key in runtime]
        require(
            executors,
            "Slurm profiles need a pinned runtime worker or native workflow interpreter",
        )
        for executable in executors:
            require(
                isinstance(executable, str)
                and Path(executable).is_absolute()
                and executable in runtime["files"],
                "Slurm runtime executables must be pinned",
            )
    qualified = profile["qualified_nodes"]
    require(isinstance(qualified, dict), "Expected qualified target evidence")
    if profile["executor"] == "slurm":
        require(
            resources["partition"] and resources["nodes"],
            "Slurm needs an explicit partition and qualified nodes",
        )
        require(
            set(resources["nodes"]) <= set(qualified),
            "Some Slurm targets are not qualified",
        )
        for node in resources["nodes"]:
            evidence = qualified[node]
            require(
                isinstance(evidence, dict)
                and evidence.get("runtime_identity") == runtime,
                "Qualified node runtime differs from the frozen deployment",
            )
            require(
                isinstance(evidence.get("capabilities"), list)
                and all(isinstance(value, str) for value in evidence["capabilities"])
                and set(capabilities) <= set(evidence["capabilities"]),
                "Qualified node lacks required task capabilities",
            )
        require(
            bool(resources["gres"]) == bool(resources["gpus"]),
            "GPU request and GRES disagree",
        )
        require(not resources["devices"], "Slurm owns device assignment")
    elif resources["gpus"]:
        require(resources["devices"], "Local GPU profile needs qualified device UUIDs")
    return copy.deepcopy(profile)
