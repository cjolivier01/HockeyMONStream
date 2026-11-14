"""Exercise the installed native protocol using real Linux descendants."""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
from pathlib import Path

SCHEMA = "hstream-runtime-worker-v1"
WORKER = Path(
    os.environ.get(
        "HSTREAM_RUNTIME_WORKER",
        "src/apps/hstream-runtime-worker/hstream-runtime-worker",
    )
).resolve()


class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temporary.name)
        self.root = self.workspace / "worker"
        self.root.mkdir(mode=0o700)
        self.python = str(Path(shutil.which("python3")).resolve())
        self.request = {
            "schema": SCHEMA,
            "id": "test",
            "token": uuid.uuid4().hex,
            "workspace": str(self.workspace),
            "files": {
                self.python: hashlib.sha256(Path(self.python).read_bytes()).hexdigest()
            },
            "limits": {
                "poll_ms": 10,
                "retire_ms": 150,
                "timeout_ms": 20000,
                "client_timeout_ms": 0,
                "max_bytes": 0,
                "max_log_bytes": 1024 * 1024,
                "max_tasks": 8,
            },
        }
        self.sequence = 0
        self.process = None

    def tearDown(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
            self.process.communicate(timeout=5)
        self.temporary.cleanup()

    def write(self, path, value):
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(value))
        temporary.chmod(0o600)
        temporary.replace(path)

    def until(self, predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.01)
        self.fail("Native runtime operation timed out")

    def start(self, *, environment=None):
        self.write(self.root / "request.json", self.request)
        self.process = subprocess.Popen(
            [str(WORKER), "--request", str(self.root / "request.json")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self.until(
            lambda: (self.root / "state.json").exists()
            or self.process.poll() is not None
        )
        if not (self.root / "state.json").exists():
            self.fail(self.process.communicate()[1].decode())

    def call(self, operation, **payload):
        self.sequence += 1
        name = f"{self.sequence:020d}.json"
        self.write(
            self.root / "commands" / name,
            {
                "schema": SCHEMA,
                "token": self.request["token"],
                "sequence": self.sequence,
                "operation": operation,
                **payload,
            },
        )
        path = self.root / "replies" / name
        self.until(path.exists)
        value = json.loads(path.read_text())
        self.assertEqual(value["sequence"], self.sequence)
        self.assertEqual(value["token"], self.request["token"])
        path.unlink()
        return value

    def launch(self, program, timeout=10000):
        task = uuid.uuid4().hex
        response = self.call(
            "launch",
            spec={
                "id": task,
                "argv": [self.python, "-c", program],
                "cwd": str(self.workspace),
                "environment": {},
                "timeout_ms": timeout,
                "stdout": str(self.workspace / (task + ".log")),
                "stderr": str(self.workspace / (task + ".log")),
            },
        )
        self.assertTrue(response["ok"], response)
        return task

    def completed(self, task):
        def poll():
            response = self.call("poll", task=task)
            self.assertTrue(response["ok"], response)
            return response["result"] if response["result"]["stopped"] else None

        return self.until(poll)

    def finish(self):
        self.assertTrue(self.call("finish")["ok"])
        self.assertEqual(self.process.wait(timeout=5), 0)
        result = json.loads((self.root / "result.json").read_text())
        self.assertTrue(result["stopped"])

    def test_independent_stdin_and_preview_retirement(self):
        self.start()
        main = self.launch("import sys; print(sys.stdin.readline(), end='')")
        preview = self.launch("import time; time.sleep(100)")
        retired = self.call("retire-task", task=preview)
        self.assertTrue(retired["result"]["stopped"])
        self.assertIsNone(self.call("poll", task=main)["result"]["returncode"])
        data = base64.b64encode(b"seek 42\n").decode()
        self.assertEqual(
            self.call("stdin", task=main, data=data)["result"]["accepted"], 8
        )
        self.assertEqual(self.completed(main)["returncode"], 0)
        self.assertEqual((self.workspace / (main + ".log")).read_text(), "seek 42\n")
        self.finish()

    def test_detached_resistant_descendant_is_reaped(self):
        self.start()
        task = self.launch("""import os, signal, time
pid = os.fork()
if pid == 0:
    os.setsid()
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    open('descendant', 'w').write(str(os.getpid()))
    while True: time.sleep(1)
while not os.path.exists('descendant'): time.sleep(.01)
""")
        self.until(lambda: (self.workspace / "descendant").exists())
        descendant = int((self.workspace / "descendant").read_text())
        self.assertEqual(self.completed(task)["returncode"], 0)
        self.assertFalse(Path(f"/proc/{descendant}").exists())
        self.finish()

    def test_client_loss_retires_compute(self):
        self.request["limits"]["client_timeout_ms"] = 250
        self.start()
        self.launch("import time; time.sleep(100)")
        self.assertEqual(self.process.wait(timeout=5), 1)
        result = json.loads((self.root / "result.json").read_text())
        self.assertTrue(result["stopped"])
        self.assertIn("lease expired", result["error"])

    def test_atomic_heartbeat_replacement_preserves_the_session(self):
        self.request["limits"]["client_timeout_ms"] = 1000
        done = threading.Event()

        def heartbeat():
            sequence = 0
            while not done.is_set():
                sequence += 1
                self.write(
                    self.root / "client.json",
                    {"token": self.request["token"], "sequence": sequence},
                )
                done.wait(0.0005)

        thread = threading.Thread(target=heartbeat)
        thread.start()
        try:
            self.start()
            task = self.launch("import time; time.sleep(1.5)")
            self.assertEqual(self.completed(task)["returncode"], 0)
            self.finish()
        finally:
            done.set()
            thread.join(timeout=2)

    def test_task_deadline_is_distinct_from_exit_success(self):
        self.start()
        task = self.launch("import time; time.sleep(100)", timeout=100)
        state = self.completed(task)
        self.assertTrue(state["timed_out"])
        self.assertNotEqual(state["returncode"], 0)
        self.finish()

    def test_compute_identity_and_hash_are_checked_together(self):
        self.start()
        path = self.workspace / "artifact"
        path.write_bytes(b"original")
        info = path.stat()
        value = self.call(
            "inspect",
            path=str(path),
            maximum=8,
            expected={"device": info.st_dev, "inode": info.st_ino},
        )
        self.assertTrue(value["ok"])
        self.assertEqual(
            value["result"]["sha256"], hashlib.sha256(b"original").hexdigest()
        )
        path.write_bytes(b"modified")
        self.assertFalse(
            self.call(
                "inspect",
                path=str(path),
                maximum=8,
                expected={"sha256": value["result"]["sha256"]},
            )["ok"]
        )
        self.finish()

    def test_symlink_escape_is_rejected(self):
        self.start()
        (self.workspace / "escaped").symlink_to("/etc/passwd")
        self.assertFalse(
            self.call("inspect", path=str(self.workspace / "escaped"), maximum=10000)[
                "ok"
            ]
        )
        self.finish()

    def test_fifo_reads_and_logs_cannot_block_supervision(self):
        fifo = self.workspace / "fifo"
        os.mkfifo(fifo)
        self.start()
        response = self.call("inspect", path=str(fifo), maximum=1024)
        self.assertFalse(response["ok"], response)
        response = self.call(
            "launch",
            spec={
                "id": uuid.uuid4().hex,
                "argv": [self.python, "-c", "print('must not run')"],
                "cwd": str(self.workspace),
                "environment": {},
                "timeout_ms": 1000,
                "stdout": str(fifo),
                "stderr": str(self.workspace / "err"),
            },
        )
        self.assertFalse(response["ok"], response)
        self.finish()
        result = subprocess.run(
            [str(WORKER), "--request", str(fifo)],
            capture_output=True,
            timeout=2,
            check=False,
        )
        self.assertEqual(result.returncode, 125)

    def test_missing_retirement_receipt_never_establishes_success(self):
        self.start()
        task = self.launch("import time; time.sleep(100)")
        (self.root / "tasks" / task / "retirement").unlink()
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 1)
        result = json.loads((self.root / "result.json").read_text())
        self.assertFalse(result["ok"])

    def test_log_limit_cancels_producer(self):
        self.request["limits"]["max_log_bytes"] = 1024
        self.start()
        self.launch("import time; print('x'*4096, flush=True); time.sleep(100)")
        self.assertEqual(self.process.wait(timeout=5), 1)
        result = json.loads((self.root / "result.json").read_text())
        self.assertTrue(result["stopped"])
        self.assertIn("log allowance", result["error"])

    def test_session_cannot_replay(self):
        self.start()
        self.finish()
        replay = subprocess.run(
            [str(WORKER), "--request", str(self.root / "request.json")],
            check=False,
            capture_output=True,
        )
        self.assertNotEqual(replay.returncode, 0)
        self.assertIn(b"cannot be replayed", replay.stderr)

    def test_stale_command_token_retires_the_session(self):
        self.start()
        task = self.launch("import time; time.sleep(100)")
        self.write(
            self.root / "commands" / "00000000000000000002.json",
            {
                "schema": SCHEMA,
                "token": "c" * 32,
                "sequence": 2,
                "operation": "poll",
                "task": task,
            },
        )
        self.assertEqual(self.process.wait(timeout=5), 1)
        result = json.loads((self.root / "result.json").read_text())
        self.assertTrue(result["stopped"])
        self.assertIn("binding changed", result["error"])

    def test_changed_executable_is_rejected_before_launch(self):
        executable = self.workspace / "executable"
        shutil.copyfile("/bin/true", executable)
        executable.chmod(0o700)
        self.request["files"][str(executable)] = hashlib.sha256(
            executable.read_bytes()
        ).hexdigest()
        self.start()
        self.until(
            lambda: json.loads((self.root / "state.json").read_text())["status"]
            == "ready"
        )
        shutil.copyfile("/bin/false", executable)
        response = self.call(
            "launch",
            spec={
                "id": uuid.uuid4().hex,
                "argv": [str(executable)],
                "cwd": str(self.workspace),
                "environment": {},
                "timeout_ms": 1000,
                "stdout": str(self.workspace / "out"),
                "stderr": str(self.workspace / "err"),
            },
        )
        self.assertFalse(response["ok"])
        self.assertIn("executable changed", response["error"])
        self.finish()

    def test_invalid_protocol_is_rejected(self):
        for document in ("", "null", '{"schema":"old"}'):
            with self.subTest(document=document):
                path = self.root / "bad.json"
                path.write_text(document)
                result = subprocess.run(
                    [str(WORKER), "--request", str(path)],
                    check=False,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 125, result.stderr.decode())

    def test_read_grants_are_exact_and_content_bound(self):
        with tempfile.TemporaryDirectory() as outside:
            granted = Path(outside) / "baseline"
            granted.write_bytes(b"native defaults")
            other = Path(outside) / "ungranted"
            other.write_bytes(b"unrelated")
            self.request["read_files"] = [str(granted)]
            self.start()
            response = self.call("read", path=str(granted), maximum=1024)
            self.assertTrue(response["ok"], response)
            value = response["result"]
            self.assertEqual(base64.b64decode(value["data"]), b"native defaults")
            self.assertEqual(
                value["sha256"], hashlib.sha256(b"native defaults").hexdigest()
            )
            self.assertFalse(self.call("read", path=str(other), maximum=1024)["ok"])
            self.finish()

    def test_controller_loss_interrupts_long_file_inspection(self):
        self.request["limits"]["client_timeout_ms"] = 100
        path = self.workspace / "sparse-artifact"
        with path.open("wb") as output:
            output.truncate(1024**3)
        self.start()
        response = self.call("inspect", path=str(path), maximum=1024**3)
        self.assertFalse(response["ok"])
        self.assertIn("lease expired", response["error"])
        self.assertEqual(self.process.wait(timeout=5), 1)
        self.assertTrue(json.loads((self.root / "result.json").read_text())["stopped"])

    def test_inspection_keeps_enforcing_task_deadlines(self):
        path = self.workspace / "sparse-artifact"
        with path.open("wb") as output:
            output.truncate(512 * 1024**2)
        self.start()
        task = self.launch(
            "import time; time.sleep(.5); print('too late')", timeout=150
        )
        self.assertTrue(self.call("inspect", path=str(path), maximum=1024**3)["ok"])
        state = self.completed(task)
        self.assertTrue(state["timed_out"], state)
        self.assertNotEqual(state["returncode"], 0)
        self.assertNotIn("too late", (self.workspace / (task + ".log")).read_text())
        self.finish()

    def test_http_keeps_enforcing_other_task_deadlines(self):
        self.start()
        server = self.launch(
            """from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import time
class Slow(BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(1)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'ok')
http = HTTPServer(('127.0.0.1', 0), Slow)
Path('port').write_text(str(http.server_port))
http.handle_request()
http.server_close()
"""
        )
        self.until(lambda: (self.workspace / "port").exists())
        port = int((self.workspace / "port").read_text())
        task = self.launch(
            "import time; time.sleep(.5); print('too late')", timeout=150
        )
        response = self.call("http", port=port, path="/", method="GET", timeout_ms=3000)
        self.assertTrue(response["ok"], response)
        state = self.completed(task)
        self.assertTrue(state["timed_out"], state)
        self.assertNotEqual(state["returncode"], 0)
        self.assertEqual(self.completed(server)["returncode"], 0)
        self.finish()

    def test_accounting_tolerates_concurrent_directory_cleanup(self):
        self.request["limits"]["max_bytes"] = 1024**3
        self.start()
        task = self.launch("""from pathlib import Path
import time
root = Path('transient')
root.mkdir()
deadline = time.monotonic() + 2.5
while time.monotonic() < deadline:
    for i in range(250): (root / str(i)).mkdir()
    for i in range(250): (root / str(i)).rmdir()
root.rmdir()
""")
        self.assertEqual(self.completed(task)["returncode"], 0)
        self.finish()

    def test_compute_tool_alias_executes_pinned_shebang_and_rejects_replacement(self):
        binaries = self.workspace / "compute-bin"
        binaries.mkdir()
        script = binaries / "test-tool"
        script.write_text("#!/bin/sh\nprintf 'compute utility\\n'\n")
        script.chmod(0o700)
        self.request["tools"] = ["test-tool"]
        self.start(environment={**os.environ, "PATH": str(binaries)})
        self.until(
            lambda: json.loads((self.root / "state.json").read_text())["status"]
            == "ready"
        )

        def launch():
            return self.call(
                "launch",
                spec={
                    "id": uuid.uuid4().hex,
                    "argv": ["test-tool"],
                    "cwd": str(self.workspace),
                    "environment": {},
                    "timeout_ms": 1000,
                    "stdout": str(self.workspace / "out"),
                    "stderr": str(self.workspace / "err"),
                },
            )

        response = launch()
        self.assertTrue(response["ok"], response)
        self.assertEqual(self.completed(response["result"]["id"])["returncode"], 0)
        self.assertEqual((self.workspace / "out").read_text(), "compute utility\n")
        script.write_text("#!/bin/sh\nprintf 'changed\\n'\n")
        response = launch()
        self.assertFalse(response["ok"], response)
        self.assertIn("executable changed", response["error"])
        self.finish()

    def test_compute_file_resolution_pins_exact_bytes_and_aliases(self):
        alias = self.workspace / "python-alias"
        alias.symlink_to(self.python)
        self.request["files"] = {}
        self.request["resolve_files"] = [str(alias)]
        self.start()
        self.until(
            lambda: json.loads((self.root / "state.json").read_text())["status"]
            == "ready"
        )
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["tools"][str(alias)], self.python)
        self.assertEqual(
            state["files"][self.python],
            hashlib.sha256(Path(self.python).read_bytes()).hexdigest(),
        )
        self.python = str(alias)
        task = self.launch("print('compute binary')")
        self.assertEqual(self.completed(task)["returncode"], 0)
        self.finish()

    def test_compute_resolution_does_not_override_qualified_hashes(self):
        self.request["files"][self.python] = "0" * 64
        self.request["resolve_files"] = [self.python]
        self.start()
        self.assertEqual(self.process.wait(timeout=5), 1)
        state = json.loads((self.root / "state.json").read_text())
        self.assertIn("Pinned runtime file changed", state["error"])
        self.assertTrue(state["stopped"])

    def test_compute_resolution_rejects_relative_paths(self):
        self.request["resolve_files"] = ["relative/python"]
        self.start()
        self.assertEqual(self.process.wait(timeout=5), 1)
        state = json.loads((self.root / "state.json").read_text())
        self.assertIn("must be absolute", state["error"])

    def test_qualified_runtime_alias_resolves_on_compute(self):
        alias = self.workspace / "qualified-python"
        alias.symlink_to(self.python)
        digest = self.request["files"].pop(self.python)
        self.request["files"][str(alias)] = digest
        self.python = str(alias)
        self.start()
        task = self.launch("print('qualified compute alias')")
        self.assertEqual(self.completed(task)["returncode"], 0)
        inspected = self.call(
            "inspect", path=str(alias), maximum=1024**3, expected={"sha256": digest}
        )
        self.assertTrue(inspected["ok"], inspected)
        self.finish()


if __name__ == "__main__":
    unittest.main()
