#!/usr/bin/env python3
"""List, or with --clean delete, superseded hstream exports below a directory."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
import re
from collections import defaultdict
from pathlib import Path


VIDEO_RE = re.compile(
    r"^(?P<base>.*(?:tracking|stitched|program_4k|program-4k|program_8k)_output(?:-with-audio)?)"
    r"(?:-(?P<version>\d+))?\.(?P<ext>mp4|mkv)$",
    re.IGNORECASE,
)
TELEMETRY_RE = re.compile(
    r"^(?:tracking|detections|camera|camera_fast|hstream_frame_index|hstream_config_events)"
    r"(?:-(?P<version>\d+))?\.csv$"
    r"|^(?:hm|hstream)_telemetry(?:-(?P<database_version>\d+))?\.(?:db|sqlite|json)$"
    r"|^.+_telemetry-(?P<game_database_version>\d+)\.(?:db|sqlite)$"
    r"|^hstream_replay(?:-(?P<replay_version>\d+))?\.jsonl$",
    re.IGNORECASE,
)
CONFIG_BASE_RE = re.compile(r"^play_tracker_(?:source|effective)(?:-(\d+))?\.yaml$", re.IGNORECASE)
CONFIG_EVENT_RE = re.compile(
    r"^play_tracker_(?:source|effective|runtime_tuning|event)(?:-(\d+))?-[1-9]\d*\.yaml$",
    re.IGNORECASE,
)
MASK_RE = re.compile(r"^rink_mask_0-(\d+)\.png$", re.IGNORECASE)
# The finalizer publishes each archive's game-time origin beside it. Nothing
# reads a sidecar whose video is gone, so it goes when the video does.
ARCHIVE_SIDECAR_SUFFIX = ".hstream-archive.json"


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
        newest_mp4 = max(
            (version for versions in videos.values() for version, _, ext in versions if ext == "mp4"),
            default=0,
        )
        for base, versions in videos.items():
            newest = max(version for version, _, _ in versions)
            nonofficial_4k = base.endswith(("program_4k_output", "program-4k_output"))
            for version, path, extension in versions:
                # CLI work archives reuse their bare MKV path on every run.
                if version == 0 and extension == "mkv":
                    continue
                nonofficial_4k_companion = (
                    version > 0
                    and extension == "mp4"
                    and nonofficial_4k
                )
                if version < newest or (nonofficial_4k_companion and version < newest_mp4):
                    obsolete.append(path)

        telemetry_kept = max((version for version, _ in telemetry), default=0)
        telemetry_newest = max(telemetry_kept, ui_newest)
        # A run that publishes video without DriveGPT carries ui_newest past
        # every database, which would retire a game's only telemetry with
        # nothing to supersede it. Whatever else goes, the newest stays.
        obsolete.extend(
            path for version, path in telemetry if version < telemetry_newest and version != telemetry_kept)

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


def archive_sidecar(path: Path) -> Path | None:
    """The origin sidecar beside an export, when there is one to take."""
    if not VIDEO_RE.fullmatch(path.name):
        return None
    sidecar = path.with_name(path.name + ARCHIVE_SIDECAR_SUFFIX)
    return sidecar if sidecar.is_file() and not sidecar.is_symlink() else None


def orphaned_sidecars(root: Path) -> list[Path]:
    """Sidecars whose export is already gone, from before they were swept."""
    orphans = []
    # "?*", not "*": a file named exactly the suffix has an empty base, and
    # with_name("") raises rather than yielding a video path to test.
    for path in root.rglob("?*" + ARCHIVE_SIDECAR_SUFFIX):
        if not path.is_file() or path.is_symlink():
            continue
        video = path.with_name(path.name[: -len(ARCHIVE_SIDECAR_SUFFIX)])
        # lexists, not is_file: an export reached through a symlink to an
        # unmounted volume is away, not gone, and its origin has to survive
        # the remount. This is the only rule here that deletes on absence.
        if VIDEO_RE.fullmatch(video.name) and not os.path.lexists(video):
            orphans.append(path)
    return sorted(orphans)


def symlink_target(source: Path, link_dir: Path) -> Path:
    """Prefer a relative target so the links survive a renamed or remounted tree."""
    try:
        relative = Path(os.path.relpath(source, link_dir))
    except ValueError:  # No common root to walk up to.
        return source
    try:
        # A relative target only works if link_dir's own parents are symlink-free.
        if (link_dir / relative).resolve() != source.resolve():
            return source
    except (OSError, RuntimeError):
        return source
    return relative


def symlink_plex_programs(
        root: Path, destination: Path, unpublished: list[str] | None = None, publish: bool = True) -> set[Path]:
    """Publish immediate games and return targets to preserve from cleanup.

    Names each game it could not publish into unpublished, because a caller
    about to delete things cannot tell a game with nothing to publish from one
    whose Program export it simply failed to spare. With publish false it
    works out both answers and writes nothing, so a dry run can report what
    the real sweep would spare instead of over-reporting.
    """
    published: set[Path] = set()
    if not destination.is_dir():
        print(f"Skipping Plex links: not an existing directory: {destination}", file=sys.stderr)
        return published
    destination = destination.resolve()
    for game in sorted(root.iterdir()):
        # Do not traverse directory aliases or archived seasons' game directories.
        if game.is_symlink() or not game.is_dir() or game.resolve() == destination:
            continue
        candidates = []
        for path in game.iterdir():
            match = VIDEO_RE.fullmatch(path.name)
            if not match or match.group("ext").lower() != "mp4" or not path.is_file():
                continue
            base = match.group("base").lower().removesuffix("-with-audio")
            is_4k = base.endswith(("program_4k_output", "program-4k_output"))
            if not is_4k and not base.endswith(("tracking_output", "program_8k_output")):
                continue
            candidates.append((int(match.group("version") or 0), is_4k,
                               "-with-audio" in match.group("base").lower(), path))
        if not candidates:
            continue
        source = max(candidates)[-1].absolute()
        link = destination / f"{game.name}.mp4"
        old_links = []
        game_root = game.resolve()
        for entry in destination.iterdir():
            if not entry.is_symlink():
                continue
            # Lexical targets also identify dangling links to removed exports.
            target = Path(os.path.abspath(entry.parent / entry.readlink()))
            try:
                belongs_to_game = target.is_relative_to(game_root) or target.resolve().is_relative_to(game_root)
            except (OSError, RuntimeError):
                belongs_to_game = target.is_relative_to(game_root)
            if belongs_to_game:
                old_links.append(entry)
        if os.path.lexists(link) and link not in old_links:
            print(f"Skipping Plex link: destination belongs to another file or game: {link}", file=sys.stderr)
            if unpublished is not None:
                unpublished.append(game.name)
            continue
        published.add(source)
        if not publish:
            continue
        # Publish first so a failed link creation leaves the old library intact.
        with tempfile.TemporaryDirectory(prefix=".plex-link-", dir=destination) as staging:
            temporary = Path(staging) / link.name
            # A relative target is read from the link's final home, not the staging one.
            temporary.symlink_to(symlink_target(source, destination))
            temporary.replace(link)
        for old in old_links:
            if old != link:
                old.unlink()
    return published


def write_job_scripts(root: Path) -> None:
    """Save an hstream-job script in each immediate game directory."""
    for game in sorted(root.iterdir()):
        # Do not traverse directory aliases or archived seasons' game directories.
        if game.is_symlink() or not game.is_dir():
            continue
        # hstream-job reads the game's config.yaml; other directories are not games.
        if not (game / "_hockeymon/config.yaml").is_file():
            continue
        try:
            # The default output is the game directory's own hstream-job.sh.
            completed = subprocess.run(
                ["hstream-job", "--force", "--game-dir", str(game)],
                capture_output=True,
                text=True,
            )
        except OSError as error:
            print(f"Skipping job scripts: cannot run hstream-job: {error}", file=sys.stderr)
            return
        if completed.returncode != 0:
            detail = (completed.stderr.strip() or completed.stdout.strip()
                      or f"exit status {completed.returncode}")
            print(f"Skipping job script for {game.name}: {detail}", file=sys.stderr)


def display(path: Path, root: Path, absolute: bool) -> Path:
    return path if absolute else path.relative_to(root)


def cleanup_targets(root: Path, published: set[Path], absolute: bool) -> list[tuple[Path, Path | None]]:
    """Each file to remove, paired with the origin sidecar that goes with it.

    Companions are resolved here rather than in find_obsolete_mp4s so that
    every rule sparing a video spares its origin too, and they stay paired so
    that a video which survives keeps the sidecar nothing else can reach.
    """
    targets: list[tuple[Path, Path | None]] = []
    for path in find_obsolete_mp4s(root):
        if path in published:
            continue
        # Discovery follows links, so a link named like an older export would
        # otherwise be unlinked while the export it points at survives.
        if path.is_symlink():
            print(f"Skipping symlink: {display(path, root, absolute)}", file=sys.stderr)
            continue
        targets.append((path, archive_sidecar(path)))
    targets.extend((orphan, None) for orphan in orphaned_sidecars(root))
    targets.sort(key=lambda pair: pair[0])
    return targets


def remove_obsolete(targets: list[tuple[Path, Path | None]], root: Path, absolute: bool, dry_run: bool) -> int:
    """Delete each pair, naming what went and what refused to. Returns failures.

    Everything it says goes to stderr, so `--dry-run` piped into rm removes
    nothing instead of feeding it the word "would".
    """
    verb = "would remove" if dry_run else "removed"
    removed = failures = 0

    def remove_one(path: Path) -> bool:
        nonlocal removed, failures
        shown = display(path, root, absolute)
        if not dry_run:
            try:
                path.unlink()
            except OSError as error:
                print(f"Could not remove {shown}: {error}", file=sys.stderr)
                failures += 1
                return False
        print(f"{verb} {shown}", file=sys.stderr)
        removed += 1
        return True

    for path, sidecar in targets:
        # The video goes first, and its sidecar only follows it out. An
        # interrupt or a refused unlink between the two then leaves an orphan
        # the sweep reclaims next run, where the other order would leave a
        # surviving export with no origin and nothing to notice.
        if remove_one(path) and sidecar is not None:
            remove_one(sidecar)
    summary = f"{verb} {removed} file{'' if removed == 1 else 's'}"
    if failures:
        summary += f", {failures} failed"
    print(summary, file=sys.stderr)
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "List older hstream UI/CLI MP4, MKV and telemetry exports in each "
            "directory, each with the archive origin sidecar it would leave "
            "behind, plus sidecars whose export is already gone. CLI work MKVs "
            "and the game's unversioned rink_mask_0.png are preserved. Use "
            "--clean to delete them: the listing is one bare path per line, so "
            "piping it into rm splits any game name containing a space."
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
    sweep = parser.add_mutually_exclusive_group()
    sweep.add_argument(
        "--clean",
        action="store_true",
        help="Delete the listed files here instead of printing them.",
    )
    sweep.add_argument(
        "--dry-run",
        action="store_true",
        help="Report what --clean would delete, on stderr, without deleting.",
    )
    parser.add_argument(
        "--symlink-plex",
        action="store_true",
        help=("Link each immediate child game's newest Program MP4 into Plex as GAME-ID.mp4; "
              "prefer 4K within the newest generation and replace all links into that game. "
              "Stitched exports and deeper directories are excluded."),
    )
    parser.add_argument(
        "--plex-dir",
        type=Path,
        default=Path("~/Plex"),
        help="Existing destination directory for --symlink-plex (default: ~/Plex).",
    )
    parser.add_argument(
        "--write-jobs",
        action="store_true",
        help=("Save hstream-job.sh in each immediate child game directory by running "
              "hstream-job --force --game-dir GAME. Directories without a config.yaml "
              "and deeper directories are excluded."),
    )
    args = parser.parse_args()

    root = args.root.resolve()
    sweeping = args.clean or args.dry_run
    # A mistyped root otherwise scans nothing and reports a reassuring zero.
    if not root.is_dir():
        parser.error(f"not a directory: {args.root}")
    plex_dir = args.plex_dir.expanduser()
    # Publication is the only thing sparing a 4K Program export from the
    # companion rule, and it spares nothing for a game it could not publish.
    # Warning and sweeping anyway deletes exactly what it exists to keep, so
    # refuse on a destination that is missing or unmounted, and again below on
    # any individual game it had to skip.
    if sweeping and args.symlink_plex and not plex_dir.is_dir():
        parser.error(f"--plex-dir is not an existing directory: {args.plex_dir}")
    unpublished: list[str] = []
    # A dry run works out what publication would spare but writes no links, so
    # that previewing --clean --symlink-plex reports what it would really do.
    published = (symlink_plex_programs(root, plex_dir, unpublished, publish=not args.dry_run)
                 if args.symlink_plex else set())
    if sweeping and unpublished:
        parser.error("cannot sweep games that were not published: " + ", ".join(unpublished))
    # --write-jobs overwrites a script in every game, which a dry run must not.
    if args.write_jobs and not args.dry_run:
        write_job_scripts(root)
    targets = cleanup_targets(root, published, args.absolute)
    if sweeping:
        if remove_obsolete(targets, root, args.absolute, args.dry_run):
            sys.exit(1)
        return
    for path, sidecar in targets:
        print(display(path, root, args.absolute))
        if sidecar is not None:
            print(display(sidecar, root, args.absolute))


if __name__ == "__main__":
    main()
