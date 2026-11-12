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

import yaml


def check(binary, replay, root, *, selected_media, dense=False):
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
    assert "camera-save-cancel-v1" in capability["features"]
    recording = root / "recording"
    subprocess.run(
        [str(replay), "--make-fixture", str(recording)]
        + (["3840", "2160", "150", "18000"] if dense else []),
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
            "duration_seconds": 120 if dense else 2,
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
        save_sequence = 0
        admitted_save_tokens = set()

        def token():
            nonlocal save_sequence
            save_sequence += 1
            return f"{save_sequence:032x}"

        def envelope(action, **fields):
            nonlocal sequence
            sequence += 1
            if action == "save":
                admitted_save_tokens.add(fields["save_token"])
            return {
                "run": run,
                "generation": 1,
                "request": sequence,
                "action": action,
                **fields,
            }

        def send(*commands):
            # One write deliberately exercises Save and cancellation arriving
            # in the same native read, with ordinary work between them.
            process.stdin.write("".join(json.dumps(item) + "\n" for item in commands))
            process.stdin.flush()

        def cancellation(save_token, **fields):
            return {
                "run": run,
                "generation": 1,
                "action": "cancel-save",
                "save_token": save_token,
                **fields,
            }

        def acknowledgement(request, status="ok"):
            event = wait(
                lambda event: event["kind"] == "ack" and event["request"] == request
            )
            assert event["status"] == status, event
            assert observed[-2]["kind"] == "state", observed[-2:]
            return event

        def command(action, status="ok", **fields):
            if action == "save":
                fields.setdefault("save_token", token())
                fields.setdefault("view", 2)
            sent = envelope(action, **fields)
            send(sent)
            # Command ACKs must follow current worker state, so a browser can
            # distinguish closing from closed before authorizing another Play.
            return acknowledgement(sent["request"], status)

        try:
            wait(lambda event: event["kind"] == "catalog")
            catalog = json.loads((root / "camera-experiment-catalog.json").read_text())
            assert catalog["frame_count"] > 1 and catalog["view_count"] == 2
            if dense:
                assert catalog["frame_count"] == 18000, catalog["frame_count"]
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
                mismatched = token()
                rejected = command("save", "rejected", save_token=mismatched, view=1)
                assert "changed before Save" in rejected["error"]
                rebound = command("save", "rejected", save_token=mismatched, view=2)
                assert "cannot select another trial" in rebound["error"]
                assert (
                    command("save", "rejected", save_token=mismatched, view=1)["error"]
                    == rejected["error"]
                )
                assert not list(root.glob("camera-trial-*.yaml"))

                cancelled_token = token()
                save = envelope("save", save_token=cancelled_token, view=2)
                later_view = envelope("view", view=1)
                send(save, later_view, cancellation(cancelled_token))
                cancel_ack = wait(lambda event: event["kind"] == "save-cancel")
                assert (
                    cancel_ack["status"] == "ok"
                    and cancel_ack["save_token"] == cancelled_token
                )
                assert "request" not in cancel_ack and "cancelled" not in cancel_ack
                cancelled = acknowledgement(save["request"])
                assert (
                    cancelled["save_token"] == cancelled_token
                    and cancelled["cancelled"] is True
                )
                assert "saved" not in cancelled and observed[-2]["view"] == 2
                acknowledgement(later_view["request"])
                assert observed[-2]["view"] == 1  # Mutation waited for Save retirement.
                assert not list(root.glob("camera-trial-*.yaml"))
                assert not list(root.glob("camera-trial-*.tmp-*"))
                command("view", view=2)
                assert command("save", save_token=cancelled_token)["cancelled"] is True
                for fields in ({}, {"request": 9007199254740991}, {"generation": 0}):
                    send(cancellation(cancelled_token, **fields))
                    ack = wait(lambda event: event["kind"] == "save-cancel")
                    assert ack["status"] == ("rejected" if fields else "ok"), ack
                send(cancellation("f" * 32))
                assert (
                    wait(lambda event: event["kind"] == "save-cancel")["status"]
                    == "rejected"
                )

                if dense:
                    # Queue the barrier and Save together. Waiting for the
                    # barrier ACK excludes old state, and native cannot poll
                    # its idle loop between these already queued commands.
                    barrier = envelope("source-confirm", confirmed=True)
                    during_token = token()
                    during = envelope("save", save_token=during_token, view=2)
                    send(barrier, during)
                    acknowledgement(barrier["request"])
                    before = len(observed)
                    wait(lambda event: event["kind"] == "state")
                    assert not any(
                        event["kind"] == "ack" and event["request"] == during["request"]
                        for event in observed[before:]
                    ), "Dense serializer finished before cancellation could be exercised"
                    send(cancellation(during_token))
                    assert (
                        wait(lambda event: event["kind"] == "save-cancel")["status"]
                        == "ok"
                    )
                    assert acknowledgement(during["request"])["cancelled"] is True
                    assert not list(root.glob("camera-trial-*.yaml"))
                    assert not list(root.glob("camera-trial-*.tmp-*"))

                committed_token = token()
                saved = command("save", save_token=committed_token)
                descriptor = yaml.safe_load((root / saved["saved"]).read_text())
                assert descriptor["media"]["kind"] == "none"
                assert (
                    "path" not in descriptor["media"]
                    and "identity" not in descriptor["media"]
                )
                assert len(descriptor["frames"]) == catalog["frame_count"]
                assert saved["artifact"]["path"] == saved["saved"]
                send(cancellation(committed_token))
                assert (
                    wait(lambda event: event["kind"] == "save-cancel")["status"] == "ok"
                )
                assert (
                    command("save", save_token=committed_token)["artifact"]
                    == saved["artifact"]
                )
                command("view", view=1)
                # Lost-result recovery cannot select the newer view or write again.
                assert (
                    command("save", save_token=committed_token)["artifact"]
                    == saved["artifact"]
                )
                assert observed[-2]["view"] == 1
                command("view", view=2)
                assert command("save")["artifact"] == saved["artifact"]
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
            assert len(list(root.glob("camera-trial-*.yaml"))) == (
                0 if selected_media else 1
            )
            if not selected_media and not dense:
                while len(admitted_save_tokens) < 256:
                    assert "confirm the source" in command("save", "rejected")["error"]
                assert "operation limit" in command("save", "rejected")["error"]
                # Reaching the bound cannot evict old outcomes and permit reuse.
                assert (
                    command("save", save_token=committed_token)["artifact"]
                    == saved["artifact"]
                )
                assert command("save", save_token=cancelled_token)["cancelled"] is True
            if dense:
                command("source-confirm", confirmed=True)
                command("trial", **{**trial, "name": "Owner disconnect comparison"})
                barrier = envelope("source-confirm", confirmed=True)
                close_save = envelope("save", save_token=token(), view=3)
                send(barrier, close_save)
                acknowledgement(barrier["request"])
                before = len(observed)
                wait(lambda event: event["kind"] == "state")
                assert not any(
                    event["kind"] == "ack" and event["request"] == close_save["request"]
                    for event in observed[before:]
                ), "Dense serializer finished before owner disconnect"
                process.stdin.close()
                assert acknowledgement(close_save["request"])["cancelled"] is True
                assert process.wait(timeout=10) != 0, "".join(lines)
                assert "command owner disconnected" in "".join(lines)
                assert not (root / "camera-trial-2.yaml").exists()
                assert not list(root.glob("camera-trial-*.tmp-*"))
            else:
                command("close")
                assert process.wait(timeout=10) == 0, "".join(lines)
            thread.join(timeout=2)
            assert not any("WebRTC preview signaling" in line for line in lines)
            result = json.loads((root / "operation-result.json").read_text())
            if dense:
                assert result["state"] == "failed" and not result["runner_succeeded"]
            else:
                assert result["state"] == "complete" and result["runner_succeeded"]
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    print(
        f"Camera CPU inspection (selected media={selected_media}, dense={dense}), Save/cancel fencing and cleanup passed"
    )


if __name__ == "__main__":
    for selected_media, dense in ((False, False), (True, False), (False, True)):
        with tempfile.TemporaryDirectory(prefix="camera-metadata-cpu-") as directory:
            check(
                Path(sys.argv[1]).resolve(),
                Path(sys.argv[2]).resolve(),
                Path(directory),
                selected_media=selected_media,
                dense=dense,
            )
