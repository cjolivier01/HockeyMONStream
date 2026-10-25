"""Exercise the native preparation process and its durable handoff without a GPU."""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main(binary):
    binary = Path(binary).resolve()
    with tempfile.TemporaryDirectory(prefix="hstream-model-prepare-test-") as temporary:
        root = Path(temporary)
        environment = dict(os.environ, XDG_CACHE_HOME=str(root / "cache"))
        environment.pop("HSTREAM_UI_PARENT_PID", None)

        def command(name, content, executable=binary):
            directory = root / name
            directory.mkdir()
            config = directory / "input.yaml"
            config.write_text(content)
            return directory, [
                str(executable),
                "--config",
                str(config),
                "--config-directory",
                str(root),
                "--output",
                str(directory / "resolved.yaml"),
                "--receipt",
                str(directory / "receipt.json"),
            ]

        for name, text in (
            (
                "full",
                "other: preserved\npipeline: {tracker: {enable: 0}, player-analytics: {pose: {enable: 0}}}\n",
            ),
            ("bare", "tracker: {enable: 0}\n"),
            (
                "implicit-custom",
                "tracker: {enable: 1, reid-enable: 1, reid-config-file: /operator/exact.yaml}\nplayer-analytics: {pose: {enable: 1, bundle: /operator/exact-pose}}\n",
            ),
            (
                "custom",
                "tracker: {enable: 1, reid-enable: 1, reid-model: custom, reid-config-file: /operator/exact.yaml}\n",
            ),
        ):
            directory, invocation = command(name, text)
            result = subprocess.run(
                invocation,
                check=False,
                input="",
                text=True,
                env=environment,
                capture_output=True,
                timeout=10,
            )
            assert result.returncode == 0, result.stderr
            receipt = json.loads((directory / "receipt.json").read_text())
            assert receipt == {
                "version": 1,
                "status": "complete",
                "output": str(directory / "resolved.yaml"),
                "prepared": [],
            }
            assert "pipeline:" in (directory / "resolved.yaml").read_text()
            assert "HSTREAM_MODEL_PREPARATION " in result.stdout
            if name == "full":
                assert "other: preserved" in (directory / "resolved.yaml").read_text()
            if name == "custom":
                assert (
                    "/operator/exact.yaml" in (directory / "resolved.yaml").read_text()
                )
            before = (directory / "receipt.json").read_bytes()
            repeated = subprocess.run(
                invocation,
                env=environment,
                capture_output=True,
                timeout=10,
                check=False,
            )
            assert repeated.returncode != 0
            assert (directory / "receipt.json").read_bytes() == before

        for name, content in (
            ("invalid", "pipeline: []\n"),
            (
                "unknown",
                "tracker: {enable: 1}\nplayer-analytics: {pose: {enable: 1, model: nonexistent}}\n",
            ),
        ):
            directory, invocation = command(name, content)
            result = subprocess.run(
                invocation,
                env=environment,
                capture_output=True,
                timeout=10,
                check=False,
            )
            assert result.returncode != 0
            assert not (directory / "resolved.yaml").exists()
            assert not (directory / "receipt.json").exists()

        directory, invocation = command("stale-parent", "tracker: {enable: 0}\n")
        result = subprocess.run(
            invocation + ["--parent-pid", "99999999"],
            check=False,
            env=environment,
            capture_output=True,
            timeout=10,
        )
        assert result.returncode != 0 and b"parent PID" in result.stderr
        result = subprocess.run(
            invocation + ["--parent-pid", str(os.getpid())],
            check=False,
            env=environment,
            capture_output=True,
            timeout=10,
        )
        assert result.returncode == 0, result.stderr

        # This replacement builder waits during runtime discovery, before any
        # model download or GPU work. Cancellation still traverses the real
        # native cache's child supervision and callback loops.
        copies = root / "bin"
        copies.mkdir()
        local_binary = copies / binary.name
        shutil.copy2(binary, local_binary)
        builder = copies / "player-model-builder"
        builder.write_text(
            f"#!{sys.executable}\nimport os,signal,time\nfrom pathlib import Path\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\nPath(os.environ['MODEL_PREPARE_MARKER']).write_text(str(os.getpid()))\nwhile True:time.sleep(.02)\n"
        )
        builder.chmod(0o755)
        for name, cancel in (
            ("stdin-q", "q\n"),
            ("stdin-quit", "quit\n"),
            ("sigterm", signal.SIGTERM),
            ("sigint", signal.SIGINT),
        ):
            directory, invocation = command(
                name,
                "tracker: {enable: 1}\nplayer-analytics: {pose: {enable: 1, model: rtmpose-m-coco17-256x192}}\n",
                local_binary,
            )
            marker = directory / "builder.pid"
            process = subprocess.Popen(
                invocation,
                env=dict(environment, MODEL_PREPARE_MARKER=str(marker)),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                deadline = time.monotonic() + 8
                while (
                    not marker.exists()
                    and process.poll() is None
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                assert marker.exists(), process.communicate(timeout=2)
                pid = int(marker.read_text())
                if isinstance(cancel, str):
                    process.stdin.write(cancel)
                    process.stdin.flush()
                else:
                    process.send_signal(cancel)
                _, error = process.communicate(timeout=8)
                assert process.returncode != 0 and "cancelled" in error.lower(), error
                assert not Path(f"/proc/{pid}").exists(), f"Leaked builder {pid}"
                assert not (directory / "resolved.yaml").exists()
                assert not (directory / "receipt.json").exists()
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=8)
    print(
        "PASS native model handoff, exact custom config, publication refusal, parent ownership and cancellation"
    )


if __name__ == "__main__":
    main(sys.argv[1])
