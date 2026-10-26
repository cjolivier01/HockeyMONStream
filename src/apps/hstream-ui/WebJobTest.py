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
import zlib
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
    tags = [
        (256, 4, 1, 8),
        (257, 4, 1, 8),
        (258, 3, 3, 134),
        (259, 3, 1, 1),
        (262, 3, 1, 2),
        (273, 4, 1, 140),
        (277, 3, 1, 3),
        (278, 4, 1, 8),
        (279, 4, 1, 192),
        (284, 3, 1, 1),
    ]
    tiff = b"II" + struct.pack("<HIH", 42, 8, len(tags))
    tiff += b"".join(struct.pack("<HHII", *tag) for tag in tags)
    tiff += struct.pack("<IHHH", 0, 8, 8, 8) + bytes([32, 96, 160]) * 64
    for name in (
        "mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif",
        "panorama.tif",
    ):
        (game / name).write_bytes(tiff)
    for name in ("hm_project.pto", "autooptimiser_out.pto", "seam_file.png"):
        (game / name).write_text("fixture")
    (game / "config.yaml").write_text(
        "hstream_ui:\n  stitching_calibration:\n    status: pending\n"
        f"    invalidation_id: {owner}\n"
    )


def png_fixture(path, width=4096, height=64):
    def chunk(kind, content):
        return (
            struct.pack("!I", len(content))
            + kind
            + content
            + struct.pack("!I", zlib.crc32(kind + content))
        )

    pixels = (b"\0" + bytes([255, 0, 0]) * width) * height
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


def calibration_still_contract(binary, root):
    for case, source in (
        ("png-only", "s.png"),
        ("prefer-png", "s.png"),
        ("tiff-fallback", "panorama.tif"),
        ("broken-png", "panorama.tif"),
        ("invalidated-png", "panorama.tif"),
        ("stale-still", None),
        ("pending-still", None),
        ("missing-still", None),
        ("wrong-generation-still", None),
        ("pending-recovery-still", None),
    ):
        directory = root / case
        directory.mkdir()
        game = directory / "game"
        calibration_fixture(game)
        config = "hstream_ui:\n  stitching_calibration:\n    status: complete\n"
        if case == "stale-still":
            config += "    stale_from: canvas\n"
        elif case == "pending-still":
            config = config.replace("complete", "pending")
        elif case == "invalidated-png":
            config += "    artifacts_invalidated: true\n"
        (game / "config.yaml").write_text(config)
        if case not in {"missing-still", "tiff-fallback"}:
            png_fixture(game / "s.png")
        if case in {"png-only", "missing-still"}:
            (game / "panorama.tif").unlink()
        if case == "broken-png":
            (game / "s.png").write_bytes(b"broken PNG")
        if case == "pending-recovery-still":
            journal = game / ".hstream-stitch-Ab12CD"
            journal.mkdir()
            (journal / "journal_version").write_text("2\n")
            (journal / "state").write_text("PREPARED\n")
            (journal / "rollback-payload").write_text("Must not recover")
        before = {
            path: (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
            for path in game.rglob("*")
            if path.is_file()
        }
        payload = {"config": {}}
        if case == "wrong-generation-still":
            payload["expected_generation"] = "another-generation"
        run(binary, request(directory, "calibration-still-read", payload))
        receipt = json.loads((directory / "operation-result.json").read_text())
        assert_artifacts(directory, receipt)
        result = receipt["result"]
        assert result["calibration_still_version"] == 1
        assert result["available"] is bool(source), (case, result)
        assert result["retryable"] is (case == "pending-recovery-still")
        expected_paths = {"operation-data.json"}
        if source:
            assert result["source_image"] == source
            assert result["preview"] == "saved-stitching-preview.png"
            assert len(result["source_revision"]) == 64
            int(result["source_revision"], 16)
            image = (directory / result["preview"]).read_bytes()
            assert image.startswith(b"\x89PNG")
            width, height = struct.unpack("!II", image[16:24])
            assert [width, height] == [result["width"], result["height"]]
            assert [width, height] == ([2048, 32] if source == "s.png" else [8, 8])
            expected_paths.add(result["preview"])
        else:
            assert "preview" not in result
            assert not (directory / "saved-stitching-preview.png").exists()
        assert {item["path"] for item in receipt["artifacts"]} == expected_paths
        assert json.loads((directory / "operation-data.json").read_text()) == result
        for path, identity in before.items():
            assert (
                path.read_bytes(),
                path.stat().st_ino,
                path.stat().st_mtime_ns,
            ) == identity
        assert not (game / "setup-preview.png").exists()

    directory = root / "still-output-symlink"
    directory.mkdir()
    game = directory / "game"
    game.mkdir()
    (game / "config.yaml").write_text(
        "hstream_ui: {stitching_calibration: {status: complete}}\n"
    )
    source = game / "s.png"
    png_fixture(source)
    before = (source.read_bytes(), source.stat().st_ino, source.stat().st_mtime_ns)
    (directory / "saved-stitching-preview.png").symlink_to(source)
    run(binary, request(directory, "calibration-still-read", {"config": {}}), 1)
    receipt = json.loads((directory / "operation-result.json").read_text())
    assert receipt["state"] == "failed" and not receipt["artifacts"]
    assert (
        source.read_bytes(),
        source.stat().st_ino,
        source.stat().st_mtime_ns,
    ) == before


def calibration_contract(binary, root):
    for case in (
        "valid",
        "valid-png-only",
        "missing-owner",
        "wrong-owner",
        "missing-generation",
        "missing-preview",
    ):
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
        elif case == "valid-png-only":
            (directory / "game" / "panorama.tif").unlink()
            png_fixture(directory / "game" / "s.png")
        path = request(directory, "calibration-complete", payload)
        valid = case in {"valid", "valid-png-only"}
        run(binary, path, 0 if valid else 1)
        receipt = json.loads((directory / "operation-result.json").read_text())
        if not valid:
            assert receipt["state"] == "failed" and not receipt["artifacts"]
            continue
        assert_artifacts(directory, receipt)
        preview = directory / receipt["result"]["preview"]
        assert preview.read_bytes().startswith(b"\x89PNG")
        assert any(
            item["path"] == "game/setup-preview.png" and item["kind"] == "image"
            for item in receipt["artifacts"]
        )
        assert "status: complete" in (directory / "game" / "config.yaml").read_text()
        identity = preview.stat().st_mtime_ns
        run(binary, path)
        assert preview.stat().st_mtime_ns == identity


def gopro_optics_movie(prescription=True, firmware="", axis_fov=True):
    """Bounded MP4/GPMF fixture shared in shape with VideoLensMetadataTest.cpp.

    No encoded video is needed: candidate creation imports header metadata only.
    Values describe the native test's convertible GoPro Mission 1 lens model.
    """

    def atom(name, payload):
        return struct.pack(">I4s", len(payload) + 8, name.encode()) + payload

    def tag(name, kind, payload, width=1):
        value = name.encode() + kind.encode() + bytes([width])
        value += struct.pack(">H", len(payload) // width) + payload
        return value + b"\0" * (-len(value) % 4)

    def floats(name, *values, width=4):
        return tag(name, "f", struct.pack(">" + "f" * len(values), *values), width)

    global_tags = b"".join(
        [
            tag("MINF", "c", b"MISSION 1 PRO"),
            tag("FMWR", "c", firmware.encode()) if firmware else b"",
            tag("PRJT", "F", b"GPRO", 4),
            tag("EISA", "c", b"HS EIS"),
            tag("HCTL", "c", b"Off"),
            floats("ZOOM", 1),
            tag("VFPS", "L", struct.pack(">II", 60000, 1001), 4),
            tag("VRES", "L", struct.pack(">II", 7680, 4320), 4),
        ]
    )
    lens = b"".join(
        [
            tag("DVID", "F", b"FOVL", 4),
            tag("VFOV", "c", b"W"),
            floats("ABSC", 0),
            floats("MAPX", 1),
            floats("MAPY", 1),
            floats("ARUW", 1.77777779102325),
            floats("ARWA", 1.77777779102325),
            floats("XFOV", 120.000427246094) if axis_fov else b"",
            floats("YFOV", 70.3657608032227) if axis_fov else b"",
            floats("ZFOV", 135.511917114258) if axis_fov else b"",
            floats("ZMPL", 0.476107776165009),
            (
                floats(
                    "POLY",
                    0,
                    2.67053532600403,
                    0.22922171652317,
                    -2.08499050140381,
                    1.64104437828064,
                    -7.24889386797406e-13,
                    4.47891317133589e-13,
                    0,
                    width=32,
                )
                if prescription
                else b""
            ),
            (
                tag(
                    "PYCF",
                    "c",
                    b"".join(f"r{i}".encode().ljust(8, b"\0") for i in range(8)),
                    8,
                )
                if prescription
                else b""
            ),
        ]
    )
    stsd = bytearray(100)
    stsd[12:16] = b"hvc1"
    stsd[40:44] = struct.pack(">HH", 7680, 4320)
    movie = atom(
        "udta", atom("GPMF", tag("DEVC", "\0", global_tags) + tag("DEVC", "\0", lens))
    )
    movie += atom("trak", atom("mdia", atom("minf", atom("stbl", atom("stsd", stsd)))))
    return atom("moov", movie)


def automatic_experiment_optics_contract(binary, root):
    lens = "  input_lens: {projection: rectilinear, optimization: fixed, distortion: [0, 0, 0], center_offset: [0, 0]}\n"
    manual = "  camera_config: manual\n  camera_automation: {fov: false, lens: false}\n"
    automatic = "  camera_config: auto\n  camera_automation: {fov: true, lens: true}\n"
    frozen = "  auto_input_lens_frozen: true\n"

    def create(name, source, override=None, metadata=True, lens_settings=None):
        directory = root / name
        directory.mkdir()
        settings = {
            "control_points": 100,
            "frame_count": 1,
            "stitch_frame_time": "00:00:00",
        }
        if override is not None:
            settings["camera"] = override
        path = request(directory, "experiment-create", {"settings": settings})
        for side in ("cam1", "cam2"):
            (directory / "game" / side).mkdir()
            (directory / "game" / side / "clip.mp4").write_bytes(
                metadata
                if isinstance(metadata, bytes)
                else gopro_optics_movie() if metadata else b"metadata unavailable"
            )
        (directory / "game/config.yaml").write_text(
            "game:\n  videos:\n    left: [cam1/clip.mp4]\n    right: [cam2/clip.mp4]\n"
            "hstream_ui:\n  video_roles:\n    left: [cam1/clip.mp4]\n    right: [cam2/clip.mp4]\n"
            "stitching:\n  camera_fov: {horizontal_fov: 111, vertical_fov: 77}\n"
            + (lens_settings or lens)
            + source
        )
        run(binary, path)
        candidate = json.loads((directory / "operation-result.json").read_text())[
            "result"
        ]["workspace"]
        return directory, candidate

    def imported(candidate):
        camera = candidate["settings"]["camera"]
        assert (
            abs(camera["camera_fov"]["horizontal_fov"] - 125.12273264898) < 1e-6
        ), camera
        assert abs(camera["input_lens"]["distortion"][1] - 0.04057437656) < 1e-8, camera

    # Initial Qt construction resolves unfrozen automatic sources. Explicit
    # policy or profile edits refresh a frozen source just like its dialog.
    _, fresh = create("auto-fresh", automatic)
    imported(fresh)
    _, policy = create(
        "auto-policy",
        manual + frozen,
        {"camera_automation": {"fov": True, "lens": True}},
    )
    imported(policy)
    directory, profile = create(
        "auto-profile", manual + frozen, {"camera_config": "auto"}
    )
    imported(profile)
    _, only_fov = create(
        "auto-fov-only", manual + frozen, {"camera_automation": {"fov": True}}
    )
    fov_camera = only_fov["settings"]["camera"]
    assert abs(fov_camera["camera_fov"]["horizontal_fov"] - 120.000427246094) < 1e-6
    assert fov_camera["input_lens"]["distortion"] == [0, 0, 0]
    _, only_lens = create(
        "auto-lens-only", manual + frozen, {"camera_automation": {"lens": True}}
    )
    lens_camera = only_lens["settings"]["camera"]
    assert lens_camera["camera_fov"] == {"horizontal_fov": 111, "vertical_fov": 77}
    assert abs(lens_camera["input_lens"]["distortion"][1] - 0.04057437656) < 1e-8
    _, optics_edit = create(
        "auto-optics-edit",
        automatic + frozen,
        {"input_lens": {"optimization": "refine"}},
    )
    assert (
        abs(
            optics_edit["settings"]["camera"]["camera_fov"]["horizontal_fov"]
            - 120.000427246094
        )
        < 1e-6
    )
    _, fallback = create(
        "auto-fallback",
        "  camera_config: manual\n  camera_automation: {fov: true, lens: true}\n"
        "  camera_configs:\n    manual:\n      display_name: Test source profile\n"
        "      horizontal_fov: 95\n      vertical_fov: 65\n"
        "      input_lens: {projection: rectilinear, optimization: fixed, distortion: [0, 0.02, 0], center_offset: [0, 0]}\n",
        metadata=False,
    )
    assert fallback["settings"]["camera"]["camera_fov"] == {
        "horizontal_fov": 95,
        "vertical_fov": 65,
    }
    assert fallback["settings"]["camera"]["input_lens"]["distortion"] == [0, 0.02, 0]
    # Compare automatic fallback with the actual bundled profile resolved by
    # the ordinary nonautomatic selection, without duplicating its defaults.
    references = {}
    for profile_name in ("manual", "gopro-mission-1", "gopro-mission-1-factory"):
        _, reference = create(
            "profile-reference-" + profile_name,
            manual + frozen,
            {
                "camera_config": profile_name,
                "camera_automation": {"fov": False, "lens": False},
            },
            metadata=False,
        )
        references[profile_name] = reference["settings"]["camera"]
    for profile_name in ("manual", "gopro-mission-1"):
        override = {"camera_automation": {"fov": True, "lens": True}}
        if profile_name != "manual":
            override["camera_config"] = profile_name
        _, fallback = create(
            "missing-profile-catalog-" + profile_name,
            manual + frozen,
            override,
            metadata=False,
            lens_settings="  input_lens: {projection: rectilinear, optimization: fixed, distortion: [0, 0.1, 0], center_offset: [0, 0]}\n",
        )
        camera = fallback["settings"]["camera"]
        assert camera["camera_fov"] == references[profile_name]["camera_fov"], camera
        assert camera["input_lens"] == references[profile_name]["input_lens"], camera
    # A partial game-level catalog retains the baseline's other profile leaves.
    _, partial = create(
        "partial-profile-catalog",
        manual
        + frozen
        + "  camera_configs:\n    gopro-mission-1:\n      horizontal_fov: 99\n",
        {
            "camera_config": "gopro-mission-1",
            "camera_automation": {"fov": True, "lens": True},
        },
        metadata=False,
    )
    assert partial["settings"]["camera"]["camera_fov"] == {
        "horizontal_fov": 99,
        "vertical_fov": references["gopro-mission-1"]["camera_fov"]["vertical_fov"],
    }
    assert (
        partial["settings"]["camera"]["input_lens"]
        == references["gopro-mission-1"]["input_lens"]
    )
    # Recording mode is identifiable but has no radial prescription. The
    # bundled verified mode catalog must provide the missing correction.
    _, mode = create(
        "baseline-recording-mode",
        automatic,
        metadata=gopro_optics_movie(
            prescription=False, firmware="H26.01.02.02.70", axis_fov=False
        ),
    )
    expected = references["gopro-mission-1-factory"]
    assert mode["settings"]["camera"]["input_lens"] == expected["input_lens"], mode
    assert mode["settings"]["camera"]["camera_fov"] == expected["camera_fov"], mode
    _, inherited = create("auto-inherited", automatic + frozen)
    assert inherited["settings"]["camera"]["camera_fov"] == {
        "horizontal_fov": 111,
        "vertical_fov": 77,
    }
    _, legacy = create("auto-legacy-manual", "")
    assert legacy["settings"]["camera"]["camera_config"] == "manual"
    assert legacy["settings"]["camera"]["camera_fov"] == {
        "horizontal_fov": 111,
        "vertical_fov": 77,
    }

    # Saved descriptors never import MP4s again, even if the files or source
    # config change. A fresh import here would replace the measured lens/FOV
    # with profile defaults because neither camera still has usable metadata.
    for side in ("cam1", "cam2"):
        (directory / "game" / side / "clip.mp4").write_bytes(b"metadata unavailable")
    game = directory / profile["game_directory"]
    config = game / "config.yaml"
    config.write_text(
        config.read_text().replace(
            "auto_input_lens_frozen: true", "auto_input_lens_frozen: false"
        )
    )
    inspection = game / "calibration-frame-inspection" / profile["invalidation_id"]
    inspection.mkdir(parents=True)
    (inspection / "frames.yaml").write_text(
        "version: 1\nexpected_pair_count: 1\ninvalidation_id: "
        + profile["invalidation_id"]
        + "\npairs:\n  - index: 0\n    left: {path: cam1/clip.mp4, source_seconds: 0}\n"
        "    right: {path: cam2/clip.mp4, source_seconds: 0}\n"
    )
    for side in ("left", "right"):
        (inspection / (side + "_0.jpg")).write_bytes(b"inspection-artifact-fixture")
    (directory / "operation-result.json").unlink()
    path = request(directory, "experiment-inspect", {"workspace": profile})
    run(binary, path)
    inspected = json.loads((directory / "operation-result.json").read_text())
    assert inspected["result"]["workspace"]["settings"] == profile["settings"]


def confined_experiment_optics_contract(binary, root):
    """Rejected source paths must not be opened by automatic metadata import."""
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
    external = root / "unapproved-optics.mp4"
    external.write_bytes(gopro_optics_movie())
    for kind in ("playlist", "alias", "authorized"):
        directory = root / ("optics-confined-" + kind)
        directory.mkdir()
        path = request(
            directory,
            "experiment-create",
            {
                "settings": {
                    "control_points": 100,
                    "frame_count": 1,
                    "stitch_frame_time": "00:00:00",
                    "camera": {"camera_config": "auto"},
                }
            },
        )
        for side in ("cam1", "cam2"):
            (directory / "game" / side).mkdir()
            media = directory / "game" / side / "clip.mp4"
            if kind != "playlist":
                media.symlink_to(external)
            else:
                media.write_bytes(gopro_optics_movie())
        config = (
            "game:\n  videos:\n    left: [cam1/clip.mp4]\n    right: [cam2/clip.mp4]\n"
        )
        if kind == "playlist":
            config += f"hstream_ui:\n  video_roles:\n    left: [{external}]\n    right: [{external}]\n"
        (directory / "game/config.yaml").write_text(config)
        if kind == "authorized":
            document = json.loads(path.read_text())
            document["authorized_media"] = [str(external)]
            path.write_text(json.dumps(document))
        events = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        assert events >= 0, "Could not observe rejected metadata input"
        try:
            assert libc.inotify_add_watch(events, os.fsencode(external), 0x20) >= 0
            run(binary, path, 0 if kind == "authorized" else 1)
            receipt = json.loads((directory / "operation-result.json").read_text())
            if kind == "authorized":
                camera = receipt["result"]["workspace"]["settings"]["camera"]
                assert (
                    abs(camera["camera_fov"]["horizontal_fov"] - 125.12273264898) < 1e-6
                )
            else:
                expected = (
                    "Workspace input must be named inside its game"
                    if kind == "playlist"
                    else "not an authorized media input"
                )
                assert expected in receipt["error"], receipt
            try:
                observed = os.read(events, 4096)
            except BlockingIOError:
                observed = b""
            if kind == "authorized":
                assert (
                    observed
                ), "Authorized input was not imported through its private alias"
            else:
                assert (
                    not observed
                ), "Metadata importer opened an unauthorized source before rejection"
        finally:
            os.close(events)


def pinned_archive_contract(binary, root):
    for case in (
        "valid",
        "changed",
        "final-link",
        "parent-link",
        "unauthorized",
        "wrong-marker",
    ):
        directory = root / ("pinned-" + case)
        directory.mkdir()
        original = root / ("archive-" + case)
        original.mkdir()
        archive = original / "archive.mp4"
        archive.write_bytes(b"owned archive")
        info = archive.stat()
        identity = {
            "dev": str(info.st_dev),
            "ino": str(info.st_ino),
            "size": str(info.st_size),
            "mtime_ns": str(info.st_mtime_ns),
            "ctime_ns": str(info.st_ctime_ns),
        }
        with sqlite3.connect(directory / "telemetry.db") as database:
            database.executescript("""
                PRAGMA application_id=1213027156;
                PRAGMA user_version=1;
                CREATE TABLE frames(run_id TEXT, sample_id INTEGER, pts_ns INTEGER,
                    geometry_id INTEGER, seek_epoch INTEGER, reset_epoch INTEGER, source_id INTEGER);
                CREATE TABLE tracks(run_id TEXT, sample_id INTEGER, tracking_id TEXT,
                    left REAL, top REAL, width REAL, height REAL, ordinal INTEGER);
                CREATE TABLE geometries(run_id TEXT, geometry_id INTEGER, width INTEGER, height INTEGER);
                CREATE TABLE cameras(run_id TEXT, sample_id INTEGER, role TEXT,
                    left REAL, top REAL, width REAL, height REAL);
                CREATE TABLE replay_frames(run_id TEXT, sample_id INTEGER,
                    edge_rotation_left REAL, edge_rotation_right REAL);
                INSERT INTO frames VALUES('recorded',1,1000000000,9,3,4,5);
                INSERT INTO frames VALUES('recorded',2,1033000000,9,3,4,5);
                INSERT INTO tracks VALUES('recorded',1,'17',200,100,100,200,0);
                INSERT INTO tracks VALUES('recorded',2,'17',220,100,100,200,0);
                INSERT INTO geometries VALUES('recorded',9,1000,500);
            """)
        authored = plan()
        cue = {
            "kind": "arrow",
            "text": "",
            "start_ms": 1000,
            "end_ms": 2000,
            "x": 0.5,
            "y": 0.35,
            "dx": 0,
            "dy": 0.12,
            "size": 0.055,
            "thickness": 0.008,
            "weight": 900,
            "color": "#ffff00",
            "blink": True,
            "blink_ms": 250,
            "motion": "fixed",
            "positions": [],
        }
        cue.update(
            {key: "" for key in ("database", "run_id", "track_id", "archive_path")}
        )
        cue.update(
            {
                key: 0
                for key in (
                    "geometry_id",
                    "seek_epoch",
                    "reset_epoch",
                    "source_id",
                    "telemetry_offset_ms",
                    "archive_origin_ms",
                    "archive_size",
                    "archive_mtime_ms",
                )
            }
        )
        authored["intervals"][0].update(
            is_card=False, start_ms=1000, end_ms=2000, annotations=[cue]
        )
        path = request(
            directory,
            "track-capture",
            {
                "plan": authored,
                "archive_path": "inputs/archive.mp4",
                "database": "telemetry.db",
                "run_id": "recorded",
                "route": "stitched",
                "item_index": 0,
                "cue_index": 0,
                "choice": {
                    "id": "17",
                    "geometry": 9,
                    "seek": 3,
                    "reset": 4,
                    "source": 5,
                },
            },
        )
        document = json.loads(path.read_text())
        document["authorized_media"] = [str(archive)]
        document["runtime"] = {
            "archive_input": {"path": str(archive), "identity": identity}
        }
        if case == "changed":
            archive.write_bytes(b"other archive")
        elif case == "final-link":
            target = original / "saved.mp4"
            archive.rename(target)
            archive.symlink_to(target)
        elif case == "parent-link":
            target = original.with_name(original.name + "-saved")
            original.rename(target)
            original.symlink_to(target, target_is_directory=True)
        elif case == "unauthorized":
            document["authorized_media"] = []
        elif case == "wrong-marker":
            document["payload"]["archive_path"] = "other.mp4"
        path.write_text(json.dumps(document))
        run(binary, path, 0 if case == "valid" else 1)
        receipt = json.loads((directory / "operation-result.json").read_text())
        assert not (directory / "inputs/archive.mp4").exists()
        if case == "valid":
            saved = receipt["result"]["plan"]
            cue = saved["intervals"][0]["annotations"][0]
            assert saved["archive_path"] == cue["archive_path"] == "inputs/archive.mp4"
            assert (
                cue["database"] == "telemetry.db"
                and cue["archive_size"] == info.st_size
            )
            assert (
                cue["geometry_id"] == 9
                and cue["seek_epoch"] == 3
                and cue["source_id"] == 5
            )
            assert [point["time_ms"] for point in cue["positions"]] == [1000, 1033]
            assert abs(cue["positions"][0]["x"] - 0.25) < 1e-9
            assert_artifacts(directory, receipt)
        else:
            assert receipt["state"] == "failed" and not receipt["artifacts"]


def runtime_defaults_contract(binary, root):
    config = root / "runtime-config"
    config.mkdir()
    home = root / "runtime-home"
    (config / "baseline.yaml").write_text(
        "runtime: {gpu_memory_profile: standard, amd: {gpu_id: 2}}\n"
    )
    (config / "ds_hockey_app_config.yaml").write_text(
        "application: {global-gpu-id: 2}\n"
    )
    environment = {
        **os.environ,
        "HOME": str(home),
        "HM_CONFIG_ROOT": str(config),
        "QT_QPA_PLATFORM": "invalid-readiness-platform",
    }
    probe = subprocess.run(
        [str(binary), "--runtime-defaults"],
        env=environment,
        capture_output=True,
        text=True,
        check=True,
        timeout=15,
    )
    assert json.loads(probe.stdout) == {
        "version": 1,
        "requested": "standard",
        "resolved": "standard",
        "gpu": 2,
    }
    assert not home.exists(), "Readiness must not create first-run preferences"
    defaults = home / ".config/hstream-ui/defaults.yaml"
    defaults.parent.mkdir(parents=True)
    defaults.write_text("runtime: {gpu-memory-profile: low}\n")
    probe = subprocess.run(
        [str(binary), "--runtime-defaults"],
        env=environment,
        capture_output=True,
        text=True,
        check=True,
        timeout=15,
    )
    assert json.loads(probe.stdout)["resolved"] == "low"
    assert not (home / ".hstream").exists()
    (config / "baseline.yaml").write_text("runtime: {}\n")
    probe = subprocess.run(
        [str(binary), "--runtime-defaults"],
        env=environment,
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert probe.returncode != 0 and not probe.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    probe = subprocess.run(
        [str(args.binary), "--capabilities"],
        env={**os.environ, "QT_QPA_PLATFORM": "invalid-readiness-platform"},
        capture_output=True,
        text=True,
        check=True,
        timeout=15,
    )
    capabilities = json.loads(probe.stdout)
    assert (
        capabilities["schema"]
        == capabilities["request_schema"]
        == capabilities["receipt_schema"]
        == 1
    )
    assert "calibration-complete" in capabilities["operations"]
    assert "calibration-still-read" in capabilities["operations"]
    assert "saved-calibration-still-v1" in capabilities["features"]
    assert "experiment-optics-filtering-v1" in capabilities["features"]
    assert "highlight-pinned-archive-v1" in capabilities["features"]
    assert len(capabilities["operations"]) == len(set(capabilities["operations"]))
    assert capabilities["maximum_request_bytes"] < capabilities["maximum_result_bytes"]
    with tempfile.TemporaryDirectory(prefix="hstream-web-contract-") as temporary:
        root = Path(temporary)
        runtime_defaults_contract(args.binary, root)
        pinned_archive_contract(args.binary, root)
        calibration_contract(args.binary, root)
        calibration_still_contract(args.binary, root)
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
                    "camera": {
                        "camera_fov": {"horizontal_fov": 103},
                        "input_lens": {"optimization": "fixed"},
                        "camera_automation": {"lens": False},
                    },
                    "control_point_filtering": {"homography_consistency": False},
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
            "stitching:\n  camera_config: manual\n"
            "  camera_fov: {horizontal_fov: 111, vertical_fov: 77}\n"
            "  input_lens: {projection: full-frame-fisheye, optimization: refine, "
            "distortion: [0.1, -0.2, 0.3], center_offset: [0.01, -0.02]}\n"
            "  camera_automation: {fov: false, lens: true}\n"
            "  control_point_filtering: {epipolar_ransac: false, homography_consistency: true}\n"
        )
        run(args.binary, path)
        receipt = json.loads((experiments / "operation-result.json").read_text())
        assert_artifacts(experiments, receipt)
        assert receipt["result"]["state"] == "queued"
        assert receipt["result"]["selection_available"] is False
        assert receipt["result"]["selection_fingerprint"] == ""
        assert not any(item["path"].endswith(".mp4") for item in receipt["artifacts"])
        candidate = receipt["result"]["workspace"]
        assert (experiments / candidate["game_directory"] / "config.yaml").is_file()
        optics = candidate["settings"]["camera"]
        assert optics["camera_fov"] == {"horizontal_fov": 103, "vertical_fov": 77}
        assert optics["input_lens"] == {
            "projection": "full-frame-fisheye",
            "optimization": "fixed",
            "distortion": [0.1, -0.2, 0.3],
            "center_offset": [0.01, -0.02],
        }
        assert optics["camera_automation"] == {"fov": False, "lens": False}
        assert candidate["settings"]["control_point_filtering"] == {
            "epipolar_ransac": False,
            "homography_consistency": False,
        }
        # A derived matrix must inherit the selected candidate's private
        # settings, not the older main game carried beside that workspace.
        (experiments / "operation-result.json").unlink()
        derived_request = request(
            experiments,
            "experiment-create",
            {
                "source_workspace": candidate,
                "sequence": 2,
                "settings": {
                    "control_points": 101,
                    "frame_count": 1,
                    "stitch_frame_time": "00:00:00",
                },
            },
        )
        document = json.loads(derived_request.read_text())
        document["authorized_media"] = [
            str(experiments / "game" / side / "clip.mp4") for side in ("cam1", "cam2")
        ]
        derived_request.write_text(json.dumps(document))
        run(args.binary, derived_request)
        derived = json.loads((experiments / "operation-result.json").read_text())[
            "result"
        ]
        assert (
            derived["workspace"]["settings"]["camera"]["camera_fov"]["horizontal_fov"]
            == 103
        )
        assert derived["workspace"]["settings"]["control_points"] == 101
        assert derived["workspace"]["game_directory"] != candidate["game_directory"]

        # A captured-pair fixture allows read-only inspection to exercise the
        # exact workspace JSON returned to the next worker, without GPU work.
        (experiments / "operation-result.json").unlink()
        run(
            args.binary,
            request(experiments, "experiment-inspect", {"workspace": candidate}),
        )
        unavailable = json.loads((experiments / "operation-result.json").read_text())[
            "result"
        ]
        assert unavailable["inspection_available"] is False
        assert unavailable["workspace"] == candidate
        assert unavailable["frames"] == []
        assert "No captured calibration frames" in unavailable["source_validation"]
        inspection = (
            experiments
            / candidate["game_directory"]
            / "calibration-frame-inspection"
            / candidate["invalidation_id"]
        )
        inspection.mkdir(parents=True)
        (inspection / "frames.yaml").write_text(
            "version: 1\nexpected_pair_count: 1\ninvalidation_id: "
            + candidate["invalidation_id"]
            + "\npairs:\n  - index: 0\n    left: {path: cam1/clip.mp4, source_seconds: 12.125}\n"
            "    right: {path: cam2/clip.mp4, source_seconds: 12.25}\n"
        )
        for side in ("left", "right"):
            (inspection / (side + "_0.jpg")).write_bytes(b"inspection-artifact-fixture")
        (experiments / "operation-result.json").unlink()
        path = request(experiments, "experiment-inspect", {"workspace": candidate})
        run(args.binary, path)
        inspected = json.loads((experiments / "operation-result.json").read_text())
        assert inspected["result"]["workspace"]["settings"] == candidate["settings"]
        result = inspected["result"]
        assert result["inspection_version"] == 1
        assert result["inspection_available"] is True
        assert result["expected_pair_count"] == 1
        assert result["player_selected"] is False
        assert result["coverage_grid"] == {
            "columns": 16,
            "rows": 9,
            "colors": ["#0000ff", "#008000", "#808000"],
        }
        frame = result["frames"][0]
        assert [source["name"] for source in frame["sources"]] == [
            "clip.mp4",
            "clip.mp4",
        ]
        assert [source["source_seconds"] for source in frame["sources"]] == [
            12.125,
            12.25,
        ]
        assert frame["match_images"] == [None, None]
        assert len(frame["thumbnails"]) == 2 and all(frame["thumbnails"])
        assert frame["coverage"] == [] and frame["size_band_counts"] == [0, 0, 0]
        assert_artifacts(experiments, inspected)
        # A missing left thumbnail cannot shift the right image into its place.
        (inspection / "left_0.jpg").unlink()
        (experiments / "operation-result.json").unlink()
        run(
            args.binary,
            request(experiments, "experiment-inspect", {"workspace": candidate}),
        )
        missing = json.loads((experiments / "operation-result.json").read_text())
        assert missing["result"]["frames"][0]["thumbnails"] == [
            None,
            frame["thumbnails"][1],
        ]
        assert_artifacts(experiments, missing)
        for change, expected_error in (
            ({"input_lens": {"distortion": [0, 0, 2]}}, "distortion"),
            ({"input_lens": {"center_offset": [0]}}, "array size"),
            ({"input_lens": {"projection": "unsupported"}}, "projection"),
            ({"camera_configs": {}}, "Unsupported experiment camera setting"),
        ):
            invalid = copy.deepcopy(candidate)
            invalid["settings"]["camera"].update(change)
            (experiments / "operation-result.json").unlink()
            path = request(experiments, "experiment-inspect", {"workspace": invalid})
            run(args.binary, path, 1)
            failed = json.loads((experiments / "operation-result.json").read_text())
            assert expected_error in failed["error"], failed
        invalid = copy.deepcopy(candidate)
        invalid["settings"]["control_point_filtering"]["unsupported"] = False
        (experiments / "operation-result.json").unlink()
        path = request(experiments, "experiment-inspect", {"workspace": invalid})
        run(args.binary, path, 1)
        assert (
            "Unknown stitching.control_point_filtering"
            in json.loads((experiments / "operation-result.json").read_text())["error"]
        )
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
        inherited = linked_receipt["result"]["workspace"]["settings"]
        assert inherited["camera"]["camera_fov"] == {
            "horizontal_fov": 111,
            "vertical_fov": 77,
        }
        assert inherited["camera"]["camera_automation"] == {"fov": False, "lens": True}
        assert inherited["control_point_filtering"] == {
            "epipolar_ransac": False,
            "homography_consistency": True,
        }
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
        automatic_experiment_optics_contract(args.binary, root)
        confined_experiment_optics_contract(args.binary, root)
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
