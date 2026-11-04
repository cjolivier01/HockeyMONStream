#!/usr/bin/env python3
"""Qualify registration against isolated real Dagster webserver/daemon processes.

No native compute is launched and no production instance is read or changed.
Run using the pinned optional Dagster venv. All synthetic state is temporary.
"""

import importlib.util
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from hstream_workflow import graph
from hstream_workflow.dagster_registry import publish_snapshot, reconcile_observation
from hstream_workflow.files import write_json
from hstream_workflow_observer_test import fixture_plan


@unittest.skipUnless(
    importlib.util.find_spec("dagster"), "requires pinned optional Dagster installation"
)
class RealObserverQueueTest(unittest.TestCase):
    def test_graph_reload_queue_native_retry_and_late_attachment(self):
        with tempfile.TemporaryDirectory(prefix="hstream-observer-e2e-") as temporary:
            root = Path(temporary)
            registry = root / "registry"
            registry.mkdir(mode=0o700)
            write_json(
                registry / "config.json",
                {
                    "schema": 1,
                    "poll_interval_ms": 10,
                    "observer_timeout_seconds": 30,
                    "max_observations": 8,
                    "public_url": "https://workflows.example.test/dagster",
                    "max_concurrent_steps": 2,
                },
            )
            instance_home = root / "instance"
            instance_home.mkdir()
            (instance_home / "dagster.yaml").write_text(
                json.dumps(
                    {
                        "telemetry": {"enabled": False},
                        "run_coordinator": {
                            "module": "dagster._core.run_coordinator",
                            "class": "QueuedRunCoordinator",
                            "config": {"max_concurrent_runs": 2},
                        },
                    }
                )
            )
            # Dagster's first SQLite schema initialization is not safe to race
            # between the daemon and webserver. The installer likewise performs
            # instance preflight before starting either service.
            from dagster import DagsterInstance

            with DagsterInstance.from_config(str(instance_home)) as instance:
                instance.get_runs(limit=1)
            definitions = root / "definitions.py"
            definitions.write_text(
                "from hstream_workflow.dagster_observer import load_definitions\ndefs = load_definitions()\n"
            )
            workspace = root / "workspace.yaml"
            workspace.write_text(
                json.dumps(
                    {
                        "load_from": [
                            {
                                "python_file": {
                                    "relative_path": str(definitions),
                                    "location_name": "hstream_workflows",
                                    "executable_path": sys.executable,
                                }
                            }
                        ]
                    }
                )
            )
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            environment = dict(
                os.environ,
                DAGSTER_HOME=str(instance_home),
                HSTREAM_DAGSTER_REGISTRY=str(registry),
                PYTHONPATH=str(Path(__file__).parent.resolve()),
            )
            binary = Path(sys.executable).parent
            processes, logs = [], []
            try:
                for name, argv in (
                    (
                        "webserver",
                        [
                            str(binary / "dagster-webserver"),
                            "-h",
                            "127.0.0.1",
                            "-p",
                            str(port),
                            "-w",
                            str(workspace),
                        ],
                    ),
                    (
                        "daemon",
                        [str(binary / "dagster-daemon"), "run", "-w", str(workspace)],
                    ),
                ):
                    log = (root / (name + ".log")).open("wb")
                    logs.append(log)
                    processes.append(
                        subprocess.Popen(
                            argv,
                            env=environment,
                            cwd=root,
                            stdin=subprocess.DEVNULL,
                            stdout=log,
                            stderr=subprocess.STDOUT,
                            start_new_session=True,
                        )
                    )
                plan = fixture_plan()
                snapshot = graph.snapshot(plan, graph.initial_state(plan))
                for node in snapshot["nodes"]:
                    node["state"] = "succeeded"
                snapshot.update(state="succeeded", revision=1)
                first = publish_snapshot(registry, plan, snapshot)
                url = f"http://127.0.0.1:{port}"

                def wait_terminal(observation, expected):
                    deadline = time.monotonic() + 75
                    last = None
                    while time.monotonic() < deadline:
                        self.assertTrue(
                            all(process.poll() is None for process in processes),
                            "An isolated Dagster service exited",
                        )
                        try:
                            record = reconcile_observation(registry, observation, url)
                            if record["state"] in {"SUCCESS", "FAILURE", "CANCELED"}:
                                self.assertEqual(record["state"], expected)
                                return
                        except (OSError, ValueError, KeyError) as error:
                            last = error
                        time.sleep(0.25)
                    self.fail(
                        f"Observation never reached {expected}: {type(last).__name__}: {last}"
                    )

                wait_terminal(first, "SUCCESS")
                # A failure in a new native epoch is visible; another retry
                # gets a new observation and retains the first run's history.
                snapshot.update(epoch=1, revision=2, state="failed")
                snapshot["nodes"][1]["state"] = "failed"
                second = publish_snapshot(registry, plan, snapshot)
                wait_terminal(second, "FAILURE")
                snapshot.update(epoch=2, revision=3, state="succeeded")
                snapshot["nodes"][1]["state"] = "succeeded"
                third = publish_snapshot(registry, plan, snapshot)
                wait_terminal(third, "SUCCESS")
                self.assertEqual(
                    reconcile_observation(registry, first, url)["state"], "SUCCESS"
                )
                self.assertEqual(
                    reconcile_observation(registry, second, url)["state"], "FAILURE"
                )
            except BaseException:
                for log in logs:
                    log.flush()
                for path in root.glob("*.log"):
                    print(
                        path.name + "\n" + path.read_text(errors="replace")[-12000:],
                        file=sys.stderr,
                    )
                raise
            finally:
                for process in processes:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                for process in processes:
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=10)
                for log in logs:
                    log.close()


if __name__ == "__main__":
    unittest.main()
