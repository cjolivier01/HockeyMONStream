#!/usr/bin/env python3
"""Print older hstream UI/CLI video and telemetry exports below a directory."""

from __future__ import annotations

import argparse
import re
from collections import defaultdict
from pathlib import Path


# The UI gives every published run a directory-wide generation. The CLI also
# uses these names for its work files and legacy telemetry exports.
VIDEO_RE = re.compile(
    r"^.*(?:tracking|stitched|program_4k|program-4k)_output"
    r"(?:-with-audio)?(?:-(?P<version>\d+))?\.(?:mp4|mkv)$",
    re.IGNORECASE,
)
TELEMETRY_RE = re.compile(
    r"^(?:tracking|detections|camera|camera_fast|hstream_frame_index|hstream_config_events)"
    r"(?:-(?P<version>\d+))?\.csv$"
    r"|^hstream_telemetry(?:-(?P<telemetry_version>\d+))?\.(?:db|sqlite|json)$"
    r"|^hstream_replay(?:-(?P<replay_version>\d+))?\.jsonl$"
    r"|^play_tracker_(?:source|effective)(?:-(?P<config_version>\d+))?\.yaml$"
    r"|^play_tracker_(?:source|effective|runtime_tuning|event)"
    r"(?:-(?P<event_version>\d+))?-[1-9]\d*\.yaml$"
    r"|^rink_mask_0-(?P<mask_version>\d+)\.png$",
    re.IGNORECASE,
)


def artifact_version(path: Path) -> int | None:
    """Return an hstream export's generation, or None for other files."""
    match = VIDEO_RE.fullmatch(path.name) or TELEMETRY_RE.fullmatch(path.name)
    if match is None:
        return None
    version = next((value for value in match.groupdict().values() if value is not None), None)
    return int(version) if version is not None else 0


def find_obsolete_mp4s(root: Path) -> list[Path]:
    """Find exports older than the newest generation in each directory."""
    artifacts_by_parent: dict[Path, list[tuple[int, Path]]] = defaultdict(list)
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        version = artifact_version(path)
        if version is not None:
            artifacts_by_parent[path.parent].append((version, path))

    obsolete = []
    for artifacts in artifacts_by_parent.values():
        newest = max(version for version, _ in artifacts)
        if newest > 0:
            obsolete.extend(path for version, path in artifacts if version < newest)
    return sorted(obsolete)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Print hstream UI/CLI video and telemetry exports older than the "
            "newest numbered generation in the same directory. Includes MP4 "
            "and MKV videos, SQLite databases, and legacy CSV, "
            "JSON, JSONL, YAML and numbered rink-mask companions. The game's "
            "unversioned rink_mask_0.png is preserved."
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
        if args.absolute:
            print(path)
        else:
            print(path.relative_to(root))


if __name__ == "__main__":
    main()
