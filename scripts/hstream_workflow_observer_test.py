#!/usr/bin/env python3
"""Observer protocol tests; real Dagster graph checks run when installed."""

import copy
import importlib.util
import json
import tempfile
import unittest
import urllib.error
from pathlib import Path
from uuid import uuid4

from hstream_workflow import graph
from hstream_workflow.dagster_observer import make_job, operation_name
from hstream_workflow.dagster_registry import (
    configuration,
    observation_ids,
    observation_path,
    publish_snapshot,
    read_observation,
    reconcile_observation,
    retire_observation,
)
from hstream_workflow.files import locked, read_json, write_json
from hstream_workflow.schema import WorkflowError


def fixture_plan():
    def stage(key, dependencies):
        return {
            "id": key,
            "kind": "test",
            "label": key,
            "candidate_id": key,
            "dependencies": dependencies,
            "inputs": {},
            "profile_id": "cpu",
        }

    return {
        "schema": 1,
        "workflow_id": str(uuid4()),
        "kind": "test",
        "source_identity": {"private_path": "/private/video.mp4"},
        "configuration_digest": "a" * 64,
        "runtime_identity": {"secret": "do-not-publish"},
        "profiles": {"cpu": {"executor": "local", "capabilities": ["test"]}},
        "stages": [
            stage("baseline", []),
            stage("solve-A", ["baseline"]),
            stage("solve.B", ["baseline"]),
            stage("independent", []),
        ],
        "limits": {"max_parallel": 3},
    }


class FakeDagster:
    def __init__(self):
        self.runs = {}
        self.launches = 0
        self.lose_response = False

    def __call__(self, url, query, variables):
        if "pipelineRunOrError" in query:
            return {
                "pipelineRunOrError": next(
                    (
                        value
                        for value in self.runs.values()
                        if value["runId"] == variables["id"]
                    ),
                    {"__typename": "RunNotFoundError"},
                )
            }
        if "runsOrError" in query:
            observation = variables["filter"]["tags"][0]["value"]
            value = self.runs.get(observation)
            return {
                "runsOrError": {
                    "__typename": "Runs",
                    "results": [value] if value else [],
                }
            }
        if "reloadRepositoryLocation" in query:
            return {
                "reloadRepositoryLocation": {"__typename": "WorkspaceLocationEntry"}
            }
        if "launchPipelineExecution" in query:
            self.launches += 1
            metadata = variables["params"]["executionMetadata"]
            value = {
                "__typename": "Run",
                "runId": str(uuid4()),
                "status": "QUEUED",
                "tags": metadata["tags"],
            }
            observation = next(
                tag["value"]
                for tag in metadata["tags"]
                if tag["key"] == "hstream/observation_id"
            )
            self.runs[observation] = value
            if self.lose_response:
                self.lose_response = False
                raise urllib.error.URLError("accepted but response lost")
            return {
                "launchPipelineExecution": {
                    "__typename": "LaunchRunSuccess",
                    "run": value,
                }
            }
        raise AssertionError(query)


class ObserverTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        write_json(
            self.root / "config.json",
            {
                "schema": 1,
                "poll_interval_ms": 1,
                "observer_timeout_seconds": 1,
                "max_observations": 8,
                "public_url": "https://workflows.example.test/dagster",
                "max_concurrent_steps": 2,
            },
        )
        self.plan = fixture_plan()
        self.snapshot = graph.snapshot(self.plan, graph.initial_state(self.plan))

    def tearDown(self):
        self.temp.cleanup()

    def publish(self):
        return publish_snapshot(self.root, self.plan, self.snapshot)

    def terminal(self, states=None):
        self.snapshot["revision"] += 1
        self.snapshot["state"] = "succeeded"
        for node in self.snapshot["nodes"]:
            node["state"] = (states or {}).get(node["id"], "succeeded")

    def test_sanitized_projection_contains_no_inputs_runtime_result_or_errors(self):
        self.snapshot["nodes"][0].update(
            reason="password=secret /private/file",
            result={"path": "/private/artifact"},
            attempts=[
                {
                    "id": str(uuid4()),
                    "epoch": 0,
                    "state": "running",
                    "request_digest": "c" * 64,
                    "details": {
                        "scheduler_id": "123",
                        "host": "gpu01",
                        "error": "secret",
                        "request_path": "/private/request",
                    },
                }
            ],
        )
        observation = self.publish()
        registration, snapshot = read_observation(self.root, observation)
        contents = json.dumps([registration, snapshot])
        for sensitive in (
            "private",
            "secret",
            "request_path",
            "runtime_identity",
            "source_identity",
        ):
            self.assertNotIn(sensitive, contents)
        self.assertEqual(
            snapshot["nodes"][0]["attempts"][0]["details"],
            {"scheduler_id": "123", "host": "gpu01"},
        )

    def test_registration_is_stable_and_rejects_changed_revision_or_graph(self):
        first = self.publish()
        self.assertEqual(first, self.publish())
        self.snapshot["nodes"][0]["state"] = "running"
        with self.assertRaisesRegex(WorkflowError, "same revision"):
            self.publish()
        self.snapshot["revision"] += 1
        self.publish()
        self.snapshot["revision"] = 0
        with self.assertRaisesRegex(WorkflowError, "Stale"):
            self.publish()

    def test_native_retry_advances_epoch_observer_restart_does_not(self):
        original = self.publish()
        restarted = publish_snapshot(self.root, self.plan, self.snapshot, str(uuid4()))
        self.assertNotEqual(original, restarted)
        self.assertEqual(read_observation(self.root, restarted)[0]["epoch"], 0)
        self.snapshot["epoch"] = 1
        self.snapshot["revision"] += 1
        next_epoch = self.publish()
        self.assertNotEqual(original, next_epoch)
        self.assertEqual(len(observation_ids(self.root)), 3)
        self.assertEqual(read_observation(self.root, original)[0]["epoch"], 0)

    def test_observer_restart_follows_later_snapshots_and_old_epoch_is_superseded(self):
        original = self.publish()
        restarted = publish_snapshot(self.root, self.plan, self.snapshot, str(uuid4()))
        self.terminal()
        self.publish()
        self.assertEqual(
            read_observation(self.root, restarted)[1]["state"], "succeeded"
        )
        self.snapshot["epoch"] += 1
        self.snapshot["revision"] += 1
        self.publish()
        self.assertEqual(
            read_observation(self.root, original)[1]["superseded_by_epoch"], 1
        )
        self.snapshot["epoch"] = 0
        with self.assertRaisesRegex(WorkflowError, "earlier workflow epoch"):
            self.publish()

    def test_short_observation_retains_scheduler_start_and_end(self):
        observation = self.publish()
        dagster = FakeDagster()
        reconcile_observation(
            self.root, observation, "http://localhost:3001", request=dagster
        )
        dagster.runs[observation].update(
            status="SUCCESS", creationTime=1000.0, startTime=1001.0, endTime=1002.0
        )
        receipt = reconcile_observation(
            self.root, observation, "http://localhost:3001", request=dagster
        )
        self.assertEqual(
            receipt["lifecycle"],
            {"queued_at": 1000.0, "started_at": 1001.0, "ended_at": 1002.0},
        )
        self.assertEqual(dagster.launches, 1)

    def test_accepted_launch_response_loss_reconciles_exact_run(self):
        observation = self.publish()
        dagster = FakeDagster()
        dagster.lose_response = True
        with self.assertRaises(urllib.error.URLError):
            reconcile_observation(
                self.root, observation, "http://localhost:3001", request=dagster
            )
        self.assertEqual(
            read_json(observation_path(self.root, observation) / "launch.json")[
                "state"
            ],
            "submitting",
        )
        result = reconcile_observation(
            self.root, observation, "http://localhost:3001", request=dagster
        )
        self.assertEqual(dagster.launches, 1)
        self.assertEqual(result["state"], "QUEUED")
        self.assertEqual(
            result["url"],
            "https://workflows.example.test/dagster/runs/" + result["run_id"],
        )

    def test_public_url_can_disable_links_without_disabling_observation(self):
        config = read_json(self.root / "config.json")
        config["public_url"] = None
        write_json(self.root / "config.json", config)
        result = reconcile_observation(
            self.root, self.publish(), "http://localhost:3001", request=FakeDagster()
        )
        self.assertEqual(result["state"], "QUEUED")
        self.assertIsNone(result["url"])

    def test_public_url_rejects_non_http_credentials_queries_and_fragments(self):
        config = read_json(self.root / "config.json")
        for address in (
            "",
            "/dagster",
            "file:///tmp/dagster",
            "https://user:secret@host",
            "https://host/?token=secret",
            "https://host/#fragment",
            123,
        ):
            with self.subTest(address=address):
                config["public_url"] = address
                write_json(self.root / "config.json", config)
                with self.assertRaises(WorkflowError):
                    configuration(self.root)

    def test_unresolved_launch_never_resubmits_on_absent_lookup(self):
        observation = self.publish()
        dagster = FakeDagster()
        dagster.lose_response = True
        with self.assertRaises(urllib.error.URLError):
            reconcile_observation(
                self.root, observation, "http://localhost:3001", request=dagster
            )
        accepted = dagster.runs.pop(observation)
        with self.assertRaisesRegex(WorkflowError, "does not authorize"):
            reconcile_observation(
                self.root, observation, "http://localhost:3001", request=dagster
            )
        self.assertEqual(dagster.launches, 1)
        dagster.runs[observation] = accepted
        self.assertEqual(
            reconcile_observation(
                self.root, observation, "http://localhost:3001", request=dagster
            )["state"],
            "QUEUED",
        )

    def test_publication_never_waits_for_a_busy_observer_registry(self):
        with locked(self.root / "registry.lock"), self.assertRaises(BlockingIOError):
            self.publish()

    def test_same_run_id_with_foreign_tags_is_never_adopted(self):
        observation = self.publish()
        dagster = FakeDagster()
        dagster.runs[observation] = {
            "__typename": "Run",
            "runId": observation,
            "status": "SUCCESS",
            "tags": [],
        }
        with self.assertRaisesRegex(WorkflowError, "tags"):
            reconcile_observation(
                self.root, observation, "http://localhost:3001", request=dagster
            )
        self.assertEqual(dagster.launches, 0)

    def test_retirement_requires_both_native_and_observer_completion(self):
        observation = self.publish()
        dagster = FakeDagster()
        reconcile_observation(
            self.root, observation, "http://localhost:3001", request=dagster
        )
        with self.assertRaisesRegex(WorkflowError, "shutdown"):
            retire_observation(self.root, observation)
        dagster.runs[observation]["status"] = "SUCCESS"
        reconcile_observation(
            self.root, observation, "http://localhost:3001", request=dagster
        )
        with self.assertRaisesRegex(WorkflowError, "still active"):
            retire_observation(self.root, observation)
        self.terminal()
        self.publish()
        retire_observation(self.root, observation)
        self.assertEqual(observation_ids(self.root), [])
        self.publish()
        self.assertEqual(observation_ids(self.root), [])
        self.assertEqual(dagster.runs[observation]["status"], "SUCCESS")

    def test_untrusted_symlink_and_resource_bounds(self):
        self.root.joinpath("observations").symlink_to(
            self.root, target_is_directory=True
        )
        with self.assertRaises(WorkflowError):
            self.publish()
        self.root.joinpath("observations").unlink()
        config = read_json(self.root / "config.json")
        config["max_concurrent_steps"] = 33
        write_json(self.root / "config.json", config)
        with self.assertRaises(WorkflowError):
            configuration(self.root)

    def test_stage_name_mapping_is_stable_and_collision_resistant(self):
        self.assertNotEqual(operation_name("solve-A"), operation_name("solve.A"))
        self.assertEqual(operation_name("solve-A"), operation_name("solve-A"))

    @unittest.skipUnless(
        importlib.util.find_spec("dagster"), "optional pinned Dagster is not installed"
    )
    def test_real_graph_late_attach_failure_sibling_retry_and_observer_restart(self):
        self.terminal({"solve-A": "failed", "solve.B": "reused"})
        observation = self.publish()
        job = make_job(self.root, observation)
        result = job.execute_in_process(raise_on_error=False)
        self.assertFalse(result.success)
        succeeded = {
            event.step_key for event in result.all_events if event.is_step_success
        }
        self.assertIn(operation_name("solve.B", observation), succeeded)
        self.assertIn(operation_name("independent", observation), succeeded)
        # Native retry is a new epoch; completed siblings remain retained.
        self.snapshot["epoch"] += 1
        self.terminal()
        retried = self.publish()
        self.assertTrue(make_job(self.root, retried).execute_in_process().success)
        # Reexecuting an observer changes no native plan, state or attempt.
        before = copy.deepcopy(self.snapshot)
        restarted = publish_snapshot(self.root, self.plan, self.snapshot, str(uuid4()))
        self.assertTrue(make_job(self.root, restarted).execute_in_process().success)
        self.assertEqual(self.snapshot, before)

    @unittest.skipUnless(
        importlib.util.find_spec("dagster"), "optional pinned Dagster is not installed"
    )
    def test_real_graph_prerequisite_failure_skips_only_dependents(self):
        self.terminal(
            {"baseline": "unavailable", "solve-A": "blocked", "solve.B": "blocked"}
        )
        observation = self.publish()
        result = make_job(self.root, observation).execute_in_process(
            raise_on_error=False
        )
        self.assertFalse(result.success)
        self.assertTrue(
            any(
                event.is_step_success
                and event.step_key == operation_name("independent", observation)
                for event in result.all_events
            )
        )
        self.assertFalse(
            any(
                event.is_step_start
                and event.step_key == operation_name("solve-A", observation)
                for event in result.all_events
            )
        )


if __name__ == "__main__":
    unittest.main()
