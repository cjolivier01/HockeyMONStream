#!/usr/bin/env python3
"""Actual bounded CPU diagnosis adapter, including scope and filesystem fences."""

import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


TOKEN = "a" * 64
INVOCATION = "b" * 32
CONTEXT = {
    "version": 1,
    "selected": {
        "matcher": "akaze-hamming",
        "mapping_backend": "opencv-magsac",
        "projection": "rectilinear",
        "frame_count": 4,
        "control_point_limit": 1500,
        "max_output_width": 8192,
        "camera": {
            "configuration": "gopro-mission-1",
            "horizontal_fov": 127.2,
            "vertical_fov": 95.0,
        },
    },
    "defaults": {
        "control_point_limit": 1500,
        "camera": {
            "configuration": "gopro-mission-1",
            "horizontal_fov": 127.2,
            "vertical_fov": 95.0,
        },
    },
    "camera_differs_from_defaults": False,
    "calibration_required": True,
    "calibration_start_stage": "input",
    "calibration_only": False,
}


def records(generation=1, context=None):
    envelope = {
        "version": 1,
        "binding": TOKEN,
        "invocation": INVOCATION,
        "generation": generation,
    }
    return (
        b"HSTREAM_FAILURE_SCOPE "
        + encoded({**envelope, "native_stage": 0, "instances": 1})
        + b"\nHSTREAM_FAILURE_CONTEXT "
        + encoded({**envelope, "context": context or CONTEXT})
        + b"\n"
    )


def run(binary, root, name, log, *, alter=None, prepare=None, success=True):
    directory = root / name
    directory.mkdir()
    (directory / "game").mkdir()
    path = directory / "pipeline.log"
    path.write_bytes(log)
    info = path.stat()
    request = {
        "schema": 1,
        "root": str(directory),
        "game_dir": "game",
        "run_id": "diagnostic-" + name,
        "operation": "calibration-failure-diagnose",
        "authorized_media": [],
        "runtime": {},
        "payload": {
            "version": 1,
            "binding": {
                "parent_run_id": "c" * 32,
                "native_run_id": "native-failure-test",
                "binding_token": TOKEN,
                "deployment_digest": "d" * 64,
                "supervisor_request_sha256": "e" * 64,
                "admission_digest": "f" * 64,
                "workflow": None,
            },
            "terminal": {
                "returncode": 1,
                "message": "INTERNAL: App run failed",
                "cancelled": False,
            },
            "log": {
                "path": "pipeline.log",
                "size": len(log),
                "sha256": hashlib.sha256(log).hexdigest(),
                "complete": True,
                "original_identity": {
                    "device": str(info.st_dev),
                    "inode": str(info.st_ino),
                    "size": str(info.st_size),
                    "mtime_ns": str(info.st_mtime_ns),
                    "ctime_ns": str(info.st_ctime_ns),
                },
            },
        },
    }
    if alter:
        alter(request["payload"])
    (directory / "operation-request.json").write_bytes(encoded(request))
    if prepare:
        prepare(directory, path)
    environment = {
        **os.environ,
        "CUDA_VISIBLE_DEVICES": "",
        "HIP_VISIBLE_DEVICES": "",
        "ROCR_VISIBLE_DEVICES": "",
        "HSTREAM_JOB_MANAGED_RUNTIME": "1",
    }
    process = subprocess.run(
        [str(binary), "--request", str(directory / "operation-request.json")],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
        check=False,
    )
    assert (process.returncode == 0) is success, (name, process.stdout.decode())
    receipt = json.loads((directory / "operation-result.json").read_text())
    assert receipt["state"] == ("complete" if success else "failed"), receipt
    assert not list((directory / "game").iterdir())
    if success:
        assert receipt["request_sha256"] == hashlib.sha256(encoded(request)).hexdigest()
        assert receipt["operation"] == "calibration-failure-diagnose"
        assert receipt["run_id"] == request["run_id"]
        assert len(receipt["artifacts"]) == 1
        assert receipt["artifacts"][0]["path"] == "operation-data.json"
        result = json.loads((directory / "operation-data.json").read_text())
        assert result == receipt["result"]
        assert result["binding"] == request["payload"]["binding"]
        assert result["log"] == request["payload"]["log"]
        assert result["invocation"] == INVOCATION
        return result
    assert not (directory / "operation-data.json").exists()
    return None


def test(binary, root):
    capabilities = json.loads(subprocess.check_output([str(binary), "--capabilities"]))
    assert "calibration-failure-diagnosis-v1" in capabilities["features"]
    assert "calibration-failure-diagnose" in capabilities["operations"]
    log = (
        records()
        + b"Cuda failure: status=2\nERROR from hmstitcher_conv0: failed to activate bufferpool\n"
    )
    log += b"ordinary output without diagnostic meaning\n" * 5000
    log += b"HSTREAM_CALIBRATION stage=calibration status=failed message=Pipeline failed during stitching calibration\n"
    result = run(binary, root, "input-oom", log)
    assert result["category"] == "cuda-oom-input-pool"
    assert result["applicability"] == "calibration"
    assert "try 4096 pixels (half)" in result["action"]
    context_record = records().splitlines()[1]
    assert result["context_record_sha256"] == hashlib.sha256(context_record).hexdigest()

    final_context = copy.deepcopy(CONTEXT)
    final_context["calibration_required"] = False
    final_context["calibration_start_stage"] = ""
    final_context["selected"]["max_output_width"] = 16384
    log = (
        records()
        + b"HSTREAM_CALIBRATION stage=calibration status=complete\n"
        + records(2, final_context)
    )
    failed = (
        log
        + b"HSTREAM_CALIBRATION stage=playback-restart status=failed message=Playback could not restart\nCUDA error at cudaBlend.cu:875: out of memory\n"
    )
    result = run(binary, root, "restart-oom", failed)
    assert result["cause_generation"] == 2 and result["category"] == "cuda-oom-blender"
    assert (
        result["solve_outcome"] == "complete" and result["restart_outcome"] == "failed"
    )
    assert result["applicability"] == "playback-restart"
    assert "try 8192 pixels (half)" in result["action"]
    completed = (
        log
        + b"HSTREAM_CALIBRATION stage=playback-restart status=complete\nPERMISSION_DENIED: archive output failed\n"
    )
    result = run(binary, root, "later-output", completed)
    assert result["applicability"] == "none" and result["restart_outcome"] == "complete"

    for stage, label in (
        ("projection", "Apply panorama projection"),
        ("materialize", "Prepare selected calibration frames"),
    ):
        log = (
            records()
            + b"HSTREAM_CALIBRATION stage=optimizer status=started\n"
            + b"HSTREAM_CALIBRATION stage=optimizer status=complete\n"
            + f"HSTREAM_CALIBRATION stage={stage} status=started\n".encode()
            + b"HSTREAM_CALIBRATION stage=calibration status=failed message=Native substep failed\n"
        )
        result = run(binary, root, "substep-" + stage, log)
        assert result["stage"] == label and result["solve_outcome"] == "failed"
        assert f"Failed stage: {label}" in result["rendered_text"]

    scope, context = records().splitlines()
    before_context = (
        scope
        + b"\nCuda failure: status=2\n"
        + context
        + b"\nHSTREAM_CALIBRATION stage=calibration status=failed message=App run failed\n"
    )
    result = run(binary, root, "pre-context", before_context)
    assert result["category"] == "cuda-oom" and result["applicability"] == "calibration"
    assert result["cause_generation"] == 0 and result["context_record_sha256"] == ""
    assert result["configuration"] == "Native launch context unavailable"
    assert "8192" not in result["action"]

    for name, altered in (
        ("token", lambda value: value["binding"].update(binding_token="c" * 64)),
        ("hash", lambda value: value["log"].update(sha256="0" * 64)),
        ("tail", lambda value: value["log"].update(complete=False)),
        ("path", lambda value: value["log"].update(path="../pipeline.log")),
        ("cancelled", lambda value: value["terminal"].update(cancelled=True)),
    ):
        run(binary, root, name, log, alter=altered, success=False)
    run(binary, root, "unscoped", b"Cuda failure: status=2\n", success=False)
    run(binary, root, "duplicate", records() + records(), success=False)
    run(binary, root, "long-line", records() + b"x" * 65537, success=False)
    run(
        binary, root, "large-log", records() + b"x\n" * (8 * 1024 * 1024), success=False
    )

    def symlink(directory, path):
        path.rename(directory / "other.log")
        path.symlink_to("other.log")

    def fifo(_directory, path):
        path.unlink()
        os.mkfifo(path)

    run(binary, root, "symlink", log, prepare=symlink, success=False)
    run(binary, root, "fifo", log, prepare=fifo, success=False)
    run(
        binary,
        root,
        "modified",
        log,
        prepare=lambda _directory, path: path.write_bytes(b"changed"),
        success=False,
    )
    print("Actual CPU failure diagnosis adapter passed")


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    if len(sys.argv) > 2:
        evidence_root = Path(sys.argv[2]).resolve()
        evidence_root.mkdir(parents=True, exist_ok=False)
        test(executable, evidence_root)
    else:
        with tempfile.TemporaryDirectory(
            prefix="hstream-failure-diagnosis-"
        ) as temporary:
            test(executable, Path(temporary))
