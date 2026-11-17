"""Saved GPU-family repairs must retain selected computers and resource intent."""

import copy
import unittest
from unittest.mock import patch

import hstream_workflow_setup_test as fixtures
from hstream_workflow import setup_execution as execution
from hstream_workflow import setup_runtime as runtime
from hstream_workflow.schema import WorkflowError
from hstream_workflow.setup_gpus import needs_inventory, resolve_profiles


class SetupGpuChoicesTest(unittest.TestCase):
    def profile(self, gres="gpu:rtx:1", nodes=None, partition="gpu"):
        return {
            "saved": {
                "label": "RTX computers",
                "gres": gres,
                "nodes": nodes or ["mini"],
                "partition": partition,
            }
        }

    def test_family_resolves_each_node_without_broadening_or_changing_count(self):
        profiles = self.profile("gpu:rtx:2", ["mini", "monster"])
        original = copy.deepcopy(profiles)
        choices, messages = resolve_profiles(
            profiles,
            "mini|gpu*|gpu:rtx5090:2(S:0,1),gpu:rx9060xt:1(S:0)\n"
            "monster|gpu|gpu:rtx4090:2(S:0-1)\n",
        )
        self.assertEqual(
            choices["saved"],
            {"mini": "gpu:rtx5090:2", "monster": "gpu:rtx4090:2"},
        )
        self.assertEqual(profiles, original)
        self.assertEqual(len(messages), 2)
        self.assertIn("updated the saved GPU choice", messages[0])

    def test_exact_type_wins_over_longer_types_and_preserves_request(self):
        choices, messages = resolve_profiles(
            self.profile("gpu:rtx:01"),
            "mini|gpu|gpu:rtx:1,gpu:rtx5090:1\n",
        )
        self.assertEqual(choices["saved"]["mini"], "gpu:rtx:01")
        self.assertEqual(messages, [])

    def test_unknown_or_ambiguous_family_does_not_guess(self):
        for gres, available in (
            ("gpu:rtx:1", "gpu:rtx4090:1,gpu:rtx5090:1"),
            ("gpu:h100:1", "gpu:rtx5090:1"),
            ("gpu:rtx:2", "gpu:rtx:1,gpu:rtx5090:2"),
            ("gpu:rtx:2", "gpu:rtx5090:1"),
            ("gpu:rtx:0", "gpu:rtx5090:1"),
            ("gpu:a10:1", "gpu:a100:1"),
            ("gpu:rtx409:1", "gpu:rtx4090:1"),
        ):
            with self.subTest(gres=gres, available=available), self.assertRaisesRegex(
                WorkflowError, "Advanced profile settings"
            ):
                resolve_profiles(self.profile(gres), "mini|gpu|" + available)

    def test_node_and_queue_membership_are_required(self):
        for inventory in ("monster|gpu|gpu:rtx5090:1", "mini|batch|gpu:rtx5090:1"):
            with self.subTest(inventory=inventory), self.assertRaisesRegex(
                WorkflowError, "not in the selected queue"
            ):
                resolve_profiles(self.profile(), inventory)
        choices, _ = resolve_profiles(
            self.profile(partition="gpu,batch"), "mini|batch|gpu:rtx5090:1"
        )
        self.assertEqual(choices["saved"]["mini"], "gpu:rtx5090:1")

    def test_untyped_and_compound_requests_are_not_reinterpreted(self):
        for gres in ("gpu:1", "gpu:2", "gpu:rtx:1,mps:50", "mps:100"):
            with self.subTest(gres=gres):
                self.assertEqual(
                    needs_inventory(self.profile(gres)), gres in {"gpu:1", "gpu:2"}
                )
                choices, messages = resolve_profiles(self.profile(gres), "")
                self.assertEqual(choices["saved"]["mini"], gres)
                self.assertEqual(messages, [])

    def test_generic_gpu_matches_only_a_single_reported_type(self):
        choices, _ = resolve_profiles(self.profile("gpu:1"), "mini|gpu|gpu:rtx5090:1")
        self.assertEqual(choices["saved"]["mini"], "gpu:rtx5090:1")
        for inventory in ("mini|gpu|gpu:rtx5090:1,gpu:a100:1", "mini|gpu|gpu:2", ""):
            choices, _ = resolve_profiles(self.profile("gpu:1"), inventory)
            self.assertEqual(choices["saved"]["mini"], "gpu:1")

    def test_bad_or_conflicting_inventory_is_not_used(self):
        for inventory in (
            "mini|gpu",
            "mini|gpu|gpu:rtx4090:1\nmini|gpu|gpu:rtx5090:1",
        ):
            with self.subTest(inventory=inventory), self.assertRaises(WorkflowError):
                resolve_profiles(self.profile(), inventory)

    def test_preparation_splits_gpu_types_and_runtime_without_multiplying_capacity(
        self,
    ):
        fixture = fixtures.SetupTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        profile = fixture.draft["profiles"]["gpu"]
        profile.update(
            gres="gpu:rtx:1",
            nodes=["mini", "meshy", "monster", "ripper"],
            account="science",
            qos="normal",
            constraint="nvidia",
        )

        def installed(root, cpu):
            task, configured = fixture.inspect(root, cpu=cpu)
            return {
                "source": "/opt/hstream",
                "task": task,
                "runtime": configured,
                "features": runtime.REQUIRED_FEATURES,
            }

        def node(registry, name, preferred, timeout, deployment_nodes):
            return installed(
                "/shared/" + (name if name in {"mini", "meshy"} else "old-os"), False
            )

        inventory = (
            "mini|gpu|gpu:rtx5090:1\nmeshy|gpu|gpu:rtx5090:1\n"
            "monster|gpu|gpu:rtx4090:1\nripper|gpu|gpu:rtx3060ti:1,gpu:rx9060xt:1\n"
        )
        with patch.object(
            runtime, "snapshot", return_value=installed("/shared/cpu", True)
        ), patch.object(
            runtime, "require_current_task", side_effect=lambda value, *args: value
        ), patch.object(
            runtime, "from_node", side_effect=node
        ), patch.object(
            runtime, "run_readonly", return_value=inventory
        ) as probe:
            draft, prepared, messages = runtime.prepare(
                fixture.registry, fixture.draft, 30, 3
            )
        probe.assert_called_once_with(
            ["sinfo", "--noheader", "--Node", "--format=%N|%P|%G"], 30
        )
        self.assertEqual(draft["profiles"]["gpu"]["gres"], "gpu:rtx:1")
        self.assertEqual(len(prepared["profiles"]["gpu"]), 4)
        configuration = execution.compile_configuration(
            draft, fixture.defaults, fixture.cpu, prepared=prepared
        )
        profiles = [
            value
            for key, value in configuration["profiles"].items()
            if key != "metadata"
        ]
        self.assertEqual(len(profiles), 4)
        self.assertEqual(len({value["pool"] for value in profiles}), 1)
        expected = {
            "mini": "rtx5090",
            "meshy": "rtx5090",
            "monster": "rtx4090",
            "ripper": "rtx3060ti",
        }
        for value in profiles:
            resources = value["resources"]
            self.assertEqual(
                resources["gres"], "gpu:" + expected[resources["nodes"][0]] + ":1"
            )
            for key in (
                "account",
                "qos",
                "constraint",
                "cpus",
                "memory_mb",
                "time_seconds",
            ):
                self.assertEqual(resources[key], profile[key])
            self.assertEqual(value["limits"], {"concurrency": 3, "per_user": 3})
        self.assertEqual(
            sum("updated the saved GPU choice" in message for message in messages), 4
        )

    def test_invalid_gpu_choice_fails_before_any_runtime_copy(self):
        fixture = fixtures.SetupTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        with patch.object(
            runtime, "run_readonly", return_value="mini|gpu|gpu:a100:1"
        ), patch.object(runtime, "snapshot") as snapshot, patch.object(
            runtime, "from_node"
        ) as remote, self.assertRaises(
            WorkflowError
        ):
            runtime.prepare(fixture.registry, fixture.draft, 30, 3)
        snapshot.assert_not_called()
        remote.assert_not_called()


if __name__ == "__main__":
    unittest.main()
