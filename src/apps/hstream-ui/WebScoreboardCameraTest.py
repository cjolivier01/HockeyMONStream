#!/usr/bin/env python3
"""Real native still publication with pinned original media and bounded pixels."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

binary = Path(sys.argv[1]).resolve()
baseline = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    source = root / "camera.mp4"
    subprocess.run(
        [
            os.environ.get("HSTREAM_TEST_FFMPEG", "/usr/bin/ffmpeg"),
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=384x216:r=25:d=1",
            "-c:v",
            "libx264",
            "-bf",
            "2",
            str(source),
        ],
        check=True,
    )
    info = source.stat()
    identity = {
        "dev": str(info.st_dev),
        "ino": str(info.st_ino),
        "size": str(info.st_size),
        "mtime_ns": str(info.st_mtime_ns),
        "ctime_ns": str(info.st_ctime_ns),
    }
    for name in ["valid", "changed", "unadmitted", "past-end"]:
        run = root / name
        (run / "game/_hockeymon").mkdir(parents=True)
        pin = dict(identity)
        if name == "changed":
            pin["mtime_ns"] = str(info.st_mtime_ns - 1)
        request = {
            "schema": 1,
            "run_id": "scoreboard-camera-test",
            "operation": "scoreboard-camera-preview",
            "root": str(run),
            "game_dir": "game",
            "payload": {
                "uri": None,
                "preview_time": "00:00:02.0000"
                if name == "past-end"
                else "00:00:00.4000",
            },
            "authorized_media": [] if name == "unadmitted" else [str(source)],
            "runtime": {"scoreboard_media": [{"path": str(source), "identity": pin}]},
        }
        path = run / "request.json"
        path.write_text(json.dumps(request))
        result = subprocess.run(
            [str(binary), "--request", str(path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
            env={**os.environ, "HM_CONFIG_ROOT": str(baseline.parent)},
        )
        if name == "valid":
            assert result.returncode == 0, result.stderr
            data = json.loads((run / "operation-data.json").read_text())
            assert data["source_width"] == 384 and data["source_height"] == 216
            assert data["time_ns"] == "400000000"
            image = (run / "scoreboard-camera.png").read_bytes()
            assert image.startswith(b"\x89PNG\r\n\x1a\n")
        else:
            assert result.returncode != 0, name
            assert not (run / "scoreboard-camera.png").exists(), name
