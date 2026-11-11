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
NONOFFICIAL_4K_SUFFIXES = ("program_4k_output", "program-4k_output")
# The exports Plex publishes, as opposed to a stitching intermediate.
PUBLISHABLE_SUFFIXES = NONOFFICIAL_4K_SUFFIXES + ("tracking_output", "program_8k_output")


def plex_rank(base: str) -> tuple[bool, bool, bool]:
    """How strongly Plex prefers a video base, ranked worst to best.

    Shared with symlink_plex_programs so that the file a sweep names as the
    successor is the file publication would actually put in the library.
    """
    stripped = base.lower().removesuffix("-with-audio")
    is_4k = stripped.endswith(NONOFFICIAL_4K_SUFFIXES)
    return (stripped.endswith(PUBLISHABLE_SUFFIXES), is_4k, "-with-audio" in base.lower())


def generation_group(match: re.Match[str]) -> int | None:
    """The index of the group holding the generation, where the name carries one."""
    return next((index for index, value in enumerate(match.groups(), start=1) if value is not None), None)


def version_from_match(match: re.Match[str]) -> int:
    index = generation_group(match)
    return int(match.group(index)) if index is not None else 0


def generation_stem(match: re.Match[str]) -> str:
    """The matched name with its generation cut out, so it pairs with its successor.

    Cut by position rather than by trailing suffix: a config event's name ends
    in its event sequence, so stripping the last -N there would file every
    generation of one event under a different kind and pair none of them.
    """
    index = generation_group(match)
    if index is None:
        return match.string.lower()
    start, end = match.span(index)
    # The dash introducing the generation goes out with it.
    return (match.string[:start - 1] + match.string[end:]).lower()


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


def find_obsolete_mp4s(root: Path, superseded_by: dict[Path, Path] | None = None) -> list[Path]:
    """Find exports older than the comparable generation in each directory.

    Records the surviving file of the same kind into superseded_by, because a
    caller about to delete something has to be able to show what replaces it.
    A file absent from it has no replacement of its kind, which the rules
    comparing a bare generation often leave it with.
    """
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

    def retire(path: Path, successor: Path | None) -> None:
        obsolete.append(path)
        if superseded_by is not None and successor is not None:
            superseded_by[path] = successor

    for parent, paths in paths_by_parent.items():
        videos: dict[str, list[tuple[int, Path, str]]] = defaultdict(list)
        ui_videos: list[tuple[int, Path]] = []
        telemetry: list[tuple[int, Path, str]] = []
        pending: list[Path] = []
        ui_prefix = parent.name.lower() + "-"
        for path in paths:
            if match := VIDEO_RE.fullmatch(path.name):
                version = int(match.group("version")) if match.group("version") is not None else 0
                videos[match.group("base").lower()].append((version, path, match.group("ext").lower()))
                if path.name.lower().startswith(ui_prefix) and match.group("ext").lower() == "mp4":
                    ui_videos.append((version, path))
            elif match := TELEMETRY_RE.fullmatch(path.name):
                telemetry.append((version_from_match(match), path, generation_stem(match)))
            else:
                pending.append(path)

        ui_newest = max((version for version, _ in ui_videos), default=0)
        # A generation holding several MP4s is credited to the one Plex would
        # publish, so the companion rule below does not retire a Program export
        # in the name of the stitching intermediate it was cut from.
        mp4s = [(version, plex_rank(base), path)
                for base, versions in videos.items()
                for version, path, ext in versions if ext == "mp4"]
        newest_mp4, _, newest_mp4_path = max(mp4s, default=(0, (), None))
        for base, versions in videos.items():
            newest = max(version for version, _, _ in versions)
            # The MP4 takes a tie: an MKV only stands in for a generation it holds alone.
            survivor = max((version, extension == "mp4", path) for version, path, extension in versions)[-1]
            nonofficial_4k = base.endswith(NONOFFICIAL_4K_SUFFIXES)
            for version, path, extension in versions:
                # CLI work archives reuse their bare MKV path on every run.
                if version == 0 and extension == "mkv":
                    continue
                nonofficial_4k_companion = (
                    version > 0
                    and extension == "mp4"
                    and nonofficial_4k
                )
                if version < newest:
                    retire(path, survivor)
                elif nonofficial_4k_companion and version < newest_mp4:
                    retire(path, newest_mp4_path)

        telemetry_kept = max((version for version, _, _ in telemetry), default=0)
        telemetry_newest = max(telemetry_kept, ui_newest)
        kept_telemetry = {stem: path for version, path, stem in sorted(telemetry)
                          if version == telemetry_kept}
        # A run that publishes video without DriveGPT carries ui_newest past
        # every database, which would retire a game's only telemetry with
        # nothing to supersede it. Whatever else goes, the newest stays.
        for version, path, stem in telemetry:
            if version < telemetry_newest and version != telemetry_kept:
                # Only its own kind is a real replacement. Naming the file
                # that happens to carry the generation instead would read as
                # these readings having moved in there.
                retire(path, kept_telemetry.get(stem))

        owners = config_owners(paths)
        dated: list[tuple[int, Path, str]] = []
        for path in pending:
            name = path.name
            if match := MASK_RE.fullmatch(name):
                version = int(match.group(1))
            elif match := CONFIG_EVENT_RE.fullmatch(name):
                version = int(match.group(1)) if match.group(1) is not None else 0
                if base_match := CONFIG_BASE_RE.fullmatch(name):
                    assigned = owners.get(name.lower(), set())
                    if len(assigned) != 1:
                        continue  # A single suffix can be a run number or event sequence.
                    version = next(iter(assigned))
                    # The suffix is the generation after all, so read the stem
                    # off the pattern that says so rather than the event one.
                    match = base_match
            elif match := CONFIG_BASE_RE.fullmatch(name):
                version = 0
            else:
                continue
            dated.append((version, path, generation_stem(match)))

        # Ascending, so the last write per kind is its newest surviving file
        # and rglob's arbitrary order cannot change which one the table names.
        kept_pending = {stem: path for version, path, stem in sorted(dated)
                        if version >= telemetry_newest}
        for version, path, stem in dated:
            if version < telemetry_newest:
                retire(path, kept_pending.get(stem))
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
            publishable, is_4k, with_audio = plex_rank(match.group("base"))
            if not publishable:
                continue
            candidates.append((int(match.group("version") or 0), is_4k, with_audio, path))
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


def cleanup_targets(root: Path, published: set[Path], absolute: bool,
                    superseded_by: dict[Path, Path] | None = None) -> list[tuple[Path, Path | None]]:
    """Each file to remove, paired with the origin sidecar that goes with it.

    Companions are resolved here rather than in find_obsolete_mp4s so that
    every rule sparing a video spares its origin too, and they stay paired so
    that a video which survives keeps the sidecar nothing else can reach.
    """
    targets: list[tuple[Path, Path | None]] = []
    for path in find_obsolete_mp4s(root, superseded_by):
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
    if superseded_by is not None:
        resolve_successors(superseded_by, {path for path, _ in targets})
    return targets


def resolve_successors(superseded_by: dict[Path, Path], going: set[Path]) -> None:
    """Follow each successor past anything this same sweep is about to delete.

    A generation retires a file through whatever is newest at the time, which
    can be a file the next rule retires in turn. Promising the user that one
    doomed export replaces another is the one thing the column must not do, so
    the chain is walked to something that will still be there afterwards.
    """
    def doomed(path: Path) -> bool:
        if path in going:
            return True
        # A link is worth no more than what it points at, and discovery came
        # through it: the export behind it can be on the list under its own
        # name, which would leave the named successor dangling.
        try:
            return path.is_symlink() and path.resolve() in going
        except (OSError, RuntimeError):
            return False

    for path in list(superseded_by):
        successor, seen = superseded_by[path], {path}
        while successor is not None and doomed(successor) and successor not in seen:
            seen.add(successor)
            successor = superseded_by.get(successor)
        if successor is None or doomed(successor):
            del superseded_by[path]
        else:
            superseded_by[path] = successor


def supersession(path: Path, superseded_by: dict[Path, Path], root: Path, absolute: bool) -> str:
    """What replaces one file, as the column says it."""
    if successor := superseded_by.get(path):
        return str(display(successor, root, absolute))
    # Only an orphan reaches the list under a sidecar's own name; a companion
    # is carried on its export's row instead.
    if path.name.endswith(ARCHIVE_SIDECAR_SUFFIX):
        return "(export already gone)"
    return "(nothing of its kind survives)"


def print_cleanup_table(targets: list[tuple[Path, Path | None]], superseded_by: dict[Path, Path],
                        root: Path, absolute: bool) -> None:
    """Print the sweep as a table: every file to delete beside what replaces it.

    rich is imported here and not at the top so that listing, --dry-run and
    the Plex and job-script passes keep working on a box without it. Only the
    table needs it, and only --clean draws the table.
    """
    from rich.console import Console
    from rich.table import Table

    # Not "exports": the sweep also takes telemetry, configs and rink masks,
    # and this heading is the one screen standing between them and an unlink.
    table = Table(title="Obsolete files to delete", title_style="bold", header_style="bold")
    table.add_column("#", justify="right", style="dim")
    table.add_column("File to delete", style="red", overflow="fold")
    table.add_column("Superseded by", style="green", overflow="fold")
    number = 0
    for path, sidecar in targets:
        # Sidecars are numbered too, so the last row counts what --all asks for.
        number += 1
        table.add_row(str(number), str(display(path, root, absolute)),
                      supersession(path, superseded_by, root, absolute))
        if sidecar is not None:
            number += 1
            table.add_row(str(number), str(display(sidecar, root, absolute)),
                          f"(origin of {display(path, root, absolute)})")
    # A game name is not markup: brackets in one would be eaten as a style
    # tag, an unbalanced pair would abort the sweep before it swept anything,
    # and "a:vs:b" would reach the user spelled with an emoji.
    Console(stderr=True, markup=False, emoji=False, highlight=False).print(table)


def confirm(question: str) -> bool:
    """Ask on stderr, where the sweep already reports. Anything but yes is no."""
    print(f"{question} [y/N] ", end="", file=sys.stderr, flush=True)
    try:
        return input().strip().lower() in ("y", "yes")
    except UnicodeDecodeError:
        return False  # Whatever that byte was, nobody typed it meaning yes.


def confirmed_targets(targets: list[tuple[Path, Path | None]], root: Path, absolute: bool,
                      once: bool) -> list[tuple[Path, Path | None]]:
    """Narrow the sweep to what was approved, in one answer or file by file.

    A sidecar is never offered on its own: it is the origin of the export on
    its row, and keeping one without the other is what the pairing prevents.
    """
    approved: list[tuple[Path, Path | None]] = []
    try:
        if once:
            count = sum(2 if sidecar is not None else 1 for _, sidecar in targets)
            return targets if confirm(f"Delete all {count} file{'' if count == 1 else 's'}?") else []
        for path, sidecar in targets:
            question = f"Delete {display(path, root, absolute)}"
            if sidecar is not None:
                question += f" and {display(sidecar, root, absolute)}"
            if confirm(question + "?"):
                approved.append((path, sidecar))
    # RuntimeError is a closed fd 0 rather than an empty one, which a
    # daemonised caller hands us just as readily. Either way nobody is there
    # to answer the rest, and an unattended --clean asking a season's worth of
    # questions it cannot hear is only noise.
    except (EOFError, RuntimeError):
        print("\nNo answer: leaving the rest in place.", file=sys.stderr)
    return approved


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
            "and the game's unversioned rink_mask_0.png are preserved. The "
            "listing is one bare path per line, so piping it into rm splits "
            "any game name containing a space. Use --clean to delete them "
            "here instead, which tables each file against the one superseding "
            "it and then asks."
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
        help=("Delete the listed files here instead of printing them, after showing a table "
              "of each one beside what supersedes it and confirming them file by file."),
    )
    sweep.add_argument(
        "--dry-run",
        action="store_true",
        help="Report everything --clean could delete, on stderr, without deleting.",
    )
    answering = parser.add_mutually_exclusive_group()
    answering.add_argument(
        "--all",
        action="store_true",
        help="Confirm the whole --clean sweep in one answer instead of file by file.",
    )
    answering.add_argument(
        "--force",
        action="store_true",
        help="Delete without confirming. The --clean table is still printed unless --quiet.",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Do not print the --clean table of files to delete.",
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
    # Each of these only reaches the confirmation --clean asks for. Accepting
    # one silently elsewhere reads as a sweep that ran and found nothing.
    for enabled, flag in ((args.all, "--all"), (args.force, "--force"), (args.quiet, "--quiet")):
        if enabled and not args.clean:
            parser.error(f"{flag} only applies to --clean")
    plex_dir = args.plex_dir.expanduser()
    # Publication is the only thing sparing a 4K Program export from the
    # companion rule, and it spares nothing for a game it could not publish.
    # Warning and sweeping anyway deletes exactly what it exists to keep, so
    # refuse on a destination that is missing or unmounted, and again below on
    # any individual game it had to skip.
    if sweeping and args.symlink_plex and not plex_dir.is_dir():
        parser.error(f"--plex-dir is not an existing directory: {args.plex_dir}")
    unpublished: list[str] = []
    # Work out what publication spares without writing anything yet. A dry run
    # must not write at all, and --clean must not either until it has asked:
    # publishing relinks the library and unlinks what it replaced, so doing it
    # first would mean answering "no" to the sweep and still changing the tree.
    published = (symlink_plex_programs(root, plex_dir, unpublished, publish=False)
                 if args.symlink_plex else set())
    if sweeping and unpublished:
        parser.error("cannot sweep games that were not published: " + ", ".join(unpublished))
    superseded_by: dict[Path, Path] = {}
    targets = cleanup_targets(root, published, args.absolute, superseded_by)
    if args.clean and targets:
        if not args.quiet:
            try:
                print_cleanup_table(targets, superseded_by, root, args.absolute)
            except ImportError as error:
                # --force waived the confirmation the table informs, so it has
                # nothing to lose by going without one. An answer given against
                # no table at all is the only thing worth refusing over.
                if not args.force:
                    parser.error(f"--clean needs rich to draw its table ({error}). "
                                 "Install it, or pass --quiet to sweep without one.")
                print(f"Skipping the table: {error}", file=sys.stderr)
        if not args.force:
            try:
                targets = confirmed_targets(targets, root, args.absolute, args.all)
            except KeyboardInterrupt:
                print("\nStopped before deleting anything.", file=sys.stderr)
                sys.exit(130)
    # Everything below here writes, and the question above has been answered.
    if not args.dry_run:
        if args.symlink_plex:
            symlink_plex_programs(root, plex_dir)
        # --write-jobs overwrites a script in every game, which a dry run must not.
        if args.write_jobs:
            write_job_scripts(root)
    if sweeping:
        try:
            failures = remove_obsolete(targets, root, args.absolute, args.dry_run)
        except KeyboardInterrupt:
            # Partway through the unlinks, where --force spends its whole run.
            # remove_obsolete has already named each file as it went.
            print("\nStopped partway through.", file=sys.stderr)
            sys.exit(130)
        if failures:
            sys.exit(1)
        return
    for path, sidecar in targets:
        print(display(path, root, args.absolute))
        if sidecar is not None:
            print(display(sidecar, root, args.absolute))


if __name__ == "__main__":
    main()
