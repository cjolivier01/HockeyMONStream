#!/usr/bin/env python3
"""Opt-in real systemd lifecycle check using one disposable CPU workflow.

HSTREAM_SYSTEMD_TESTS=1 PYTHONPATH=scripts python scripts/hstream_workflow_systemd_e2e.py
Requires passwordless sudo for transient systemd units; never changes installed services.
"""

import os
import pwd
import subprocess
import sys
import time
import unittest
from pathlib import Path
from uuid import uuid4

import hstream_workflow_controller_test as fixtures
from hstream_workflow.files import read_json
from hstream_workflow.processes import same_process
from hstream_workflow.schema import WorkflowError
from install_workflow_services import render_unit


@unittest.skipUnless(
    os.environ.get("HSTREAM_SYSTEMD_TESTS") == "1",
    "explicit systemd qualification required",
)
class SystemdLifecycleTest(unittest.TestCase):
    def test_restart_stop_and_main_crash_preserve_attempt_supervisor(self):
        fixture = fixtures.ControllerTest()
        fixture.setUp()
        unit = "hstream-workflow-test-" + uuid4().hex
        scripts = Path(__file__).resolve().parent
        user = pwd.getpwuid(os.getuid()).pw_name
        rendered = render_unit(
            "Disposable workflow lifecycle test",
            {"user": user, "gid": os.getgid()},
            str(scripts),
            [sys.executable],
            independent_attempts=True,
        ).decode()
        kill_mode = next(
            line for line in rendered.splitlines() if line.startswith("KillMode=")
        )

        def command(*args):
            return subprocess.check_output(
                ["sudo", "-n", *args], text=True, stderr=subprocess.STDOUT, timeout=20
            ).strip()

        def wait_for(predicate, timeout=25):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                value = predicate()
                if value:
                    return value
                time.sleep(0.1)
            self.fail("Timed out waiting for systemd workflow lifecycle")

        started = False
        try:
            fixture.plan["stages"] = [fixture.stage("a", delay=18)]
            fixture.controller.register(fixture.plan, fixture.game)
            command(
                "systemd-run",
                "--unit=" + unit,
                "--property=" + kill_mode,
                "--property=User=" + user,
                "--property=Group=" + str(os.getgid()),
                "--property=Restart=on-failure",
                "--property=RestartSec=1",
                "--property=TimeoutStopSec=5",
                "--setenv=PYTHONPATH=" + str(scripts),
                sys.executable,
                "-m",
                "hstream_workflow.cli",
                "serve",
                "--registry",
                str(fixture.registry),
            )
            started = True
            directory, _ = fixture.controller.registry.lookup(
                fixture.plan["workflow_id"]
            )

            def supervisor_record():
                records = list(directory.glob("attempts/*/supervisor.json"))
                return read_json(records[0]) if records else None

            supervisor = wait_for(supervisor_record)
            cgroup = Path(f"/proc/{supervisor['pid']}/cgroup").read_text()
            self.assertIn(unit, cgroup)
            for action in ("restart", "stop-start", "crash"):
                previous = command(
                    "systemctl", "show", unit, "--property=MainPID", "--value"
                )
                if action == "restart":
                    command("systemctl", "restart", unit)
                elif action == "stop-start":
                    command("systemctl", "stop", unit)
                    self.assertTrue(same_process(supervisor))
                    command("systemctl", "start", unit)
                else:
                    command(
                        "systemctl",
                        "kill",
                        "--kill-whom=main",
                        "--signal=SIGKILL",
                        unit,
                    )
                wait_for(
                    lambda: command(
                        "systemctl", "show", unit, "--property=MainPID", "--value"
                    )
                    not in {"0", previous}
                )
                self.assertTrue(same_process(supervisor), action)
                self.assertEqual(
                    Path(f"/proc/{supervisor['pid']}/cgroup").read_text(), cgroup
                )

            def completed():
                try:
                    state = fixture.controller.status(fixture.plan["workflow_id"])
                except WorkflowError as exc:
                    # The strict reader can observe atomic state replacement by
                    # the live dispatcher. Retry that read as the monitor does.
                    if "replaced while reading" in str(exc.__cause__):
                        return None
                    raise
                return (
                    state
                    if state["state"] == "succeeded" and state["owner_released"]
                    else None
                )

            result = wait_for(completed)
            self.assertEqual(len(result["nodes"][0]["attempts"]), 1)
        finally:
            if started:
                command("systemctl", "stop", unit)
            fixture.tearDown()


if __name__ == "__main__":
    unittest.main()
