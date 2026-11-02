"""Real local subprocesses exercise controller/worker crash and ownership boundaries."""

import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from uuid import uuid4

from hstream_workflow.controller import Controller, SubmissionRejected
from hstream_workflow.files import file_digest, locked, read_json, write_json
from hstream_workflow.profiles import controller_files
from hstream_workflow.schema import WorkflowError, digest

TASK = r"""
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--admission'); p.add_argument('--request', required=True)
a=p.parse_args(); r=json.loads(Path(a.request).read_text()); game=Path(r['game']); owner=game/'_hockeymon/test-owner.json'
owner.parent.mkdir(parents=True,exist_ok=True)
old=json.loads(owner.read_text()) if owner.exists() else None
if a.admission:
 if a.admission=='acquire':
  if old and old['token'] != r['token']:
   print(json.dumps({'error':'Another game operation owns this recording','code':'conflict'})); sys.exit(2)
  old=old or {'token':r['token'],'kind':r['kind'],'id':r['id'],'revision':1,'workflow_directory':r['workflow_directory']}
  owner.write_text(json.dumps(old))
  if (game/'hold-acquire').exists():
   (game/'acquire-ready').write_text(str(os.getpid()))
   until=time.monotonic()+10
   while not (game/'release-acquire').exists() and time.monotonic()<until: time.sleep(.01)
 elif a.admission=='cancel':
  old['cancel_requested']=True; old['revision']+=1; owner.write_text(json.dumps(old))
 elif a.admission=='release':
  assert old['token']==r['token'] and old['revision']==r['revision'] and r['stopped_and_clean']
  if (game/'hold-release').exists():
   (game/'release-ready').write_text(str(os.getpid()))
   until=time.monotonic()+10
   while not (game/'release-release').exists() and time.monotonic()<until: time.sleep(.01)
  owner.unlink(); old=None
 elif a.admission in {'adopt','adopt-selection','adoption-status'}:
  action=r.get('adoption',a.admission)
  record=game/('_hockeymon/test-'+action+'.json')
  exact={k:r[k] for k in ('workflow_id','attempt_id','catalog_key','workspace')}
  if a.admission=='adoption-status':
   accepted=json.loads(record.read_text()) if record.exists() else None
   assert accepted is None or accepted==exact, 'A different result was already adopted'
   print(json.dumps({'adopted':accepted is not None})); sys.exit(0)
  if record.exists(): assert json.loads(record.read_text())==exact, 'A different result was already adopted'
  else: record.write_text(json.dumps(exact))
  if (game/'hold-adopt').exists():
   (game/'adopt-ready').write_text(str(os.getpid()))
   until=time.monotonic()+10
   while not (game/'release-adopt').exists() and time.monotonic()<until: time.sleep(.01)
 print(json.dumps({'owner':old})); sys.exit(0)
root=Path(r['root']); inputs=r['inputs']
if inputs.get('detached'):
 child=subprocess.Popen([sys.executable,'-c','import os,time; os.setsid(); time.sleep(120)'])
 (root/'detached.pid').write_text(str(child.pid))
time.sleep(inputs.get('delay',0))
if inputs.get('fail'): print('fixture solver failed',flush=True); sys.exit(9)
output=root/'output.txt'; output.write_text(r['stage_id'])
result={key:r[key] for key in ('workflow_id','plan_digest','stage_id','attempt_id')}
result.update(schema=1,status='unavailable' if inputs.get('unavailable') else 'succeeded',outputs={'workspace':str(output)},message='fixture completed',artifacts=[{'identity_schema':2,'storage_namespace':r['runtime']['storage_namespace'],'path':'output.txt','kind':'data','size':output.stat().st_size,'sha256':hashlib.sha256(output.read_bytes()).hexdigest()}])
if inputs.get('catalog_key'): result['outputs'].update(catalog_key=inputs['catalog_key'],adoption=inputs.get('adoption','complete'))
Path(r['result_path']).write_text(json.dumps(result))
"""


class ControllerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix="hstream-workflow-controller-"
        )
        self.root = Path(self.temporary.name)
        self.game = self.root / "game"
        self.game.mkdir(mode=0o700)
        self.registry = self.root / "registry"
        self.registry.mkdir(mode=0o700)
        self.task = self.root / "task.py"
        self.task.write_text(TASK)
        python = str(Path(sys.executable).resolve())
        self.profile = {
            "executor": "local",
            "pool": "cpu",
            "task_command": [python, str(self.task)],
            "runtime": {
                "files": {
                    path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                    for path in (python, str(self.task), *map(str, controller_files()))
                },
                "storage_namespace": "fixture",
                "backend": "cpu",
                "architecture": os.uname().machine,
            },
            "capabilities": ["solve"],
            "resources": {
                "gpus": 0,
                "cpus": 1,
                "memory_mb": 128,
                "time_seconds": 30,
                "partition": "",
                "gres": "",
                "nodes": [],
                "constraint": "",
                "account": "",
                "qos": "",
                "devices": [],
            },
            "limits": {"concurrency": 2, "per_user": 2},
            "qualified_nodes": {},
        }
        defaults = {
            "max_parallel": 2,
            "poll_interval_ms": 10,
            "command_timeout_seconds": 5,
            "shutdown_timeout_seconds": 1,
            "observer_timeout_seconds": 60,
        }
        write_json(
            self.registry / "config.json",
            {
                "schema": 1,
                "defaults": defaults,
                "profiles": {"cpu": self.profile},
                "admission_profile": "cpu",
                "allowed_game_roots": [str(self.root)],
                "observer_registry": None,
            },
        )
        self.controller = Controller(self.registry)
        self.plan = {
            "schema": 1,
            "workflow_id": str(uuid4()),
            "kind": "stitching-experiments",
            "source_identity": {"game_directory": str(self.game)},
            "configuration_digest": "1" * 64,
            "runtime_identity": {"revision": "fixture"},
            "profiles": {"cpu": self.profile},
            "limits": {"max_parallel": 2},
            "stages": [self.stage("a"), self.stage("b")],
        }

    def tearDown(self):
        # Every admitted fixture is cancelled/reconciled before removing its files.
        for entry in self.controller.registry.read()["workflows"].values():
            plan = read_json(Path(entry["directory"]) / "plan.json")
            state = self.controller.status(plan["workflow_id"])
            if state["state"] not in {"succeeded", "failed", "cancelled"}:
                self.controller.cancel(plan["workflow_id"])
                self.wait(plan["workflow_id"])
        self.temporary.cleanup()

    @staticmethod
    def stage(key, dependencies=None, **inputs):
        return {
            "id": key,
            "kind": "solve",
            "label": key,
            "candidate_id": key,
            "dependencies": dependencies or [],
            "inputs": inputs,
            "profile_id": "cpu",
        }

    def wait(self, workflow_id=None):
        workflow_id = workflow_id or self.plan["workflow_id"]
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            self.controller.tick(workflow_id)
            value = self.controller.status(workflow_id)
            if value["state"] in {"succeeded", "failed", "cancelled"}:
                return value
            time.sleep(0.02)
        self.fail(json.dumps(value))

    def test_parallel_children_finish_after_controller_recreation(self):
        for stage in self.plan["stages"]:
            stage["inputs"]["delay"] = 0.3
        self.controller.register(self.plan, self.game)
        self.controller.tick(self.plan["workflow_id"])
        self.assertEqual(len(self.controller.registry.read()["reservations"]), 2)
        self.controller = Controller(self.registry)
        result = self.wait()
        self.assertEqual(result["state"], "succeeded")
        self.assertFalse((self.game / "_hockeymon/test-owner.json").exists())
        self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_profile_rejection_has_exact_unregistered_submission_identity(self):
        self.controller.config["profiles"]["cpu"]["label"] = "changed"
        with self.assertRaises(SubmissionRejected) as failure:
            self.controller.register(self.plan, self.game)
        self.assertEqual(failure.exception.workflow_id, self.plan["workflow_id"])
        self.assertEqual(self.controller.registry.read()["workflows"], {})
        self.assertFalse((self.game / "_hockeymon/test-owner.json").exists())

    def test_cancel_reconciles_committed_acquire_with_lost_response(self):
        native = self.controller._native

        def lose_response(directory, action, payload, **kwargs):
            result = native(directory, action, payload, **kwargs)
            if action == "acquire":
                raise OSError("response lost after claim")
            return result

        with patch.object(
            self.controller, "_native", side_effect=lose_response
        ), self.assertRaises(OSError):
            self.controller.register(self.plan, self.game)
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        self.assertFalse(read_json(directory / "owner.json")["acquired"])
        self.assertTrue((self.game / "_hockeymon/test-owner.json").exists())
        self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        result = self.wait()
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(result["owner_released"])
        self.assertFalse((self.game / "_hockeymon/test-owner.json").exists())
        self.controller.retry(self.plan["workflow_id"], ["a", "b"], expected_epoch=0)
        self.assertEqual(self.wait()["state"], "succeeded")

    def test_cancel_waits_for_orphaned_native_acquire_helper(self):
        (self.game / "hold-acquire").touch()
        plan = self.root / "plan.json"
        write_json(plan, self.plan)
        parent = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).parent / "hstream-workflow"),
                "submit",
                "--registry",
                str(self.registry),
                "--plan",
                str(plan),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        outcomes, failures = [], []
        thread = None
        try:
            deadline = time.monotonic() + 5
            while (
                not (self.game / "acquire-ready").exists()
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            self.assertTrue((self.game / "acquire-ready").exists())
            parent.kill()
            parent.wait(timeout=5)
            entered = threading.Event()

            def cancel():
                entered.set()
                try:
                    outcomes.append(
                        self.controller.cancel(
                            self.plan["workflow_id"], expected_epoch=0
                        )
                    )
                except Exception as exc:  # noqa: BLE001 - retain thread failures
                    failures.append(exc)

            thread = threading.Thread(target=cancel)
            thread.start()
            self.assertTrue(entered.wait(2))
            thread.join(0.1)
            self.assertTrue(
                thread.is_alive(), "Orphan helper lost the native admission barrier"
            )
            directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
            self.assertFalse((directory / "cancel-intent.json").exists())
        finally:
            (self.game / "release-acquire").touch()
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=5)
            if thread is not None:
                thread.join(5)
                self.assertFalse(thread.is_alive())
        self.assertFalse(failures)
        self.assertEqual(len(outcomes), 1)
        result = self.wait()
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(result["owner_released"])
        self.assertFalse((self.game / "_hockeymon/test-owner.json").exists())

    def test_real_local_fork_rejection_releases_game_and_allows_retry(self):
        self.controller.register(self.plan, self.game)
        script = """
import resource, sys
from hstream_workflow.controller import Controller
controller = Controller(sys.argv[1])
before = resource.getrlimit(resource.RLIMIT_NPROC)
try:
 resource.setrlimit(resource.RLIMIT_NPROC, (0, before[1]))
 controller.tick(sys.argv[2])
finally:
 resource.setrlimit(resource.RLIMIT_NPROC, before)
"""
        if os.geteuid() == 0:
            self.controller.cancel(self.plan["workflow_id"])
            self.wait()
            self.skipTest("Root is exempt from RLIMIT_NPROC")
        subprocess.run(
            [
                sys.executable,
                "-c",
                script,
                str(self.registry),
                self.plan["workflow_id"],
            ],
            check=True,
            env=dict(os.environ, PYTHONPATH=os.pathsep.join(sys.path)),
        )
        self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        result = self.wait()
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(result["owner_released"])
        self.assertEqual(self.controller.registry.read()["reservations"], {})
        self.controller.retry(self.plan["workflow_id"], ["a", "b"], expected_epoch=0)
        self.assertEqual(self.wait()["state"], "succeeded")

    def test_registered_or_unreadable_state_never_claims_nonacceptance(self):
        self.controller.register(self.plan, self.game)
        self.controller.config["profiles"]["cpu"]["label"] = "changed"
        with self.assertRaises(WorkflowError) as failure:
            self.controller.register(self.plan, self.game)
        self.assertNotIsInstance(failure.exception, SubmissionRejected)
        self.controller.config["profiles"]["cpu"].pop("label")
        self.wait()
        fresh = copy.deepcopy(self.plan)
        fresh["workflow_id"] = str(uuid4())
        fresh["profiles"]["cpu"]["label"] = "changed"
        with patch.object(
            self.controller.registry, "read", side_effect=OSError("unavailable")
        ), self.assertRaises(WorkflowError) as failure:
            self.controller.register(fresh, self.game)
        self.assertNotIsInstance(failure.exception, SubmissionRejected)

    def test_cli_classifies_profile_and_configuration_rejection(self):
        plan = self.root / "plan.json"
        write_json(plan, self.plan)
        config = read_json(self.registry / "config.json")
        for invalid_config in (False, True):
            with self.subTest(invalid_config=invalid_config):
                changed = copy.deepcopy(config)
                if invalid_config:
                    changed["defaults"] = {}
                else:
                    changed["profiles"]["cpu"]["label"] = "changed"
                write_json(self.registry / "config.json", changed)
                result = subprocess.run(
                    [
                        sys.executable,
                        str(Path(__file__).parent / "hstream-workflow"),
                        "submit",
                        "--registry",
                        str(self.registry),
                        "--plan",
                        str(plan),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)
                error = json.loads(result.stderr)
                self.assertEqual(error["submission"], "rejected")
                self.assertEqual(error["workflow_id"], self.plan["workflow_id"])
        write_json(self.registry / "config.json", config)

    def test_failed_sibling_retry_preserves_successful_result(self):
        self.plan["stages"][0]["inputs"]["fail"] = True
        self.controller.register(self.plan, self.game)
        result = self.wait()
        self.assertEqual(
            [node["state"] for node in result["nodes"]], ["failed", "succeeded"]
        )
        retained = result["nodes"][1]["result"]
        self.controller.retry(self.plan["workflow_id"], ["a"])
        result = self.wait()
        self.assertEqual(result["nodes"][1]["result"], retained)
        self.assertEqual(len(result["nodes"][0]["attempts"]), 2)
        events = self.controller.events(self.plan["workflow_id"], "a")["events"]
        self.assertEqual({event["epoch"] for event in events}, {0, 1})
        self.assertEqual(
            [event["revision"] for event in events],
            sorted({event["revision"] for event in events}),
        )
        count = len(events)
        self.controller.tick(self.plan["workflow_id"])
        self.assertEqual(
            len(self.controller.events(self.plan["workflow_id"], "a")["events"]), count
        )

    def test_stop_retires_detached_descendant_and_prevents_dependents(self):
        self.plan["stages"] = [
            self.stage("a", delay=10, detached=True),
            self.stage("b", ["a"]),
        ]
        self.controller.register(self.plan, self.game)
        self.controller.tick(self.plan["workflow_id"])
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        deadline = time.monotonic() + 5
        while (
            not list(directory.glob("attempts/*/detached.pid"))
            and time.monotonic() < deadline
        ):
            time.sleep(0.02)
        pid_path = next(directory.glob("attempts/*/detached.pid"))
        pid = int(pid_path.read_text())
        self.controller.cancel(self.plan["workflow_id"])
        result = self.wait()
        self.assertEqual(result["state"], "cancelled")
        self.assertFalse(Path(f"/proc/{pid}").exists())
        self.assertEqual(result["nodes"][1]["attempts"], [])

    def test_definite_admission_conflict_never_starts_after_owner_releases(self):
        self.controller.register(self.plan, self.game)
        rejected = copy.deepcopy(self.plan)
        rejected["workflow_id"] = str(uuid4())
        with self.assertRaisesRegex(WorkflowError, "Another game operation"):
            self.controller.register(rejected, self.game)
        self.wait()
        result = self.controller.tick(rejected["workflow_id"])
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(all(not node["attempts"] for node in result["nodes"]))
        self.assertFalse((self.game / "_hockeymon/test-owner.json").exists())

    def test_cancel_and_status_do_not_wait_for_busy_controller(self):
        self.controller.register(self.plan, self.game)
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        with locked(directory / "controller.lock"):
            started = time.monotonic()
            status = self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
            self.assertLess(time.monotonic() - started, 2)
            self.assertTrue(status["cancel_requested"])
            self.assertTrue(status["cancellation_pending"])
        result = self.wait()
        self.assertTrue(all(not node["attempts"] for node in result["nodes"]))

    def completed_attempt_before_adoption(self):
        self.plan["stages"] = [self.stage("a")]
        self.controller.register(self.plan, self.game)
        self.controller.tick(self.plan["workflow_id"])
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        root = next((directory / "attempts").iterdir())
        until = time.monotonic() + 10
        while not (root / "completion.json").exists() and time.monotonic() < until:
            time.sleep(0.02)
        self.assertTrue((root / "completion.json").exists())
        return directory, root

    def cancel_without_success_file(self, missing):
        _, root = self.completed_attempt_before_adoption()
        (root / missing).unlink()
        self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        result = self.wait()
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(result["owner_released"])
        self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_cancel_stopped_attempt_without_success_receipt(self):
        self.cancel_without_success_file("native-result.json")

    def test_cancel_stopped_attempt_without_success_artifact(self):
        self.cancel_without_success_file("output.txt")

    def test_cancel_cannot_release_foreign_completion(self):
        _, root = self.completed_attempt_before_adoption()
        completion = read_json(root / "completion.json")
        changed = {**completion, "attempt_id": str(uuid4())}
        write_json(root / "completion.json", changed)
        self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        result = self.controller.tick(self.plan["workflow_id"])
        self.assertEqual(result["state"], "recovery")
        self.assertFalse(
            self.controller.status(self.plan["workflow_id"])["owner_released"]
        )
        self.assertEqual(len(self.controller.registry.read()["reservations"]), 1)
        write_json(root / "completion.json", completion)
        self.assertEqual(self.wait()["state"], "cancelled")

    def test_cancel_committed_before_publication_prevents_adoption(self):
        self.completed_attempt_before_adoption()
        verify = self.controller._verify_result

        def verified(*args):
            result = verify(*args)
            result["outputs"].update(
                catalog_key="candidate", workspace={"fixture": "result"}
            )
            with patch.object(
                self.controller,
                "_cancel_owner",
                side_effect=OSError("cancel helper timed out"),
            ):
                self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
            return result

        with patch.object(
            self.controller, "_verify_result", side_effect=verified
        ), patch.object(
            self.controller, "_native", wraps=self.controller._native
        ) as native:
            result = self.controller.tick(self.plan["workflow_id"])
        self.assertFalse(
            any(
                call.args[1] in {"adopt", "adopt-selection"}
                for call in native.call_args_list
            )
        )
        self.assertEqual(result["state"], "cancelled")
        self.assertTrue(
            self.controller.status(self.plan["workflow_id"])["owner_released"]
        )

    def test_inflight_adoption_publishes_before_later_cancel(self):
        directory, _ = self.completed_attempt_before_adoption()
        verify, native = self.controller._verify_result, self.controller._native
        attempted = threading.Event()
        outcomes, failures, threads = [], [], []

        def verified(*args):
            result = verify(*args)
            result["outputs"].update(
                catalog_key="candidate", workspace={"fixture": "result"}
            )
            return result

        def cancel():
            attempted.set()
            try:
                outcomes.append(
                    self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
                )
            except Exception as exc:  # noqa: BLE001 - retain thread failures
                failures.append(exc)

        def adopting(directory, action, payload, **kwargs):
            if action == "adopt":
                thread = threading.Thread(target=cancel)
                threads.append(thread)
                thread.start()
                self.assertTrue(attempted.wait(2))
                thread.join(0.1)
                self.assertTrue(thread.is_alive())
                self.assertFalse((directory / "cancel-intent.json").exists())
            return native(directory, action, payload, **kwargs)

        try:
            with patch.object(
                self.controller, "_verify_result", side_effect=verified
            ), patch.object(self.controller, "_native", side_effect=adopting):
                result = self.controller.tick(self.plan["workflow_id"])
        finally:
            for thread in threads:
                thread.join(5)
                self.assertFalse(thread.is_alive())
        self.assertFalse(failures)
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(result["state"], "succeeded")
        self.assertFalse(read_json(directory / "state.json")["cancel_requested"])
        self.assertFalse((directory / "cancel-intent.json").exists())

    def test_cancel_reconciles_committed_adoption_after_lost_response(self):
        for adoption, action in (
            ("complete", "adopt"),
            ("selection", "adopt-selection"),
        ):
            with self.subTest(adoption=adoption):
                plan = copy.deepcopy(self.plan)
                plan["workflow_id"] = str(uuid4())
                plan["stages"] = [
                    self.stage("a", catalog_key="candidate", adoption=adoption)
                ]
                self.controller.register(plan, self.game)
                native = self.controller._native

                def lost_response(
                    directory,
                    command,
                    payload,
                    *,
                    _native=native,
                    _action=action,
                    **kwargs,
                ):
                    response = _native(directory, command, payload, **kwargs)
                    if command == _action:
                        raise OSError("Lost native adoption response after commit")
                    return response

                with patch.object(
                    self.controller, "_native", side_effect=lost_response
                ):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        status = self.controller.tick(plan["workflow_id"])
                        if status["state"] == "recovery":
                            break
                        time.sleep(0.02)
                self.assertEqual(status["state"], "recovery")
                directory, _ = self.controller.registry.lookup(plan["workflow_id"])
                root = next((directory / "attempts").iterdir())
                for name in ("native-result.json", "output.txt"):
                    (root / name).unlink()
                self.controller.cancel(plan["workflow_id"], expected_epoch=0)
                with patch.object(self.controller, "_native", wraps=native) as calls:
                    status = self.wait(plan["workflow_id"])
                self.assertEqual(status["state"], "succeeded")
                self.assertEqual(status["nodes"][0]["state"], "succeeded")
                self.assertTrue(status["owner_released"])
                self.assertFalse(
                    any(
                        call.args[1] in {"adopt", "adopt-selection"}
                        for call in calls.call_args_list
                    )
                )
                self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_cancel_uncommitted_adoption_intent_without_output_artifacts(self):
        self.plan["stages"] = [self.stage("a", catalog_key="candidate")]
        self.controller.register(self.plan, self.game)
        native = self.controller._native

        def not_committed(directory, action, payload, **kwargs):
            if action == "adopt":
                raise OSError("Native helper did not accept adoption")
            return native(directory, action, payload, **kwargs)

        with patch.object(self.controller, "_native", side_effect=not_committed):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                status = self.controller.tick(self.plan["workflow_id"])
                if status["state"] == "recovery":
                    break
                time.sleep(0.02)
        self.assertEqual(status["state"], "recovery")
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        root = next((directory / "attempts").iterdir())
        self.assertTrue((root / "adoption-intent.json").exists())
        for name in ("native-result.json", "output.txt"):
            (root / name).unlink()
        self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        self.assertEqual(self.wait()["state"], "cancelled")

    def orphan_control(self, action, command):
        (self.game / ("hold-" + action)).touch()
        parent = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).parent / "hstream-workflow"),
                command,
                "--registry",
                str(self.registry),
                "--workflow",
                self.plan["workflow_id"],
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 5
        while (
            not (self.game / (action + "-ready")).exists()
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)
        self.assertTrue((self.game / (action + "-ready")).exists())
        parent.kill()
        parent.wait(timeout=5)
        return self.game / ("release-" + action)

    def test_cancel_waits_for_orphaned_adoption_and_retains_committed_result(self):
        self.plan["stages"] = [self.stage("a", catalog_key="candidate")]
        self.controller.register(self.plan, self.game)
        self.controller.tick(self.plan["workflow_id"])
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        root = next((directory / "attempts").iterdir())
        deadline = time.monotonic() + 5
        while not (root / "completion.json").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((root / "completion.json").exists())
        release = self.orphan_control("adopt", "reconcile")
        outcomes = []
        thread = threading.Thread(
            target=lambda: outcomes.append(
                self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
            )
        )
        try:
            thread.start()
            thread.join(0.1)
            self.assertTrue(thread.is_alive())
            self.assertFalse((directory / "cancel-intent.json").exists())
        finally:
            release.touch()
            thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(self.wait()["state"], "succeeded")

    def test_retry_waits_for_orphaned_release_and_confirmed_ownership_retirement(self):
        self.plan["stages"] = [self.stage("a", fail=True)]
        self.controller.register(self.plan, self.game)
        self.controller.tick(self.plan["workflow_id"])
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        root = next((directory / "attempts").iterdir())
        deadline = time.monotonic() + 5
        while not (root / "completion.json").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((root / "completion.json").exists())
        release = self.orphan_control("release", "reconcile")
        errors = []

        def retry():
            try:
                self.controller.retry(self.plan["workflow_id"], ["a"], expected_epoch=0)
            except WorkflowError as exc:
                errors.append(str(exc))

        thread = threading.Thread(target=retry)
        try:
            thread.start()
            thread.join(0.1)
            self.assertTrue(thread.is_alive())
            self.assertEqual(read_json(directory / "state.json")["epoch"], 0)
        finally:
            release.touch()
            thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(
            errors, ["Wait for confirmed game ownership release before retrying"]
        )
        self.assertTrue(self.wait()["owner_released"])
        self.controller.retry(self.plan["workflow_id"], ["a"], expected_epoch=0)
        self.wait()

    def test_old_epoch_cannot_cancel_or_retry_a_new_attempt(self):
        self.plan["stages"] = [self.stage("a", fail=True)]
        self.controller.register(self.plan, self.game)
        self.wait()
        self.controller.retry(self.plan["workflow_id"], ["a"], expected_epoch=0)
        with self.assertRaisesRegex(WorkflowError, "Stale workflow epoch"):
            self.controller.cancel(self.plan["workflow_id"], expected_epoch=0)
        with self.assertRaisesRegex(WorkflowError, "Stale workflow epoch"):
            self.controller.retry(self.plan["workflow_id"], ["a"], expected_epoch=0)
        self.wait()

    def test_crash_after_reservation_before_attempt_releases_capacity(self):
        self.controller.register(self.plan, self.game)
        with patch(
            "hstream_workflow.controller.graph.begin_attempt",
            side_effect=OSError("crash"),
        ), self.assertRaises(OSError):
            self.controller.tick(self.plan["workflow_id"])
        self.assertEqual(len(self.controller.registry.read()["reservations"]), 1)
        self.controller = Controller(self.registry)
        self.assertEqual(self.wait()["state"], "succeeded")
        self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_crash_after_terminal_save_before_capacity_release_reconciles(self):
        self.controller.register(self.plan, self.game)
        original = self.controller.registry.release
        count = 0

        def crash(*args, **kwargs):
            nonlocal count
            count += 1
            if count == 1:
                raise OSError("crash after terminal save")
            return original(*args, **kwargs)

        with patch.object(
            self.controller.registry, "release", side_effect=crash
        ), self.assertRaisesRegex(OSError, "crash after terminal save"):
            self.wait()
        self.controller = Controller(self.registry)
        self.wait()
        self.controller.tick(self.plan["workflow_id"])
        self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_partial_registration_is_completed_before_native_acquisition(self):
        directory = self.game / "_hockeymon/workflows" / self.plan["workflow_id"]
        directory.mkdir(parents=True, mode=0o700)
        write_json(directory / "plan.json", self.plan)
        self.controller.register(self.plan, self.game)
        self.assertEqual(self.wait()["state"], "succeeded")

    def test_shared_pool_limit_applies_across_controllers_and_games(self):
        self.plan["stages"] = [self.stage("a", delay=0.3), self.stage("b", delay=0.3)]
        self.controller.register(self.plan, self.game)
        other_game = self.root / "game2"
        other_game.mkdir(mode=0o700)
        second = copy.deepcopy(self.plan)
        second["workflow_id"] = str(uuid4())
        second["source_identity"]["game_directory"] = str(other_game)
        other = Controller(self.registry)
        other.register(second, other_game)
        self.controller.tick(self.plan["workflow_id"])
        other.tick(second["workflow_id"])
        self.assertEqual(len(self.controller.registry.read()["reservations"]), 2)
        self.assertTrue(
            all(
                not node["attempts"]
                for node in other.status(second["workflow_id"])["nodes"]
            )
        )
        self.wait()
        self.assertEqual(self.wait(second["workflow_id"])["state"], "succeeded")

    def test_successful_slurm_job_waits_for_delayed_nfs_receipt(self):
        self.plan["stages"] = [self.stage("a")]
        profile = copy.deepcopy(self.profile)
        profile["executor"] = "slurm"
        profile["pool"] = "cluster"
        profile["runtime"]["python"] = str(Path(sys.executable).resolve())
        profile["resources"].update(partition="cpu", nodes=["compute"])
        profile["qualified_nodes"] = {
            "compute": {
                "runtime_identity": copy.deepcopy(profile["runtime"]),
                "capabilities": profile["capabilities"],
            }
        }
        self.plan["profiles"]["cpu"] = profile
        config = read_json(self.registry / "config.json")
        config["profiles"] = {"cpu": profile, "helper": self.profile}
        config["admission_profile"] = "helper"
        write_json(self.registry / "config.json", config)
        self.controller = Controller(self.registry)
        self.controller.register(self.plan, self.game)
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])

        def submit(root, *_args):
            write_json(
                root / "dispatch.json", {"attempt_id": root.name, "backend": "slurm"}
            )
            return {"scheduler_id": "42", "cluster": "fixture"}

        with patch("hstream_workflow.controller.slurm.submit", side_effect=submit):
            self.controller.tick(self.plan["workflow_id"])
        root = next((directory / "attempts").iterdir())
        request = read_json(root / "request.json")
        subprocess.run(
            [*profile["task_command"], "--request", str(root / "request.json")],
            check=True,
        )
        observation = {
            "scheduler_id": "42",
            "cluster": "fixture",
            "state": "COMPLETED",
            "exit_code": "0:0",
            "terminal": True,
        }
        with patch(
            "hstream_workflow.controller.slurm.lookup", return_value=observation
        ):
            self.controller.tick(self.plan["workflow_id"])
            status = self.controller.status(self.plan["workflow_id"])
            self.assertEqual(status["state"], "recovery")
            self.assertFalse(status["owner_released"])
            self.assertEqual(len(self.controller.registry.read()["reservations"]), 1)
            self.assertTrue((root / "output.txt").exists())
            write_json(
                root / "completion.json",
                {
                    "schema": 1,
                    "attempt_id": root.name,
                    "request_digest": digest(request),
                    "profile_digest": digest(profile),
                    "stopped": True,
                    "cleanup_complete": True,
                    "exit_code": 0,
                    "receipt": file_digest(root / "native-result.json", 4096),
                },
            )
            self.controller.tick(self.plan["workflow_id"])
        status = self.controller.status(self.plan["workflow_id"])
        self.assertEqual(status["state"], "succeeded")
        self.assertTrue(status["owner_released"])
        self.assertEqual(len(status["nodes"][0]["attempts"]), 1)
        self.assertEqual(self.controller.registry.read()["reservations"], {})

    def test_status_revision_is_the_persisted_revision(self):
        self.controller.register(self.plan, self.game)
        directory, _ = self.controller.registry.lookup(self.plan["workflow_id"])
        self.assertEqual(
            self.controller.status(self.plan["workflow_id"])["revision"],
            read_json(directory / "state.json")["revision"],
        )

    def test_repeated_submit_attaches_without_new_attempts(self):
        self.controller.register(self.plan, self.game)
        self.controller.register(self.plan, self.game)
        result = self.wait()
        self.assertTrue(all(len(node["attempts"]) == 1 for node in result["nodes"]))
        self.assertEqual(len(self.controller.registry.read()["workflows"]), 1)


if __name__ == "__main__":
    unittest.main()
