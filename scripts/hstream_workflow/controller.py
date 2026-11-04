"""Durable controller for file-backed, typed native workflows."""

import copy
import datetime
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from uuid import uuid4

from . import events, graph, local, slurm
from .files import (
    contained,
    file_digest,
    locked,
    private_directory,
    read_json,
    read_tail,
    write_json,
)
from .processes import host_identity, process_identity, same_process
from .profiles import (
    controller_files,
    load_config,
    preflight_runtime,
    validate_game,
    validate_profile,
)
from .schema import WorkflowError, digest, require, validate_plan
from .state import Registry, attempt_directory

MAX_ARTIFACT_BYTES = 64 * 1024**3


class NativeControlError(WorkflowError):
    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


class SubmissionRejected(WorkflowError):
    def __init__(self, message, workflow_id):
        super().__init__(message)
        self.workflow_id = workflow_id


class Controller:
    def __init__(self, registry):
        self.registry = Registry(registry)
        self.config = load_config(registry)
        self.defaults = self.config["defaults"]

    def profiles(self):
        return {
            "schema": 1,
            "admission_profile": self.config["admission_profile"],
            "profiles": [
                {"id": key, **copy.deepcopy(value)}
                for key, value in self.config["profiles"].items()
            ],
            "defaults": copy.deepcopy(self.defaults),
        }

    def _native(
        self,
        directory,
        action,
        payload,
        *,
        runtime_checked=False,
        control_descriptor=None
    ):
        helper = self.config["profiles"][self.config["admission_profile"]]
        if not runtime_checked:
            preflight_runtime(helper)
        request = Path(directory) / ("control-" + str(uuid4()) + ".json")
        write_json(request, payload, exclusive=True)
        try:
            result = subprocess.run(
                [
                    *helper["task_command"],
                    "--admission",
                    action,
                    "--request",
                    str(request),
                ],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                # Keep the publication/admission barrier alive in the native
                # helper if the controller dies before its response arrives.
                pass_fds=() if control_descriptor is None else (control_descriptor,),
                timeout=self.defaults["command_timeout_seconds"],
                check=False,
            )
            require(
                len(result.stdout) <= 4 * 1024 * 1024
                and len(result.stderr) <= 1024 * 1024,
                "Native control response exceeds its bound",
            )
            import json

            response = json.loads(result.stdout)
            require(isinstance(response, dict), "Invalid native control response")
            if result.returncode != 0:
                raise NativeControlError(
                    str(response.get("error", result.stderr.decode(errors="replace")))[
                        :8192
                    ],
                    response.get("code"),
                )
            return response
        finally:
            request.unlink(missing_ok=True)

    def register(self, plan, game, *, defer_admission=False):
        try:
            return self._register(plan, game, defer_admission=defer_admission)
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            workflow_id = plan.get("workflow_id") if isinstance(plan, dict) else None
            if not isinstance(workflow_id, str):
                workflow_id = None
            # External admission follows durable registry insertion. Prove its
            # absence under the same lock before classifying a definite rejection.
            # A failed read leaves acceptance ambiguous and must keep monitoring.
            try:
                with self.registry.lock():
                    absent = (
                        workflow_id
                        and workflow_id not in self.registry.read()["workflows"]
                    )
            except (OSError, ValueError):
                absent = False
            if absent:
                raise SubmissionRejected(str(exc), workflow_id) from exc
            raise

    def _register(self, plan, game, *, defer_admission=False):
        plan = validate_plan(plan)
        if any(stage["kind"].startswith("experiment-") for stage in plan["stages"]):
            self._user_config_snapshot(plan)
        game = validate_game(self.config, game)
        for key, profile in plan["profiles"].items():
            validate_profile(profile)
            require(
                all(
                    str(path) in profile["runtime"]["files"]
                    for path in controller_files()
                ),
                "Resource profile must pin the installed workflow launcher and Python package",
            )
            require(
                key in self.config["profiles"]
                and digest(profile) == digest(self.config["profiles"][key]),
                "Resource profile changed before admission",
            )
        workflow_id = plan["workflow_id"]
        directory = private_directory(
            game / "_hockeymon/workflows" / workflow_id, create=True
        )
        with self.registry.lock():
            ledger = self.registry.read()
            existing = ledger["workflows"].get(workflow_id)
            if existing:
                require(
                    existing["game"] == str(game)
                    and existing["plan_digest"] == digest(plan),
                    "Workflow UUID was already used for another plan",
                )
            else:
                require(
                    len(ledger["workflows"]) < 4096,
                    "Workflow registry history limit reached",
                )
                # Seal all intent before native ownership acquisition. A service
                # restart can discover and reconcile a lost acquire response.
                if (directory / "plan.json").exists():
                    require(
                        digest(read_json(directory / "plan.json")) == digest(plan),
                        "Unregistered workflow directory has another plan",
                    )
                else:
                    write_json(directory / "plan.json", plan, exclusive=True)
                # No external admission can happen before this registry entry is
                # published. Complete interrupted local registration idempotently.
                if not (directory / "state.json").exists():
                    write_json(
                        directory / "state.json",
                        graph.refresh(plan, graph.initial_state(plan)),
                        exclusive=True,
                    )
                if not (directory / "owner.json").exists():
                    write_json(
                        directory / "owner.json",
                        {
                            "schema": 1,
                            "game": str(game),
                            "workflow_id": workflow_id,
                            "token": str(uuid4()),
                            "acquired": False,
                            "released": False,
                            "bound": False,
                        },
                        exclusive=True,
                    )
                ledger["workflows"][workflow_id] = {
                    "directory": str(directory),
                    "game": str(game),
                    "plan_digest": digest(plan),
                    "user": os.getuid(),
                }
                self.registry.write(ledger)
        # The durable entry lets the service acquire and bind ownership before
        # admitting any tasks. A GUI submit must not wait for native runtime
        # verification or a service tick holding the reconciliation lock.
        if not defer_admission:
            with locked(directory / "controller.lock"):
                self._ensure_owner(directory, plan)
        return self.status(workflow_id)

    def _ensure_owner(self, directory, plan, control_descriptor=None):
        if control_descriptor is None:
            with locked(directory / "control-intent.lock") as descriptor:
                return self._ensure_owner(directory, plan, descriptor)
        owner = read_json(directory / "owner.json")
        if owner["released"]:
            return owner
        payload = {
            "game": owner["game"],
            "kind": "workflow",
            "id": owner["workflow_id"],
            "token": owner["token"],
            "workflow_directory": str(directory),
        }
        state = read_json(directory / "state.json")
        cancelled = state["cancel_requested"] or self._cancel_pending(directory, state)
        if not owner["acquired"] and cancelled:
            # Acquired=False also covers a committed claim with a lost response.
            # Every acquire helper holds this same inherited lock, so none can
            # still claim ownership after this inspection and cancellation.
            current = self._native(
                directory,
                "inspect",
                {"game": owner["game"]},
                control_descriptor=control_descriptor,
            ).get("owner")
            if current is None or current.get("token") != owner["token"]:
                self._save(directory, plan, graph.cancel(plan, state))
                owner["released"] = True
                write_json(directory / "owner.json", owner)
                return owner
            owner.update(acquired=True, record=current)
            write_json(directory / "owner.json", owner)
        if not owner["acquired"]:
            try:
                response = self._native(
                    directory, "acquire", payload, control_descriptor=control_descriptor
                )
            except NativeControlError as exc:
                if exc.code == "conflict":
                    # Definite rejection must never become a deferred submission
                    # when a different game operation eventually releases its lock.
                    state = graph.cancel(plan, read_json(directory / "state.json"))
                    self._save(directory, plan, state)
                    owner.update(released=True, rejection=str(exc))
                    write_json(directory / "owner.json", owner)
                raise
            require(
                isinstance(response.get("owner"), dict)
                and response["owner"].get("token") == owner["token"],
                "Native admission did not return the expected owner",
            )
            owner["record"] = response["owner"]
            owner["acquired"] = True
            write_json(directory / "owner.json", owner)
        if not owner["bound"] and not cancelled:
            keys = sorted(
                {
                    stage["inputs"]["catalog_key"]
                    for stage in plan["stages"]
                    if isinstance(stage["inputs"].get("catalog_key"), str)
                }
            )
            self._native(
                directory,
                "bind",
                {**payload, "workflow_id": plan["workflow_id"], "catalog_keys": keys},
                control_descriptor=control_descriptor,
            )
            owner["bound"] = True
            write_json(directory / "owner.json", owner)
        return owner

    def _save(self, directory, plan, state):
        graph.validate_state(plan, state)
        write_json(directory / "state.json", state)
        events.record(directory, plan, state)
        projection = graph.snapshot(plan, state)
        write_json(directory / "status.json", projection)
        observer = self.config["observer_registry"]
        if observer:
            try:
                from .dagster_registry import publish_snapshot

                observation_id = publish_snapshot(observer, plan, projection)
                write_json(
                    directory / "observer-binding.json",
                    {"observation_id": observation_id, "epoch": state["epoch"]},
                )
                (directory / "observer-error.json").unlink(missing_ok=True)
            except (OSError, ValueError, RuntimeError) as exc:
                # Compute state is already durable. Observation never owns it.
                write_json(
                    directory / "observer-error.json",
                    {"error": str(exc)[:8192], "revision": state["revision"]},
                )
        return state

    @staticmethod
    def _cancel_pending(directory, state):
        path = directory / "cancel-intent.json"
        if not path.exists():
            return False
        intent = read_json(path)
        require(
            intent.get("workflow_id") == state["workflow_id"],
            "Cancellation belongs to another workflow",
        )
        return intent.get("epoch") == state["epoch"]

    def status(self, workflow_id):
        directory, _ = self.registry.lookup(workflow_id)
        # Atomic files make status/log reads independent of a slow scheduler or
        # artifact verifier. Preserve the persisted revision in the projection.
        plan = read_json(directory / "plan.json")
        state = read_json(directory / "state.json")
        pending = self._cancel_pending(directory, state)
        value = graph.snapshot(plan, graph.cancel(plan, state) if pending else state)
        value["revision"] = state["revision"]
        owner = read_json(directory / "owner.json")
        value["owner_released"] = owner["released"]
        if owner.get("rejection"):
            value["controller_error"] = {"error": owner["rejection"]}
        value["limits"] = copy.deepcopy(plan["limits"])
        value["cancellation_pending"] = pending and not state["cancel_requested"]
        stages = {stage["id"]: stage for stage in plan["stages"]}
        for node in value["nodes"]:
            profile_id = stages[node["id"]]["profile_id"]
            node["profile_id"] = profile_id
            node["resources"] = copy.deepcopy(plan["profiles"][profile_id]["resources"])
        binding_path = directory / "observer-binding.json"
        if self.config["observer_registry"] and binding_path.exists():
            try:
                from .dagster_registry import observation_path

                binding = read_json(binding_path)
                if binding["epoch"] == state["epoch"]:
                    launch_path = (
                        observation_path(
                            self.config["observer_registry"], binding["observation_id"]
                        )
                        / "launch.json"
                    )
                    if launch_path.exists():
                        launch = read_json(launch_path)
                        if launch.get("run_id") and launch.get("url"):
                            value["observer"] = {
                                "run_id": launch["run_id"],
                                "url": launch["url"],
                            }
            except (OSError, ValueError) as exc:
                value["observer_error"] = {"error": str(exc)[:8192]}
        heartbeat_path = self.registry.root / "heartbeat.json"
        try:
            heartbeat = read_json(heartbeat_path) if heartbeat_path.exists() else None
            maximum_age = (
                3 * self.defaults["poll_interval_ms"] / 1000
                + self.defaults["command_timeout_seconds"]
            )
            healthy = bool(
                heartbeat
                and 0 <= time.time_ns() - heartbeat["time_ns"] <= maximum_age * 1e9
            )
            if healthy and heartbeat.get("host") == host_identity()["host"]:
                healthy = same_process(heartbeat)
            value["controller_online"] = healthy
            if not healthy:
                value["controller_warning"] = {
                    "error": "No live controller-service heartbeat. Keep the foreground runner active or start the workflow service."
                }
        except (OSError, ValueError, KeyError, TypeError) as exc:
            value["controller_online"] = False
            value["controller_warning"] = {
                "error": "Controller heartbeat unavailable: " + str(exc)[:1024]
            }
        for name in ("controller-error", "observer-error", "cancel-error"):
            if (directory / (name + ".json")).exists():
                value[name.replace("-", "_")] = read_json(directory / (name + ".json"))
        return value

    def _cancel_owner(self, directory):
        owner = read_json(directory / "owner.json")
        if owner["acquired"] and not owner["released"]:
            current = self._native(directory, "inspect", {"game": owner["game"]}).get(
                "owner"
            )
            require(
                current is not None and current.get("token") == owner["token"],
                "Game owner changed during cancellation",
            )
            if not current.get("cancel_requested"):
                self._native(
                    directory,
                    "cancel",
                    {
                        "game": owner["game"],
                        "token": owner["token"],
                        "revision": current["revision"],
                    },
                )

    def _apply_cancel(self, directory, plan, state):
        if self._cancel_pending(directory, state):
            state = graph.cancel(plan, state)
            self._cancel_owner(directory)
        return state

    def cancel(self, workflow_id, expected_epoch=None):
        directory, _ = self.registry.lookup(workflow_id)
        with locked(directory / "control-intent.lock"):
            state = read_json(directory / "state.json")
            require(
                expected_epoch is None or expected_epoch == state["epoch"],
                "Stale workflow epoch",
            )
            if not all(
                node["state"] in graph.TERMINAL for node in state["nodes"].values()
            ):
                write_json(
                    directory / "cancel-intent.json",
                    {"workflow_id": workflow_id, "epoch": state["epoch"]},
                )
                # Publish cancellation to supervisors immediately, without waiting
                # for a controller holding its lock during scheduler calls/hashes.
                self._request_cancellations(directory, state, scheduler=False)
                try:
                    self._cancel_owner(directory)
                    (directory / "cancel-error.json").unlink(missing_ok=True)
                except (OSError, ValueError, subprocess.SubprocessError) as exc:
                    write_json(
                        directory / "cancel-error.json", {"error": str(exc)[:8192]}
                    )
        return self.status(workflow_id)

    def retry(self, workflow_id, stages, expected_epoch=None):
        directory, _ = self.registry.lookup(workflow_id)
        with locked(directory / "controller.lock"), locked(
            directory / "control-intent.lock"
        ) as descriptor:
            plan = read_json(directory / "plan.json")
            previous = read_json(directory / "state.json")
            require(
                expected_epoch is None or expected_epoch == previous["epoch"],
                "Stale workflow epoch",
            )
            owner = read_json(directory / "owner.json")
            require(
                owner["released"],
                "Wait for confirmed game ownership release before retrying",
            )
            state = graph.retry(plan, previous, stages)
            owner.update(
                acquired=False, released=False, bound=False, token=str(uuid4())
            )
            owner.pop("rejection", None)
            write_json(directory / "owner.json", owner)
            self._save(directory, plan, state)
            self._ensure_owner(directory, plan, descriptor)
        return self.status(workflow_id)

    def _request_cancellations(self, directory, state, scheduler=True):
        for node in state["nodes"].values():
            if node["state"] not in graph.ACTIVE:
                continue
            attempt = node["attempts"][-1]
            root = attempt_directory(directory, attempt["id"])
            write_json(root / "cancel.json", {"attempt_id": attempt["id"]})
            if (
                scheduler
                and attempt["backend"] == "slurm"
                and (root / "submission.json").exists()
            ):
                try:
                    slurm.cancel(root, self.defaults["command_timeout_seconds"])
                except (OSError, ValueError, subprocess.SubprocessError) as exc:
                    write_json(root / "cancel-error.json", {"error": str(exc)[:8192]})

    @staticmethod
    def _resolve(value, state):
        if isinstance(value, dict):
            if set(value) == {"stage", "output"}:
                node = state["nodes"][value["stage"]]
                require(
                    node["state"] in graph.SUCCESS
                    and value["output"] in node["result"],
                    "Required verified prerequisite output is unavailable",
                )
                return copy.deepcopy(node["result"][value["output"]])
            return {
                key: Controller._resolve(child, state) for key, child in value.items()
            }
        if isinstance(value, list):
            return [Controller._resolve(child, state) for child in value]
        return value

    @staticmethod
    def _user_config_snapshot(plan):
        snapshot = plan["runtime_identity"].get("user_config_snapshot")
        require(
            isinstance(snapshot, dict)
            and snapshot.get("schema") == 1
            and isinstance(snapshot.get("values"), dict)
            and isinstance(snapshot.get("source_path"), str),
            "Native experiment workflows need a frozen user configuration snapshot",
        )
        return copy.deepcopy(snapshot)

    def _admit(self, directory, plan, state, stage, owner):
        state = self._apply_cancel(directory, plan, state)
        if state["cancel_requested"]:
            return self._save(directory, plan, state)
        profile = plan["profiles"][stage["profile_id"]]
        runtime = copy.deepcopy(profile["runtime"])
        if stage["kind"].startswith("experiment-"):
            runtime["user_config_snapshot"] = self._user_config_snapshot(plan)
        # A permanently invalid legacy plan or missing prerequisite must not
        # allocate an empty attempt directory on every controller poll.
        inputs = self._resolve(stage["inputs"], state)
        attempt_id = str(uuid4())
        root = attempt_directory(directory, attempt_id)
        request = {
            "schema": 1,
            "workflow_id": plan["workflow_id"],
            "plan_digest": digest(plan),
            "stage_id": stage["id"],
            "attempt_id": attempt_id,
            "epoch": state["epoch"],
            "operation": stage["kind"],
            "root": str(root),
            "game": owner["game"],
            "inputs": inputs,
            "runtime": runtime,
            "result_path": str(root / "native-result.json"),
            "owner": {"game": owner["game"], "token": owner["token"]},
        }
        reservation = self.registry.reserve(
            plan["workflow_id"],
            stage["id"],
            attempt_id,
            stage["profile_id"],
            profile,
            digest(request),
        )
        if reservation is None:
            root.rmdir()
            return state
        # All these writes precede the external effect. A partial setup remains a
        # reserved attempt and is reconciled as a never-submitted task on restart.
        state = graph.begin_attempt(
            plan, state, stage["id"], attempt_id, digest(request), profile["executor"]
        )
        self._save(directory, plan, state)
        write_json(root / "request.json", request, exclusive=True)
        write_json(root / "profile.json", profile, exclusive=True)
        write_json(root / "settings.json", self.defaults, exclusive=True)
        timestamp = datetime.datetime.now(
            datetime.timezone.utc  # noqa: UP017 -- Jetson Python 3.10
        ) - datetime.timedelta(minutes=5)
        intent = {
            "schema": 1,
            "attempt_id": attempt_id,
            "request_digest": digest(request),
            "profile_digest": digest(profile),
            "backend": profile["executor"],
            "name": "hsw-" + attempt_id,
            "submitted_after": timestamp.strftime("%Y-%m-%dT%H:%M:%S"),
            "device": reservation["device"],
        }
        write_json(root / "submission.json", intent, exclusive=True)
        command = [
            (
                profile["runtime"]["python"]
                if profile["executor"] == "slurm"
                else sys.executable
            ),
            str(Path(__file__).resolve().parents[1] / "hstream-workflow"),
            "_supervise",
            "--root",
            str(root),
        ]
        # Persist the exact installed supervisor command for diagnostics. The
        # environment is inherited from the fixed controller deployment.
        write_json(root / "supervisor-command.json", {"argv": command}, exclusive=True)
        try:
            if profile["executor"] == "slurm":
                launched = slurm.submit(
                    root, profile, command, self.defaults["command_timeout_seconds"]
                )
                details = {**launched, "reason": "Waiting for Slurm resources"}
                outcome = "queued"
            else:
                launched = local.submit(root, command)
                details = {
                    "host": launched["host"],
                    "reason": "Local supervisor launched",
                }
                outcome = "running"
            state = graph.update_attempt(
                plan, state, stage["id"], attempt_id, outcome, details
            )
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            state = graph.update_attempt(
                plan,
                state,
                stage["id"],
                attempt_id,
                "recovery",
                {"reason": "Submission acceptance is unresolved: " + str(exc)[:8000]},
            )
        return self._save(directory, plan, state)

    @staticmethod
    def _verify_completion(request, completion, profile):
        require(completion.get("schema") == 1, "Unsupported completion schema")
        require(
            completion.get("request_digest") == digest(request)
            and completion.get("attempt_id") == request["attempt_id"],
            "Completion belongs to another native request",
        )
        require(
            completion.get("profile_digest") == digest(profile),
            "Completion deployment changed",
        )
        require(
            completion.get("stopped") is True
            and completion.get("cleanup_complete") is True,
            "Native descendants or cleanup remain unresolved",
        )

    def _verify_result(self, root, request, completion, profile):
        self._verify_completion(request, completion, profile)
        require(
            completion.get("exit_code") == 0
            and not completion.get("error")
            and not completion.get("cancelled"),
            completion.get("error") or "Native task did not succeed",
        )
        require(
            file_digest(root / "native-result.json", 4 * 1024 * 1024)
            == completion.get("receipt"),
            "Native receipt changed after supervision",
        )
        result = read_json(root / "native-result.json")
        require(result.get("schema") == 1, "Unsupported native task receipt schema")
        for key in ("workflow_id", "plan_digest", "stage_id", "attempt_id"):
            require(
                result.get(key) == request[key],
                "Native result identity mismatch: " + key,
            )
        require(
            result.get("status") in {"succeeded", "unavailable"},
            "Invalid native task outcome",
        )
        outputs = result.get("outputs")
        require(isinstance(outputs, dict), "Missing native task output descriptor")
        artifacts = result.get("artifacts")
        require(
            isinstance(artifacts, list) and len(artifacts) <= 10001,
            "Invalid native artifact manifest",
        )
        paths, total = set(), 0
        for artifact in artifacts:
            require(
                isinstance(artifact, dict) and artifact.get("identity_schema") == 2,
                "Distributed tasks require portable artifact identities",
            )
            require(
                artifact.get("storage_namespace")
                == profile["runtime"]["storage_namespace"],
                "Artifact storage namespace changed",
            )
            relative = artifact.get("path")
            require(
                isinstance(relative, str) and relative not in paths,
                "Invalid or duplicate artifact path",
            )
            paths.add(relative)
            actual = file_digest(contained(root, relative), MAX_ARTIFACT_BYTES - total)
            require(
                actual["size"] == artifact.get("size")
                and actual["sha256"] == artifact.get("sha256"),
                "Native artifact content changed",
            )
            total += actual["size"]
        require(
            result["status"] != "succeeded" or artifacts,
            "Successful native task has no verified artifacts",
        )
        return result

    def _committed_adoption(self, directory, root, request, completion, owner):
        path = root / "adoption-intent.json"
        if not path.exists():
            return None
        intent = read_json(path)
        require(
            intent.get("schema") == 1
            and intent.get("request_digest") == digest(request)
            and intent.get("completion_digest") == digest(completion)
            and intent.get("action") in {"adopt", "adopt-selection"},
            "Adoption intent belongs to another verified completion",
        )
        payload, details = intent.get("payload"), intent.get("details")
        require(
            isinstance(payload, dict)
            and all(
                payload.get(key) == value
                for key, value in {
                    "game": owner["game"],
                    "token": owner["token"],
                    "workflow_id": request["workflow_id"],
                    "workflow_directory": str(directory),
                    "attempt_id": request["attempt_id"],
                }.items()
            )
            and isinstance(details, dict)
            and details.get("verified") is True
            and details.get("stopped") is True
            and details.get("cleanup_complete") is True
            and isinstance(details.get("outputs"), dict)
            and details["outputs"].get("workspace") == payload.get("workspace")
            and details["outputs"].get("catalog_key") == payload.get("catalog_key"),
            "Adoption intent lost its exact verified output binding",
        )
        with locked(directory / "control-intent.lock") as descriptor:
            response = self._native(
                directory,
                "adoption-status",
                {**payload, "adoption": intent["action"]},
                control_descriptor=descriptor,
            )
            require(
                type(response.get("adopted")) is bool, "Invalid native adoption status"
            )
            return copy.deepcopy(details) if response["adopted"] else None

    def _reconcile_attempt(self, directory, plan, state, stage, owner):
        node = state["nodes"][stage["id"]]
        attempt = node["attempts"][-1]
        root = attempt_directory(directory, attempt["id"])
        if not (root / "dispatch.json").exists():
            # Both adapters fsync this barrier immediately before the external
            # launch. Missing barrier proves that preparation never submitted.
            details = {
                "stopped": True,
                "cleanup_complete": True,
                "reason": "Task preparation stopped before the external submission barrier",
            }
            state = graph.update_attempt(
                plan, state, stage["id"], attempt["id"], "failed", details
            )
            self._save(directory, plan, state)
            self.registry.release(
                plan["workflow_id"], attempt["id"], stopped=True, cleanup_complete=True
            )
            return state
        try:
            observation = (
                slurm.lookup(root, self.defaults["command_timeout_seconds"])
                if attempt["backend"] == "slurm"
                else local.lookup(root)
            )
            if attempt["backend"] == "local" and observation["state"] == "REJECTED":
                require(
                    observation.get("attempt_id") == attempt["id"]
                    and observation.get("stopped") is True
                    and observation.get("cleanup_complete") is True,
                    "Local launch rejection lacks exact shutdown proof",
                )
                state = self._apply_cancel(directory, plan, state)
                state = graph.update_attempt(
                    plan, state, stage["id"], attempt["id"], "failed", observation
                )
                self._save(directory, plan, state)
                self.registry.release(
                    plan["workflow_id"],
                    attempt["id"],
                    stopped=True,
                    cleanup_complete=True,
                )
                return state
            complete = (
                read_json(root / "completion.json")
                if (root / "completion.json").exists()
                else None
            )
            stopped = bool(
                complete
                and complete.get("stopped") is True
                and complete.get("cleanup_complete") is True
            )
            if attempt["backend"] == "slurm":
                stopped = stopped and observation["terminal"]
            if stopped:
                request, profile = read_json(root / "request.json"), read_json(
                    root / "profile.json"
                )
                require(
                    digest(request) == attempt["request_digest"],
                    "Sealed task request changed",
                )
                require(
                    digest(profile) == digest(plan["profiles"][stage["profile_id"]]),
                    "Sealed resource profile changed",
                )
                require(
                    request["plan_digest"] == digest(plan)
                    and request["stage_id"] == stage["id"],
                    "Request belongs to another plan stage",
                )
                # Cancellation requires shutdown proof, never usable success
                # artifacts. A missing NFS result must not prevent explicit Stop.
                self._verify_completion(request, complete, profile)
                committed = self._committed_adoption(
                    directory, root, request, complete, owner
                )
                if committed is not None:
                    # Native adoption precedes any later Cancel even when its
                    # response was lost. Restore that order before cancelling the
                    # remaining graph; this never re-adopts or reads output files.
                    cancelled = state["cancel_requested"]
                    before_cancel = copy.deepcopy(state)
                    before_cancel["cancel_requested"] = False
                    state = graph.update_attempt(
                        plan,
                        before_cancel,
                        stage["id"],
                        attempt["id"],
                        "succeeded",
                        committed,
                    )
                    if cancelled:
                        state = graph.cancel(plan, state)
                    self._save(directory, plan, state)
                    self.registry.release(
                        plan["workflow_id"],
                        attempt["id"],
                        stopped=True,
                        cleanup_complete=True,
                    )
                    return state
                state = self._apply_cancel(directory, plan, state)
                result, verification_error = None, None
                outcome, details = "cancelled", {
                    "reason": "Cancelled after confirmed shutdown"
                }
                if not state["cancel_requested"]:
                    try:
                        if attempt["backend"] == "slurm":
                            require(
                                observation["state"] == "COMPLETED"
                                and observation.get("exit_code") == "0:0",
                                "Slurm did not complete successfully: "
                                + observation["state"]
                                + " (exit "
                                + str(observation.get("exit_code", "unknown"))
                                + ")"
                                + (
                                    ": " + str(complete["error"])
                                    if complete.get("error")
                                    else ""
                                ),
                            )
                        result = self._verify_result(root, request, complete, profile)
                        outcome = result["status"]
                        details = {
                            "verified": True,
                            "outputs": result["outputs"],
                            "reason": result.get("message", outcome),
                        }
                    except OSError as exc:
                        # Recheck cancellation before keeping transient storage
                        # failures in recovery; no artifact is needed to cancel.
                        verification_error = exc
                    except ValueError as exc:
                        outcome, details = "failed", {"reason": str(exc)[:8192]}
                        # Diagnostics are advisory and must never grant success
                        # or hold up release after verified shutdown/cleanup.
                        # Keep the actual native failure beside scheduler status
                        # so both UI monitors can explain a failed stage.
                        try:
                            tail = read_tail(root / "task.log", 4096).decode(
                                "utf-8", errors="replace"
                            ).strip()
                            if tail:
                                details["reason"] = (
                                    details["reason"][:2048]
                                    + "\nTask log (tail):\n"
                                    + tail
                                )
                        except (OSError, ValueError):
                            pass
                adoption = bool(
                    result is not None
                    and outcome == "succeeded"
                    and result["outputs"].get("catalog_key")
                )
                if adoption:
                    preflight_runtime(
                        self.config["profiles"][self.config["admission_profile"]]
                    )
                # Linearize the final publication against cancel(). File hashing
                # and runtime preflight stay outside this short bounded native
                # commit. A cancel already committed always wins; a later one
                # observes the terminal state published under this same lock.
                with locked(directory / "control-intent.lock") as descriptor:
                    state = self._apply_cancel(directory, plan, state)
                    if state["cancel_requested"]:
                        outcome, details = "cancelled", {
                            "reason": "Cancelled after confirmed shutdown"
                        }
                    elif verification_error is not None:
                        raise verification_error
                    elif adoption:
                        # Lost responses retain recovery for exact idempotent
                        # adoption; never assume the catalog write did not happen.
                        action = (
                            "adopt-selection"
                            if result["outputs"].get("adoption") == "selection"
                            else "adopt"
                        )
                        payload = {
                            "game": owner["game"],
                            "token": owner["token"],
                            "workflow_id": plan["workflow_id"],
                            "workflow_directory": str(directory),
                            "attempt_id": attempt["id"],
                            "catalog_key": result["outputs"]["catalog_key"],
                            "workspace": result["outputs"]["workspace"],
                            "require_ice_mask": bool(
                                request["operation"] == "experiment-baseline"
                                or request["inputs"].get("require_ice_mask", False)
                            ),
                        }
                        intent = {
                            "schema": 1,
                            "request_digest": digest(request),
                            "completion_digest": digest(complete),
                            "action": action,
                            "payload": payload,
                            "details": {
                                **details,
                                "stopped": True,
                                "cleanup_complete": True,
                            },
                        }
                        intent_path = root / "adoption-intent.json"
                        if intent_path.exists():
                            require(
                                read_json(intent_path) == intent,
                                "Verified adoption intent changed",
                            )
                        else:
                            write_json(intent_path, intent, exclusive=True)
                        self._native(
                            directory,
                            action,
                            payload,
                            runtime_checked=True,
                            control_descriptor=descriptor,
                        )
                    details.update(
                        stopped=True,
                        cleanup_complete=True,
                        **{
                            key: value
                            for key, value in observation.items()
                            if key in {"scheduler_id", "cluster", "host"}
                        },
                    )
                    state = graph.update_attempt(
                        plan, state, stage["id"], attempt["id"], outcome, details
                    )
                    self._save(directory, plan, state)
                self.registry.release(
                    plan["workflow_id"],
                    attempt["id"],
                    stopped=True,
                    cleanup_complete=True,
                )
                return state
            if attempt["backend"] == "slurm" and observation["terminal"]:
                if (
                    observation["state"] == "COMPLETED"
                    and observation.get("exit_code") == "0:0"
                    and not state["cancel_requested"]
                ):
                    # Accounting can become visible before NFS invalidates a
                    # negative completion-file lookup on the controller host.
                    # Successful allocation exit is never proof of missing data.
                    return graph.update_attempt(
                        plan,
                        state,
                        stage["id"],
                        attempt["id"],
                        "recovery",
                        {
                            **observation,
                            "reason": "Slurm completed successfully; waiting for the exact supervised receipt and cleanup proof",
                        },
                    )
                # Slurm cgroups prove shutdown, but absent private-runtime cleanup
                # is reconciled explicitly before capacity/workspace release.
                import shutil

                runtime = root / "runtime"
                if runtime.exists():
                    require(
                        not runtime.is_symlink(), "Runtime cleanup path was replaced"
                    )
                    shutil.rmtree(runtime)
                details = {
                    "stopped": True,
                    "cleanup_complete": True,
                    "reason": "Slurm stopped without a successful supervised native receipt",
                    **observation,
                }
                details["reason"] = (
                    "Slurm stopped without a successful supervised native receipt: "
                    + observation["state"]
                )
                state = graph.update_attempt(
                    plan, state, stage["id"], attempt["id"], "failed", details
                )
                self._save(directory, plan, state)
                self.registry.release(
                    plan["workflow_id"],
                    attempt["id"],
                    stopped=True,
                    cleanup_complete=True,
                )
                return state
            outcome = (
                "recovery"
                if observation["state"] == "UNKNOWN"
                else (
                    "running"
                    if observation["state"] in {"RUNNING", "COMPLETING"}
                    else "queued"
                )
            )
            return graph.update_attempt(
                plan, state, stage["id"], attempt["id"], outcome, observation
            )
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            if state["nodes"][stage["id"]]["state"] in graph.TERMINAL:
                # Terminal state was already published. Preserve it and let the
                # next pass reconcile the reservation instead of rewriting it.
                raise
            return graph.update_attempt(
                plan,
                state,
                stage["id"],
                attempt["id"],
                "recovery",
                {"reason": str(exc)[:8192]},
            )

    def _reconcile_reservations(self, directory, plan, state):
        with self.registry.lock():
            rows = [
                copy.deepcopy(row)
                for row in self.registry.read()["reservations"].values()
                if row["workflow_id"] == plan["workflow_id"]
            ]
        attempts = {
            attempt["id"]: attempt
            for node in state["nodes"].values()
            for attempt in node["attempts"]
        }
        for row in rows:
            attempt = attempts.get(row["attempt_id"])
            if attempt is None:
                root = directory / "attempts" / row["attempt_id"]
                require(
                    not (root / "dispatch.json").exists(),
                    "Uncatalogued submission intent requires recovery",
                )
                # A crash after reserving capacity but before saving stage intent
                # cannot have launched anything: submission is always later.
                self.registry.release(
                    plan["workflow_id"],
                    row["attempt_id"],
                    stopped=True,
                    cleanup_complete=True,
                )
            elif attempt["state"] in graph.TERMINAL:
                details = attempt["details"]
                self.registry.release(
                    plan["workflow_id"],
                    row["attempt_id"],
                    stopped=details.get("stopped"),
                    cleanup_complete=details.get("cleanup_complete"),
                )

    def tick(self, workflow_id):
        directory, _ = self.registry.lookup(workflow_id)
        try:
            with locked(directory / "controller.lock", blocking=False):
                plan = validate_plan(read_json(directory / "plan.json"))
                state = graph.refresh(plan, read_json(directory / "state.json"))
                self._reconcile_reservations(directory, plan, state)
                owner = self._ensure_owner(directory, plan)
                state = graph.refresh(plan, read_json(directory / "state.json"))
                if owner["released"]:
                    (directory / "controller-error.json").unlink(missing_ok=True)
                    self._save(directory, plan, state)
                    return graph.snapshot(plan, state)
                state = self._apply_cancel(directory, plan, state)
                if state["cancel_requested"]:
                    self._request_cancellations(directory, state)
                for stage in plan["stages"]:
                    if state["nodes"][stage["id"]]["state"] in graph.ACTIVE:
                        state = self._reconcile_attempt(
                            directory, plan, state, stage, owner
                        )
                self._save(directory, plan, state)
                for stage in graph.ready_stages(plan, state):
                    if stage["id"] in {
                        row["id"] for row in graph.ready_stages(plan, state)
                    }:
                        state = self._admit(directory, plan, state, stage, owner)
                state = self._save(directory, plan, graph.refresh(plan, state))
                if all(
                    node["state"] in graph.TERMINAL for node in state["nodes"].values()
                ):
                    with locked(directory / "control-intent.lock") as descriptor:
                        current = self._native(
                            directory, "inspect", {"game": owner["game"]}
                        ).get("owner")
                        if current is not None:
                            require(
                                current.get("token") == owner["token"],
                                "Game ownership changed before release",
                            )
                            self._native(
                                directory,
                                "release",
                                {
                                    "game": owner["game"],
                                    "token": owner["token"],
                                    "revision": current["revision"],
                                    "stopped_and_clean": True,
                                },
                                control_descriptor=descriptor,
                            )
                        owner["released"] = True
                        write_json(directory / "owner.json", owner)
                (directory / "controller-error.json").unlink(missing_ok=True)
                return graph.snapshot(plan, state)
        except BlockingIOError:
            return {"workflow_id": workflow_id, "locked": True}
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            write_json(directory / "controller-error.json", {"error": str(exc)[:8192]})
            raise

    def tick_all(self):
        with self.registry.lock():
            identifiers = list(self.registry.read()["workflows"])
        results = []
        for workflow_id in identifiers:
            try:
                results.append(self.tick(workflow_id))
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                results.append({"workflow_id": workflow_id, "error": str(exc)[:8192]})
        return results

    def events(self, workflow_id, stage_id=None):
        directory, _ = self.registry.lookup(workflow_id)
        if stage_id is not None:
            require(
                stage_id in read_json(directory / "state.json")["nodes"],
                "Unknown stage",
            )
        return events.read(directory, workflow_id, stage_id)

    def log(self, workflow_id, stage_id, attempt_id=None):
        directory, _ = self.registry.lookup(workflow_id)
        state = read_json(directory / "state.json")
        require(stage_id in state["nodes"], "Unknown stage")
        attempts = state["nodes"][stage_id]["attempts"]
        selected = (
            next((item for item in attempts if item["id"] == attempt_id), None)
            if attempt_id
            else (attempts[-1] if attempts else None)
        )
        require(selected is not None, "No matching stage attempt")
        path = directory / "attempts" / selected["id"] / "task.log"
        # Tail through a pinned descriptor; do not read a potentially huge log.
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            import stat

            info = os.fstat(descriptor)
            require(stat.S_ISREG(info.st_mode), "Task log is not a regular file")
            os.lseek(descriptor, max(0, info.st_size - 1024 * 1024), os.SEEK_SET)
            value = os.read(descriptor, 1024 * 1024).decode("utf-8", errors="replace")
        finally:
            os.close(descriptor)
        return {
            "workflow_id": workflow_id,
            "stage_id": stage_id,
            "attempt_id": selected["id"],
            "text": value,
        }


def serve(registry, once=False):
    controller = Controller(registry)
    stopped = threading.Event()
    errors = []

    def heartbeat():
        write_json(
            controller.registry.root / "heartbeat.json",
            {
                "schema": 1,
                **host_identity(),
                **process_identity(os.getpid()),
                "time_ns": time.time_ns(),
            },
        )

    def pulse():
        while not stopped.wait(controller.defaults["poll_interval_ms"] / 1000):
            try:
                heartbeat()
            except (OSError, ValueError) as exc:
                errors.append(exc)
                stopped.set()

    heartbeat()
    writer = threading.Thread(target=pulse, daemon=True)
    writer.start()
    try:
        while True:
            controller.tick_all()
            if errors:
                raise errors[0]
            if once:
                return
            stopped.wait(controller.defaults["poll_interval_ms"] / 1000)
    finally:
        stopped.set()
        writer.join(controller.defaults["command_timeout_seconds"])
