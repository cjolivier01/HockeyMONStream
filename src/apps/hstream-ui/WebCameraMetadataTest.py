#!/usr/bin/env python3
"""Actual CPU Camera inspection before Play, with and without selected media."""

import json
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def check(binary, replay, root, *, selected_media):
    environment = dict(
        os.environ,
        CUDA_VISIBLE_DEVICES="",
        HIP_VISIBLE_DEVICES="",
        ROCR_VISIBLE_DEVICES="",
    )
    capability = json.loads(
        subprocess.check_output([str(binary), "--capabilities"], env=environment)
    )
    assert "camera-optional-panorama-v1" in capability["features"]
    recording = root / "recording"
    subprocess.run(
        [str(replay), "--make-fixture", str(recording)],
        env=environment,
        check=True,
        capture_output=True,
    )
    manifest = next(recording.glob("hstream_telemetry*.json"))
    (root / "game").mkdir()
    run = "camera-metadata-cpu"
    request = {
        "schema": 1,
        "run_id": run,
        "operation": "camera-experiment",
        "root": str(root),
        "game_dir": "game",
        "payload": {
            "database": str(manifest.relative_to(root)),
            "run_id": "legacy",
            "media_kind": "panorama",
            "archive_path": "",
            "start_seconds": 0,
            "duration_seconds": 2,
            "playback_start_seconds": 0,
            "video_origin_seconds": 0,
            "source_confirmed": False,
            "loop": False,
            "realtime": True,
            "players": False,
            "play": False,
            "rink": False,
        },
    }
    if selected_media:
        panorama = root / "panorama.mp4"
        subprocess.run(
            [
                "/usr/bin/ffmpeg",
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=c=black:s=320x180:r=10:d=0.3",
                "-an",
                "-c:v",
                "libx264",
                "-threads",
                "1",
                "-pix_fmt",
                "yuv420p",
                str(panorama),
            ],
            check=True,
            capture_output=True,
            timeout=20,
        )
        request["payload"].update(
            archive_path=panorama.name, media_width=320, media_height=180
        )
    path = root / "operation-request.json"
    path.write_text(json.dumps(request))
    lines, observed, events = [], [], queue.Queue()
    # Holding the port proves preparation did not try to create its transport.
    with socket.socket() as held:
        held.bind(("127.0.0.1", 0))
        held.listen()
        process = subprocess.Popen(
            [
                str(binary),
                "--request",
                str(path),
                f"--preview-port={held.getsockname()[1]}",
            ],
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        def reader():
            for line in process.stdout:
                lines.append(line)
                if line.startswith("HSTREAM_CAMERA_EXPERIMENT "):
                    events.put(json.loads(line.partition(" ")[2]))
            events.put(None)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def wait(predicate):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                assert event is not None, "".join(lines)
                assert event["run"] == run and event["generation"] == 1
                observed.append(event)
                if predicate(event):
                    return event
            raise AssertionError("Native Camera command deadline")

        sequence = 0

        def command(action, status="ok", **fields):
            nonlocal sequence
            sequence += 1
            process.stdin.write(
                json.dumps(
                    {
                        "run": run,
                        "generation": 1,
                        "request": sequence,
                        "action": action,
                        **fields,
                    }
                )
                + "\n"
            )
            process.stdin.flush()
            event = wait(
                lambda event: event["kind"] == "ack" and event["request"] == sequence
            )
            assert event["status"] == status, event
            # Command ACKs must follow current worker state, so a browser can
            # distinguish closing from closed before authorizing another Play.
            assert observed[-2]["kind"] == "state", observed[-2:]
            return event

        try:
            wait(lambda event: event["kind"] == "catalog")
            catalog = json.loads((root / "camera-experiment-catalog.json").read_text())
            assert catalog["frame_count"] > 1 and catalog["view_count"] == 2
            assert catalog["media_available"] is selected_media
            command("view", view=1)
            command("seek", frame=catalog["frame_count"] - 1)
            state = wait(
                lambda event: event["kind"] == "state"
                and event["frame"] == catalog["frame_count"] - 1
            )
            assert state["frame"] == catalog["frame_count"] - 1 and state["view"] == 1
            assert (
                not state["open"] and not state["playing"] and state["operation"] == "0"
            )
            rejected = command("play", "rejected")
            assert "source confirmation" in rejected["error"]
            trial = {
                "name": "Metadata comparison",
                "tuning": {},
                "apply_to_fast_box": True,
                "apply_to_follower_box": True,
                "rink_aware": False,
                "anticipate": False,
                "frame_goals": False,
                "adaptive_zoom": False,
            }
            rejected = command("trial", "rejected", **trial)
            assert "confirm the video source" in rejected["error"]
            command("source-confirm", confirmed=True)
            # Confirmation alone must not turn view/seek into a video-open command.
            command("view", view=0)
            state = wait(lambda event: event["kind"] == "state" and event["view"] == 0)
            assert state["frame"] == 0
            command("seek", frame=1)
            state = wait(lambda event: event["kind"] == "state" and event["frame"] == 1)
            assert (
                not state["open"] and not state["playing"] and state["operation"] == "0"
            )
            if not selected_media:
                command("trial", **trial)
                catalog = json.loads(
                    (root / "camera-experiment-catalog.json").read_text()
                )
                assert catalog["view_count"] == 3
                rejected = command("play", "rejected")
                assert "prepare the recording again" in rejected["error"]
            command("source-confirm", confirmed=False)
            rejected = command("save", "rejected")
            assert "confirm the source" in rejected["error"]
            command("view", view=1)
            command("seek", frame=1)
            state = wait(
                lambda event: event["kind"] == "state"
                and event["frame"] == 1
                and not event["busy"]
            )
            assert (
                state["view"] == 1 and not state["open"] and state["operation"] == "0"
            )
            assert not list(root.glob("camera-trial-*.yaml"))
            command("close")
            assert process.wait(timeout=10) == 0, "".join(lines)
            thread.join(timeout=2)
            assert not any("WebRTC preview signaling" in line for line in lines)
            result = json.loads((root / "operation-result.json").read_text())
            assert result["state"] == "complete" and result["runner_succeeded"]
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    print(
        f"Camera CPU inspection (selected media={selected_media}), confirmation errors and cleanup passed"
    )


if __name__ == "__main__":
    for selected_media in (False, True):
        with tempfile.TemporaryDirectory(prefix="camera-metadata-cpu-") as directory:
            check(
                Path(sys.argv[1]).resolve(),
                Path(sys.argv[2]).resolve(),
                Path(directory),
                selected_media=selected_media,
            )
