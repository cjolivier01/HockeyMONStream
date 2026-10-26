#!/usr/bin/env python3
"""Run the opt-in real Qt tone-control check using a copied recording fixture."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

import yaml

MEDIA_SUFFIXES = {".mp4", ".mkv", ".mov", ".avi"}


def copy_fixture(source, destination):
    destination.mkdir()
    for entry in source.iterdir():
        if entry.name == ".hstream-ui" or (
            entry.name.startswith(".hstream-") and entry.name.endswith(".lock")
        ):
            continue
        # Recorded telemetry is disabled for this test and can dwarf the fixture.
        if entry.suffix.lower() in {
            ".db",
            ".sqlite",
            ".sqlite3",
        } or entry.name.endswith(("-wal", "-shm")):
            continue
        target = destination / entry.name
        if entry.is_dir():
            if entry.is_symlink():
                raise ValueError(f"Use a fixture with ordinary directories: {entry}")
            copy_fixture(entry, target)
        elif entry.is_file():
            if entry.suffix.lower() in MEDIA_SUFFIXES:
                target.symlink_to(entry.resolve())
            else:
                subprocess.run(
                    [
                        "cp",
                        "--preserve=timestamps",
                        "--reflink=auto",
                        "--",
                        str(entry),
                        str(target),
                    ],
                    check=True,
                )
        else:
            raise ValueError(f"Unsupported fixture entry: {entry}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-game", type=Path, required=True)
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--precision", choices=("standard", "high-bit"), required=True)
    args = parser.parse_args()
    source = args.source_game.resolve(strict=True)
    native = args.native_root.resolve(strict=True)
    artifacts = args.artifacts.resolve()
    if artifacts == source or source in artifacts.parents:
        parser.error("Artifacts must be outside the source recording")
    for name in ("config.yaml", "panorama.tif"):
        if not (source / name).is_file():
            parser.error(f"The calibrated recording must contain {name}")
    source_config_hash = hashlib.sha256(
        (source / "config.yaml").read_bytes()
    ).hexdigest()
    driver = (
        Path(__file__).resolve().parent.parent
        / "bazel-bin/src/apps/hstream-ui/tone_controls_gpu_test"
    )
    runner = native / "bazel-bin/src/apps/hstream-cli/hstream-cli"
    for path in (driver, runner):
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"Build the executable first: {path}")
    if not shutil.which("xvfb-run"):
        parser.error("xvfb-run is required for the Qt interaction check")
    artifacts.mkdir(parents=True, exist_ok=False)
    games = artifacts / "games"
    games.mkdir()
    game = games / source.name
    copy_fixture(source, game)
    config_path = game / "config.yaml"
    config = yaml.safe_load(config_path.read_text())
    ui = config.setdefault("hstream_ui", {})
    ui.pop("job", None)
    ui["show_crop_dialog"] = False
    ui["show_leveling_dialog"] = False
    ui["drivegpt_database"] = {"enabled": False}
    config.setdefault("pipeline", {}).setdefault("hmplaycropper", {})[
        "show-scoreboard"
    ] = 0
    config_path.write_text(yaml.safe_dump(config, sort_keys=False))
    (game / ".hstream-tone-gpu-fixture").touch()
    for name in ("preferences", "output", "state"):
        (artifacts / name).mkdir()
    env = dict(os.environ)
    env.update(
        XDG_CONFIG_HOME=str(artifacts / "preferences"),
        XDG_STATE_HOME=str(artifacts / "state"),
        HM_GAME_DIR=str(games),
        HM_OUTPUT_WORK_DIR=str(artifacts / "output"),
        HM_CONFIG_ROOT=str(native / "configs"),
        TONE_NATIVE_ROOT=str(native),
        TONE_GAME_ID=source.name,
        TONE_ARTIFACTS=str(artifacts),
        TONE_HIGH_BIT="1" if args.precision == "high-bit" else "0",
        QT_QPA_PLATFORM="xcb",
        HSTREAM_SCOREBOARD_BROWSER="/bin/true",
    )
    with runner.open("rb") as binary:
        digest = hashlib.file_digest(binary, "sha256").hexdigest()
    with (artifacts / "acceptance.log").open("wb") as log:
        result = subprocess.run(
            ["xvfb-run", "-a", "-s", "-screen 0 1600x1100x24", str(driver)],
            check=False,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    exit_code = result.returncode
    saved = yaml.safe_load(config_path.read_text())
    controls = saved.get("hstream_ui", {}).get("camera_controls", {})
    baseline_controls = yaml.safe_load((native / "configs/baseline.yaml").read_text())[
        "hstream_ui"
    ]["camera_controls"]
    expected = {
        "Bring_Up_Shadows": 0,
        "Lift_Shadow_Black_Point": 1,
        "Exposure_x100": 73,
    }
    saved_controls = {
        name: controls.get(name, baseline_controls[name]) for name in expected
    }
    saved_precision = (
        saved.get("pipeline", {})
        .get("hmstitcher", {})
        .get("properties", {})
        .get("high-bit-depth")
    )
    with runner.open("rb") as binary:
        final_digest = hashlib.file_digest(binary, "sha256").hexdigest()
    source_unchanged = (
        hashlib.sha256((source / "config.yaml").read_bytes()).hexdigest()
        == source_config_hash
    )
    if (
        saved_controls != expected
        or saved_precision != (args.precision == "standard")
        or digest != final_digest
        or not source_unchanged
    ):
        exit_code = 1
    (artifacts / "result.json").write_text(
        json.dumps(
            {
                "precision": args.precision,
                "native_sha256": digest,
                "exit_code": exit_code,
                "saved_controls": saved_controls,
                "saved_next_high_bit": saved_precision,
                "native_binary_unchanged": digest == final_digest,
                "source_config_unchanged": source_unchanged,
                "rendering": False,
            },
            indent=2,
        )
        + "\n"
    )
    print(f"Qt tone check exit={exit_code}; artifacts: {artifacts}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
