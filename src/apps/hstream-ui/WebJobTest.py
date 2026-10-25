#!/usr/bin/env python3
"""Real headless companion contract checks; --gpu also exercises native encoding."""

import argparse
import copy
import json
import os
import sqlite3
import struct
import subprocess
import tempfile
import time
from pathlib import Path


def request(root, operation, payload):
    (root / "game").mkdir(exist_ok=True)
    value = {
        "schema": 1,
        "run_id": "companion-contract-123",
        "operation": operation,
        "root": str(root),
        "game_dir": "game",
        "payload": payload,
    }
    path = root / "operation-request.json"
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def run(binary, path, expected=0):
    result = subprocess.run(
        [str(binary), "--request", str(path)],
        capture_output=True,
        check=False,
        text=True,
        timeout=45,
    )
    assert result.returncode == expected, result.stderr
    return result


def plan(duration=1000, logo=""):
    card = {
        "matchup": False,
        "heading": "Native web title",
        "team_a": "",
        "team_b": "",
        "logo_a": logo,
        "logo_b": "",
        "date": "",
        "duration_ms": duration,
        "background": "#101b2c",
        "color": "#ffffff",
        "size": 0.075,
        "weight": 800,
    }
    item = {
        "is_card": True,
        "card": card,
        "annotations": [],
        "label": "Title",
        "event_mode": False,
        "event_ms": 0,
        "duration_ms": duration,
        "start_ms": 0,
        "end_ms": duration,
    }
    return {
        "schema": 2,
        "base_name": "native-web",
        "archive_path": "",
        "intervals": [item],
    }


def assert_artifacts(root, receipt):
    assert receipt["state"] == "complete" and receipt["runner_succeeded"]
    assert receipt["run_id"] == "companion-contract-123"
    for artifact in receipt["artifacts"]:
        path = (root / artifact["path"]).resolve()
        assert path.is_relative_to(root)
        info = path.stat()
        assert int(artifact["size"]) == info.st_size > 0
        assert int(artifact["inode"]) == info.st_ino
        assert int(artifact["device"]) == info.st_dev
        assert int(artifact["mtime_ns"]) == info.st_mtime_ns


def calibration_fixture(game, owner="owner"):
    """Small uncompressed RGB TIFFs: exercise native completion without GPU work."""
    game.mkdir(exist_ok=True)
    tags = [(256, 4, 1, 8), (257, 4, 1, 8), (258, 3, 3, 134),
            (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, 140),
            (277, 3, 1, 3), (278, 4, 1, 8), (279, 4, 1, 192),
            (284, 3, 1, 1)]
    tiff = b"II" + struct.pack("<HIH", 42, 8, len(tags))
    tiff += b"".join(struct.pack("<HHII", *tag) for tag in tags)
    tiff += struct.pack("<IHHH", 0, 8, 8, 8) + bytes([32, 96, 160]) * 64
    for name in ("mapping_0000.tif", "mapping_0000_x.tif", "mapping_0000_y.tif",
                 "mapping_0001.tif", "mapping_0001_x.tif", "mapping_0001_y.tif", "panorama.tif"):
        (game / name).write_bytes(tiff)
    for name in ("hm_project.pto", "autooptimiser_out.pto", "seam_file.png"):
        (game / name).write_text("fixture")
    (game / "config.yaml").write_text(
        "hstream_ui:\n  stitching_calibration:\n    status: pending\n"
        f"    invalidation_id: {owner}\n")


def calibration_contract(binary, root):
    for case in ("valid", "missing-owner", "wrong-owner", "missing-generation", "missing-preview"):
        directory = root / case
        directory.mkdir()
        calibration_fixture(directory / "game")
        payload = {"invalidation_id": "owner", "require_ice_mask": False}
        if case == "missing-owner":
            payload.pop("invalidation_id")
        elif case == "wrong-owner":
            payload["invalidation_id"] = "stale-owner"
        elif case == "missing-generation":
            (directory / "game" / "mapping_0001.tif").unlink()
        elif case == "missing-preview":
            (directory / "game" / "panorama.tif").unlink()
        path = request(directory, "calibration-complete", payload)
        run(binary, path, 0 if case == "valid" else 1)
        receipt = json.loads((directory / "operation-result.json").read_text())
        if case != "valid":
            assert receipt["state"] == "failed" and not receipt["artifacts"]
            continue
        assert_artifacts(directory, receipt)
        preview = directory / receipt["result"]["preview"]
        assert preview.read_bytes().startswith(b"\x89PNG")
        assert any(item["path"] == "game/setup-preview.png" and item["kind"] == "image"
                   for item in receipt["artifacts"])
        assert "status: complete" in (directory / "game" / "config.yaml").read_text()
        identity = preview.stat().st_mtime_ns
        run(binary, path)
        assert preview.stat().st_mtime_ns == identity


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    probe = subprocess.run(
        [str(args.binary), "--capabilities"],
        env={**os.environ, "QT_QPA_PLATFORM": "invalid-readiness-platform"},
        capture_output=True, text=True, check=True, timeout=15,
    )
    capabilities = json.loads(probe.stdout)
    assert capabilities["schema"] == capabilities["request_schema"] == capabilities["receipt_schema"] == 1
    assert "calibration-complete" in capabilities["operations"]
    assert len(capabilities["operations"]) == len(set(capabilities["operations"]))
    assert capabilities["maximum_request_bytes"] < capabilities["maximum_result_bytes"]
    with tempfile.TemporaryDirectory(prefix="hstream-web-contract-") as temporary:
        root = Path(temporary)
        calibration_contract(args.binary, root)
        missing_matches = root / "missing-matches"
        missing_matches.mkdir()
        calibration_fixture(missing_matches / "game")
        path = request(missing_matches, "matches-read", {})
        run(args.binary, path, 1)
        receipt = json.loads((missing_matches / "operation-result.json").read_text())
        assert "no editable control-point snapshot" in receipt["error"]
        assert not receipt["artifacts"]
        invalid = root / "invalid"
        invalid.mkdir()
        path = request(invalid, "highlight-preview", {"plan": {"schema": 999}})
        run(args.binary, path, 1)
        receipt = json.loads((invalid / "operation-result.json").read_text())
        assert receipt["state"] == "failed" and not receipt["runner_succeeded"]
        assert not receipt["artifacts"]
        outside = root / "outside"
        outside.mkdir()
        path = request(outside, "highlight-preview", {"plan": plan(logo="/etc/passwd")})
        run(args.binary, path, 1)
        assert (
            "relative"
            in json.loads((outside / "operation-result.json").read_text())["error"]
        )
        tracks = root / "tracks"
        tracks.mkdir()
        with sqlite3.connect(tracks / "telemetry.db") as database:
            database.executescript("""
                PRAGMA application_id=1213027156;
                PRAGMA user_version=1;
                CREATE TABLE runs(run_id TEXT, game_id TEXT, started_utc TEXT);
                INSERT INTO runs VALUES('run-one', 'game-one', '2026-01-01');
                INSERT INTO runs VALUES('run-two', 'game-two', '2026-01-02');
            """)
        path = request(tracks, "track-runs", {"database": "telemetry.db"})
        run(args.binary, path)
        receipt = json.loads((tracks / "operation-result.json").read_text())
        assert receipt["result"]["runs"] == ["run-one", "run-two"]
        assert_artifacts(tracks, receipt)
        # Valid result documents may exceed the request limit. Completed-run
        # retries must use the advertised result bound and preserve the receipt.
        receipt_path = tracks / "operation-result.json"
        receipt["result"]["details"] = "x" * (capabilities["maximum_request_bytes"] + 1)
        large_receipt = json.dumps(receipt).encode()
        assert len(large_receipt) < capabilities["maximum_result_bytes"]
        receipt_path.write_bytes(large_receipt)
        run(args.binary, path)
        assert receipt_path.read_bytes() == large_receipt
        experiments = root / "experiments"
        experiments.mkdir()
        path = request(
            experiments,
            "experiment-create",
            {
                "settings": {
                    "control_points": 100,
                    "frame_count": 1,
                    "stitch_frame_time": "00:00:00",
                }
            },
        )
        for camera in ("cam1", "cam2"):
            (experiments / "game" / camera).mkdir()
            (experiments / "game" / camera / "clip.mp4").write_bytes(
                b"native-workspace-fixture"
            )
        (experiments / "game" / "config.yaml").write_text(
            "game:\n  videos:\n    left: [cam1/clip.mp4]\n    right: [cam2/clip.mp4]\n"
        )
        run(args.binary, path)
        receipt = json.loads((experiments / "operation-result.json").read_text())
        assert_artifacts(experiments, receipt)
        assert receipt["result"]["state"] == "queued"
        assert not any(item["path"].endswith(".mp4") for item in receipt["artifacts"])
        candidate = receipt["result"]["workspace"]
        assert (experiments / candidate["game_directory"] / "config.yaml").is_file()
        linked = root / "linked"
        linked.mkdir()
        path = request(
            linked,
            "experiment-create",
            {
                "settings": {
                    "control_points": 100,
                    "frame_count": 1,
                    "stitch_frame_time": "00:00:00",
                }
            },
        )
        authorized = []
        for camera in ("cam1", "cam2"):
            (linked / "game" / camera).mkdir()
            media = root / ("upload-uuid-" + camera)
            media.write_bytes(b"native-workspace-fixture")
            (linked / "game" / camera / "clip.mp4").symlink_to(media)
            authorized.append(str(media))
        (linked / "game/config.yaml").write_bytes(
            (experiments / "game/config.yaml").read_bytes()
        )
        run(args.binary, path, 1)
        assert (
            "not an authorized media input"
            in json.loads((linked / "operation-result.json").read_text())["error"]
        )
        (linked / "operation-result.json").unlink()
        value = json.loads(path.read_text())
        value["authorized_media"] = authorized
        path.write_text(json.dumps(value))
        run(args.binary, path)
        linked_receipt = json.loads((linked / "operation-result.json").read_text())
        assert_artifacts(linked, linked_receipt)
        linked_workspace = (
            linked / linked_receipt["result"]["workspace"]["game_directory"]
        )
        assert (linked_workspace / "cam1/clip.mp4").resolve() == Path(authorized[0])
        # A fresh worker snapshot is allowed to solve; failed runner startup
        # must never turn an uncalibrated candidate into a completed result.
        (experiments / "operation-result.json").unlink()
        path = request(experiments, "experiment-run", {"workspace": candidate})
        value = json.loads(path.read_text())
        value["runtime"] = {
            "exporter": "/bin/false",
            "cli": "/bin/false",
            "root": str(experiments),
            "config": str(experiments / "game/config.yaml"),
        }
        path.write_text(json.dumps(value))
        run(args.binary, path, 1)
        failed = json.loads((experiments / "operation-result.json").read_text())
        assert failed["state"] == "failed" and not failed["artifacts"]
        assert "Native experiment process failed" in failed["error"], failed
        if not args.gpu:
            print("Headless native validation/failed-receipt tests passed")
            return
        rendered = root / "rendered"
        rendered.mkdir()
        path = request(
            rendered,
            "highlight-preview",
            {"plan": plan(), "archive_path": "", "archive_offset_ms": 0},
        )
        run(args.binary, path)
        receipt_path = rendered / "operation-result.json"
        content = receipt_path.read_bytes()
        receipt = json.loads(content)
        assert_artifacts(rendered, receipt)
        run(args.binary, path)
        assert receipt_path.read_bytes() == content
        value = json.loads(path.read_text())
        value["payload"]["archive_offset_ms"] = 1
        path.write_text(json.dumps(value))
        run(args.binary, path, 1)
        assert (
            receipt_path.read_bytes() == content
        ), "Conflicting reuse overwrote a successful receipt"
        probe = json.loads(
            subprocess.check_output(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-show_streams",
                    "-of",
                    "json",
                    str(rendered / "highlights.mp4"),
                ],
                text=True,
            )
        )
        durations = [float(stream["duration"]) for stream in probe["streams"]]
        assert len(durations) == 2 and all(
            abs(duration - 1) < 0.05 for duration in durations
        ), durations
        clipped = root / "clipped"
        clipped.mkdir()
        (clipped / "inputs").mkdir()
        os.link(rendered / "highlights.mp4", clipped / "inputs" / "archive.mp4")
        clips = copy.deepcopy(plan())
        clips["intervals"][0]["is_card"] = False
        path = request(
            clipped,
            "highlight-render",
            {
                "plan": clips,
                "archive_path": "inputs/archive.mp4",
                "archive_offset_ms": 0,
            },
        )
        run(args.binary, path)
        assert_artifacts(
            clipped, json.loads((clipped / "operation-result.json").read_text())
        )
        cancelled = root / "cancelled"
        cancelled.mkdir()
        path = request(
            cancelled, "highlight-render", {"plan": plan(120000), "archive_path": ""}
        )
        with (cancelled / "test.log").open("w") as log:
            process = subprocess.Popen(
                [str(args.binary), "--request", str(path)], stdout=log, stderr=log
            )
            try:
                deadline = time.monotonic() + 10
                while (
                    not (cancelled / "highlights.mp4").exists()
                    and process.poll() is None
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.02)
                process.terminate()
                assert process.wait(timeout=15) == 130
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
        cancelled_receipt = json.loads(
            (cancelled / "operation-result.json").read_text()
        )
        assert (
            cancelled_receipt["state"] == "cancelled"
            and not cancelled_receipt["artifacts"]
        )
    print(
        "Native GPU card+archive encoding, audio duration, receipt identity/idempotency and cancellation tests passed"
    )


if __name__ == "__main__":
    main()
