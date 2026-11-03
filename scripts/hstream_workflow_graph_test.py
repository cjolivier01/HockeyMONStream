"""Dependency and attempt ownership regressions, without a scheduler or GPU."""

import copy
import unittest
from uuid import uuid4

from hstream_workflow import graph
from hstream_workflow.schema import WorkflowError, validate_plan


def plan():
    return {
        "schema": 1,
        "workflow_id": str(uuid4()),
        "kind": "stitching-experiments",
        "source_identity": {"game": "fixture"},
        "configuration_digest": "1" * 64,
        "runtime_identity": {"revision": "fixture"},
        "profiles": {
            "gpu": {
                "executor": "slurm",
                "capabilities": ["solve", "scan", "materialize"],
            }
        },
        "limits": {"max_parallel": 2},
        "stages": [
            {
                "id": key,
                "kind": kind,
                "label": key,
                "candidate_id": key,
                "dependencies": dependencies,
                "inputs": {},
                "profile_id": "gpu",
            }
            for key, kind, dependencies in (
                ("baseline", "solve", []),
                ("scan", "scan", ["baseline"]),
                ("frames", "materialize", ["scan"]),
                ("a", "solve", ["frames"]),
                ("b", "solve", ["frames"]),
                ("ordinary", "solve", []),
            )
        ],
    }


class GraphTest(unittest.TestCase):
    def setUp(self):
        self.plan = plan()
        self.state = graph.initial_state(self.plan)

    def begin(self, key):
        attempt = str(uuid4())
        self.state = graph.begin_attempt(
            self.plan, self.state, key, attempt, "2" * 64, "slurm"
        )
        return attempt

    def finish(self, key, attempt, outcome="succeeded", **details):
        self.state = graph.update_attempt(
            self.plan,
            self.state,
            key,
            attempt,
            outcome,
            {
                "stopped": True,
                "cleanup_complete": True,
                "verified": True,
                "outputs": {"workspace": key},
                **details,
            },
        )

    def test_shared_preparation_then_parallel_siblings(self):
        baseline, ordinary = self.begin("baseline"), self.begin("ordinary")
        self.assertEqual(graph.ready_stages(self.plan, self.state), [])
        self.finish("baseline", baseline)
        scan = self.begin("scan")
        self.finish("scan", scan)
        frames = self.begin("frames")
        self.finish("frames", frames)
        self.finish("ordinary", ordinary)
        a, b = self.begin("a"), self.begin("b")
        self.finish("a", a, "failed", reason="Matcher unavailable")
        self.assertEqual(self.state["nodes"]["b"]["state"], "submitting")
        self.finish("b", b)
        self.assertEqual(graph.snapshot(self.plan, self.state)["state"], "failed")
        self.assertEqual(self.state["nodes"]["frames"]["state"], "succeeded")

    def test_baseline_failure_blocks_only_dependents_and_retry_reuses_sibling(self):
        baseline, ordinary = self.begin("baseline"), self.begin("ordinary")
        self.finish("baseline", baseline, "failed")
        self.assertEqual(self.state["nodes"]["frames"]["state"], "blocked")
        self.finish("ordinary", ordinary)
        self.state = graph.retry(self.plan, self.state, ["baseline"])
        self.assertEqual(self.state["epoch"], 1)
        self.assertEqual(self.state["nodes"]["ordinary"]["state"], "succeeded")
        self.assertEqual(self.state["nodes"]["a"]["state"], "waiting")
        retry = self.begin("baseline")
        with self.assertRaisesRegex(WorkflowError, "Stale attempt"):
            self.finish("baseline", baseline)
        self.finish("baseline", retry)
        self.assertEqual(self.state["nodes"]["scan"]["state"], "ready")

    def test_cancellation_wins_late_success_and_waits_for_cleanup(self):
        attempt = self.begin("baseline")
        self.state = graph.cancel(self.plan, self.state)
        with self.assertRaisesRegex(WorkflowError, "shutdown and cleanup"):
            graph.update_attempt(
                self.plan,
                self.state,
                "baseline",
                attempt,
                "succeeded",
                {"stopped": True},
            )
        self.finish("baseline", attempt)
        self.assertEqual(self.state["nodes"]["baseline"]["state"], "cancelled")
        self.assertIsNone(self.state["nodes"]["baseline"]["result"])
        self.assertEqual(graph.snapshot(self.plan, self.state)["state"], "cancelled")

    def test_unknown_scheduler_acceptance_blocks_retry_and_keeps_slot(self):
        attempt = self.begin("baseline")
        self.state = graph.update_attempt(
            self.plan,
            self.state,
            "baseline",
            attempt,
            "recovery",
            {"reason": "Accounting unavailable"},
        )
        with self.assertRaisesRegex(WorkflowError, "every attempt"):
            graph.retry(self.plan, self.state, ["baseline"])
        self.assertEqual(graph.snapshot(self.plan, self.state)["state"], "recovery")
        self.assertEqual(
            [row["id"] for row in graph.ready_stages(self.plan, self.state)],
            ["ordinary"],
        )

    def test_domain_unavailable_retains_successful_baseline(self):
        self.finish("baseline", self.begin("baseline"))
        self.finish(
            "scan",
            self.begin("scan"),
            "unavailable",
            reason="Insufficient player coverage",
        )
        self.assertEqual(self.state["nodes"]["baseline"]["state"], "succeeded")
        self.assertEqual(self.state["nodes"]["a"]["state"], "blocked")

    def test_inputs_and_plan_are_immutable(self):
        original = copy.deepcopy(self.state)
        self.begin("baseline")
        self.assertEqual(original["nodes"]["baseline"]["state"], "waiting")
        changed = copy.deepcopy(self.plan)
        changed["configuration_digest"] = "3" * 64
        with self.assertRaisesRegex(WorkflowError, "plan changed"):
            graph.snapshot(changed, self.state)

    def test_rejects_invalid_dependencies_and_unqualified_tasks(self):
        for mutate in (
            lambda p: p["stages"][0]["dependencies"].append("a"),
            lambda p: p["stages"][0]["dependencies"].append("missing"),
            lambda p: p["stages"].append(copy.deepcopy(p["stages"][0])),
            lambda p: p["stages"][0]["inputs"].update(
                frame={"stage": "frames", "output": "bundle"}
            ),
            lambda p: p["profiles"]["gpu"].update(capabilities=["scan"]),
        ):
            with self.subTest(mutate=mutate):
                changed = copy.deepcopy(self.plan)
                mutate(changed)
                with self.assertRaises(WorkflowError):
                    validate_plan(changed)


if __name__ == "__main__":
    unittest.main()
