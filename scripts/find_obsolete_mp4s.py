#!/usr/bin/env python3
"""Print older hstream UI/CLI video and telemetry exports below a directory."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


VIDEO_RE = re.compile(
    r"^(?P<base>.*(?:tracking|stitched|program_4k|program-4k)_output(?:-with-audio)?)"
    r"(?:-(?P<version>\d+))?\.(?P<ext>mp4|mkv)$",
    re.IGNORECASE,
)
TELEMETRY_RE = re.compile(
    r"^(?:tracking|detections|camera|camera_fast|hstream_frame_index|hstream_config_events)"
    r"(?:-(?P<version>\d+))?\.csv$"
    r"|^hstream_telemetry(?:-(?P<database_version>\d+))?\.(?:db|sqlite|json)$"
    r"|^hstream_replay(?:-(?P<replay_version>\d+))?\.jsonl$",
    re.IGNORECASE,
)
CONFIG_BASE_RE = re.compile(r"^play_tracker_(?:source|effective)(?:-(\d+))?\.yaml$", re.IGNORECASE)
CONFIG_EVENT_RE = re.compile(
    r"^play_tracker_(?:source|effective|runtime_tuning|event)(?:-(\d+))?-[1-9]\d*\.yaml$",
    re.IGNORECASE,
)
MASK_RE = re.compile(r"^rink_mask_0-(\d+)\.png$", re.IGNORECASE)


def version_from_match(match: re.Match[str]) -> int:
    version = next((value for value in match.groups() if value is not None), None)
    return int(version) if version is not None else 0


def config_owners(paths: list[Path]) -> dict[str, set[int]]:
    """Find the run that names each config artifact in telemetry metadata."""
    owners: dict[str, set[int]] = defaultdict(set)
    for path in paths:
        name = path.name
        manifest = re.fullmatch(r"hstream_telemetry(?:-(\d+))?\.json", name, re.IGNORECASE)
        events = re.fullmatch(r"hstream_config_events(?:-(\d+))?\.csv", name, re.IGNORECASE)
        if manifest:
            try:
                document = json.loads(path.read_text(encoding="utf-8"))
                provenance = document.get("config_provenance", {}) if isinstance(document, dict) else {}
                if isinstance(provenance, dict):
                    for key in ("source_artifact", "effective_artifact"):
                        artifact = provenance.get(key)
                        if isinstance(artifact, str) and Path(artifact).name == artifact:
                            owners[artifact.lower()].add(version_from_match(manifest))
            except (OSError, UnicodeError, ValueError):
                continue
        elif events:
            try:
                with path.open(newline="", encoding="utf-8") as source:
                    for row in csv.reader(source):
                        if len(row) > 5 and row[5] and Path(row[5]).name == row[5]:
                            owners[row[5].lower()].add(version_from_match(events))
            except (OSError, UnicodeError, csv.Error):
                continue
    return owners


def find_obsolete_mp4s(root: Path) -> list[Path]:
    """Find exports older than the comparable generation in each directory."""
    paths_by_parent: dict[Path, list[Path]] = defaultdict(list)
    for path in root.rglob("*"):
        if path.is_file() and (
            VIDEO_RE.fullmatch(path.name)
            or TELEMETRY_RE.fullmatch(path.name)
            or CONFIG_BASE_RE.fullmatch(path.name)
            or CONFIG_EVENT_RE.fullmatch(path.name)
            or MASK_RE.fullmatch(path.name)
        ):
            paths_by_parent[path.parent].append(path)

    obsolete: list[Path] = []
    for parent, paths in paths_by_parent.items():
        videos: dict[str, list[tuple[int, Path, str]]] = defaultdict(list)
        ui_videos: list[tuple[int, Path]] = []
        telemetry: list[tuple[int, Path]] = []
        pending: list[Path] = []
        ui_prefix = parent.name.lower() + "-"
        for path in paths:
            if match := VIDEO_RE.fullmatch(path.name):
                version = int(match.group("version")) if match.group("version") is not None else 0
                videos[match.group("base").lower()].append((version, path, match.group("ext").lower()))
                if path.name.lower().startswith(ui_prefix) and match.group("ext").lower() == "mp4":
                    ui_videos.append((version, path))
            elif match := TELEMETRY_RE.fullmatch(path.name):
                telemetry.append((version_from_match(match), path))
            else:
                pending.append(path)

        ui_newest = max((version for version, _ in ui_videos), default=0)
        for versions in videos.values():
            newest = max(version for version, _, _ in versions)
            for version, path, extension in versions:
                # CLI work archives reuse their bare MKV path on every run.
                if version == 0 and extension == "mkv":
                    continue
                if version < (ui_newest if (version, path) in ui_videos else newest):
                    obsolete.append(path)

        telemetry_newest = max((version for version, _ in telemetry), default=0)
        telemetry_newest = max(telemetry_newest, ui_newest)
        obsolete.extend(path for version, path in telemetry if version < telemetry_newest)

        owners = config_owners(paths)
        for path in pending:
            name = path.name
            if match := MASK_RE.fullmatch(name):
                version = int(match.group(1))
            elif match := CONFIG_EVENT_RE.fullmatch(name):
                version = int(match.group(1)) if match.group(1) is not None else 0
                if CONFIG_BASE_RE.fullmatch(name):
                    assigned = owners.get(name.lower(), set())
                    if len(assigned) != 1:
                        continue  # A single suffix can be a run number or event sequence.
                    version = next(iter(assigned))
            elif match := CONFIG_BASE_RE.fullmatch(name):
                version = 0
            else:
                continue
            if version < telemetry_newest:
                obsolete.append(path)
    return sorted(obsolete)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Print older hstream UI/CLI MP4, MKV, and telemetry exports in each "
            "directory. CLI work MKVs and the game's unversioned rink_mask_0.png "
            "are preserved."
        )
    )
    parser.add_argument(
        "root",
        nargs="?",
        default=".",
        type=Path,
        help="Directory to scan recursively. Defaults to the current directory.",
    )
    parser.add_argument(
        "--absolute",
        action="store_true",
        help="Print absolute paths instead of paths relative to the scan root.",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    obsolete = find_obsolete_mp4s(root)
    for path in obsolete:
        print(path if args.absolute else path.relative_to(root))


if __name__ == "__main__":
    main()
