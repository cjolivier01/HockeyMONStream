"""Opt-in native TensorRT process validation using an already prepared ReID engine."""

from __future__ import annotations

import array
import ctypes
import json
import math
import os
import queue
import select
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

INPUT_FLOATS = 3 * 256 * 128
OUTPUT_FLOATS = 256


def main(binary: str, engine: str) -> None:
    invocation = [str(Path(binary).resolve()), "--engine", str(Path(engine).resolve())]
    environment = dict(os.environ)
    environment.pop("HSTREAM_UI_PARENT_PID", None)
    description = subprocess.run(
        [*invocation, "--describe"],
        env=environment,
        capture_output=True,
        check=True,
        timeout=20,
    )
    metadata = json.loads(description.stdout)
    assert metadata["input_shape"] == [-1, 3, 256, 128]
    assert metadata["output_shape"] == [-1, 256]
    assert metadata["max_request_batch"] == 32

    payload = bytearray()
    for count in (1, 3, 32):
        payload.extend(struct.pack("<I", count))
        payload.extend((array.array("f", [0.25]) * (count * INPUT_FLOATS)).tobytes())
    payload.extend(struct.pack("<I", 0))
    started = time.monotonic()
    result = subprocess.run(
        invocation,
        input=payload,
        env=environment,
        capture_output=True,
        check=True,
        timeout=30,
    )
    elapsed = time.monotonic() - started
    cursor, reference = 0, None
    for count in (1, 3, 32):
        assert struct.unpack_from("<I", result.stdout, cursor) == (count,)
        cursor += 4
        data = array.array("f")
        data.frombytes(result.stdout[cursor : cursor + count * OUTPUT_FLOATS * 4])
        cursor += count * OUTPUT_FLOATS * 4
        assert len(data) == count * OUTPUT_FLOATS
        assert all(math.isfinite(value) for value in data)
        assert max(abs(value) for value in data) > 0
        if reference is None:
            reference = data[:OUTPUT_FLOATS]
        for sample in range(count):
            assert (
                max(
                    abs(a - b)
                    for a, b in zip(
                        reference,
                        data[sample * OUTPUT_FLOATS : (sample + 1) * OUTPUT_FLOATS],
                    )
                )
                < 1e-3
            )
    assert cursor == len(result.stdout)

    nonfinite = array.array("f", [0]) * INPUT_FLOATS
    nonfinite[0] = float("nan")
    invalid = {
        "oversized": struct.pack("<I", 33),
        "truncated-header": b"\1\0",
        "truncated-payload": struct.pack("<I", 1) + b"\0" * 8,
        "nonfinite": struct.pack("<I", 1) + nonfinite.tobytes(),
    }
    for name, data in invalid.items():
        failed = subprocess.run(
            invocation,
            input=data,
            env=environment,
            capture_output=True,
            check=False,
            timeout=20,
        )
        assert failed.returncode != 0 and b"hstream-model-infer:" in failed.stderr, (
            name,
            failed.stderr,
        )
        assert failed.stdout == b""
    empty = subprocess.run(
        invocation,
        input=b"",
        env=environment,
        capture_output=True,
        check=True,
        timeout=20,
    )
    assert empty.stdout == b""
    stale = subprocess.run(
        [*invocation, "--parent-pid", "99999999"],
        env=environment,
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert stale.returncode != 0 and b"Stale" in stale.stderr

    # Aspen creates inference sessions on worker threads, then finalizes them
    # after those threads exit. The session belongs to the entire process.
    children = queue.Queue()
    release_creator = threading.Event()

    def spawn_from_worker():
        child = subprocess.Popen(
            [*invocation, "--parent-pid", str(os.getpid())],
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        children.put(child)
        release_creator.wait(timeout=30)

    request = struct.pack("<I", 1) + (array.array("f", [0.25]) * INPUT_FLOATS).tobytes()
    with ThreadPoolExecutor(max_workers=1) as executor:
        future = executor.submit(spawn_from_worker)
        threaded = children.get(timeout=10)
        try:
            # Complete a request while the creator is still alive, proving the
            # helper has installed its lifetime monitor before that thread exits.
            os.set_blocking(threaded.stdin.fileno(), False)
            os.set_blocking(threaded.stdout.fileno(), False)
            sent, initial = 0, bytearray()
            deadline = time.monotonic() + 20
            while len(initial) < 4 + OUTPUT_FLOATS * 4:
                assert time.monotonic() < deadline, "Inference startup timed out"
                ready, writable, _ = select.select(
                    [threaded.stdout],
                    [threaded.stdin] if sent < len(request) else [],
                    [],
                    0.05,
                )
                if writable:
                    sent += os.write(threaded.stdin.fileno(), request[sent:])
                if ready:
                    chunk = os.read(threaded.stdout.fileno(), 4 + OUTPUT_FLOATS * 4)
                    assert chunk, "Inference helper exited before creator thread"
                    initial.extend(chunk)
            assert struct.unpack_from("<I", initial) == (1,)
            os.set_blocking(threaded.stdin.fileno(), True)
            os.set_blocking(threaded.stdout.fileno(), True)
        except BaseException:
            threaded.kill()
            threaded.communicate()
            raise
        finally:
            release_creator.set()
        future.result(timeout=5)
    # The executor's creating thread has now exited. Inference must still work.
    with threaded:
        try:
            output, errors = threaded.communicate(
                request + struct.pack("<I", 0),
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            threaded.kill()
            threaded.communicate()
            raise
    assert threaded.returncode == 0, errors
    assert struct.unpack_from("<I", output) == (1,)
    assert len(output) == 4 + OUTPUT_FLOATS * 4
    features = array.array("f")
    features.frombytes(output[4:])
    assert all(math.isfinite(value) for value in features)
    assert max(abs(a - b) for a, b in zip(features, reference)) < 1e-3

    # Reap the helper ourselves after its real direct parent is killed.
    assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
    with tempfile.TemporaryDirectory(prefix="hstream-infer-parent-") as temporary:
        marker = Path(temporary) / "child.pid"
        launcher = "import os,subprocess,sys,time;from pathlib import Path;child=subprocess.Popen([*sys.argv[2:],'--parent-pid',str(os.getpid())],stdin=sys.stdin,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);Path(sys.argv[1]).write_text(str(child.pid));time.sleep(60)"
        read_fd, write_fd = os.pipe()
        parent = subprocess.Popen(
            [sys.executable, "-c", launcher, str(marker), *invocation],
            env=environment,
            stdin=read_fd,
        )
        os.close(read_fd)
        helper = None
        try:
            deadline = time.monotonic() + 8
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            helper = int(marker.read_text())
            time.sleep(1)
            parent.kill()
            parent.wait(timeout=3)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                pid, status = os.waitpid(helper, os.WNOHANG)
                if pid:
                    assert (
                        os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGTERM
                    )
                    helper = None
                    break
                time.sleep(0.01)
            assert helper is None, "Native inference helper outlived its direct parent"
        finally:
            os.close(write_fd)
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=3)
            if helper is not None:
                os.kill(helper, signal.SIGKILL)
                os.waitpid(helper, 0)
    print(
        json.dumps(
            {
                "status": "passed",
                "description": metadata,
                "batches": [1, 3, 32],
                "persistent_seconds": elapsed,
                "invalid_inputs": list(invalid),
                "parent_death": "reaped",
                "creator_thread_exit": "inferred_and_closed",
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
