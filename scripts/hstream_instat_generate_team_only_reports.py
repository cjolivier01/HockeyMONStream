#!/usr/bin/env python3
"""Create team-only InStat hockey match and player reports.

The script scans PDF files in a directory, discovers the teams on each report,
asks which team to keep, and writes filtered copies to a separate directory.

Requirements:
  * Python 3
  * Pillow (``python3 -m pip install Pillow``)
  * Poppler command-line tools: ``pdftotext`` and ``pdftoppm``
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover - environment-dependent message
    raise SystemExit("Pillow is required. Install it with: python3 -m pip install Pillow") from exc


BLUE = (21, 61, 105)
RED = (194, 34, 45)
TEXT = (28, 32, 37)
MUTED = (95, 105, 115)
LINE = (190, 198, 205)
WHITE = (255, 255, 255)
FONT_REGULAR = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
MATCHUP_RE = re.compile(r"^(.*?)\s+(\d+)\s*:\s*(\d+)\s+(.*?)$")
DATE_RE = re.compile(r"^\d{2}\.\d{2}\.\d{4}$")
XHTML = {"x": "http://www.w3.org/1999/xhtml"}
RESAMPLE_LANCZOS = getattr(Image, "Resampling", Image).LANCZOS


@dataclass(frozen=True)
class Report:
    path: Path
    kind: str
    team1: str
    score1: int
    score2: int
    team2: str
    date_raw: str

    @property
    def teams(self) -> tuple[str, str]:
        return self.team1, self.team2

    @property
    def display_date(self) -> str:
        try:
            return datetime.strptime(self.date_raw, "%d.%m.%Y").strftime("%-d %B %Y")
        except ValueError:
            return self.date_raw


def normalize(value: str) -> str:
    return " ".join(value.casefold().split())


def slug(value: str) -> str:
    ascii_value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    return re.sub(r"[^A-Za-z0-9]+", "_", ascii_value).strip("_") or "team"


def display_name(value: str) -> str:
    roman_or_suffix = {"Ii", "Iii", "Iv", "Vi", "Jr", "Sr"}
    return " ".join(part.upper() if part in roman_or_suffix else part for part in value.title().split())


def run_text(command: list[str]) -> str:
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return result.stdout


def page_text(pdf: Path, first: int, last: int | None = None, layout: bool = False) -> str:
    command = ["pdftotext", "-f", str(first), "-l", str(last or first)]
    if layout:
        command.append("-layout")
    command.extend([str(pdf), "-"])
    return run_text(command)


def inspect_report(pdf: Path) -> Report | None:
    try:
        lines = [line.strip() for line in page_text(pdf, 1).splitlines() if line.strip()]
    except subprocess.CalledProcessError:
        return None

    kind = ""
    marker_index = -1
    for i, line in enumerate(lines):
        upper = line.upper()
        if upper in {"MATCH REPORT", "PLAYER REPORT"}:
            kind = "match" if upper.startswith("MATCH") else "player"
            marker_index = i
            break
    if not kind:
        return None

    matchup = None
    matchup_index = -1
    for i in range(marker_index + 1, min(marker_index + 7, len(lines))):
        candidate = MATCHUP_RE.match(lines[i])
        if candidate:
            matchup = candidate
            matchup_index = i
            break
    if matchup is None:
        return None

    date_raw = "Unknown date"
    for line in lines[matchup_index + 1 : matchup_index + 5]:
        if DATE_RE.match(line):
            date_raw = line
            break

    team1, score1, score2, team2 = matchup.groups()
    return Report(pdf, kind, team1.strip(), int(score1), int(score2), team2.strip(), date_raw)


def scan_reports(directory: Path, recursive: bool) -> list[Report]:
    iterator = directory.rglob("*.pdf") if recursive else directory.glob("*.pdf")
    reports: list[Report] = []
    for pdf in sorted(iterator):
        lowered = pdf.name.casefold()
        if ".team_only." in lowered or ".scb_only." in lowered:
            continue
        report = inspect_report(pdf)
        if report:
            reports.append(report)
    return reports


def select_team(teams: list[str], requested: str | None) -> str:
    if requested:
        exact = [team for team in teams if normalize(team) == normalize(requested)]
        if exact:
            return exact[0]
        partial = [team for team in teams if normalize(requested) in normalize(team)]
        if len(partial) == 1:
            return partial[0]
        raise SystemExit(f"Team not found or ambiguous: {requested}")

    if not sys.stdin.isatty():
        raise SystemExit("No interactive terminal detected. Supply a team with --team TEAM.")
    print("\nTeams found:")
    for index, team in enumerate(teams, start=1):
        print(f"  {index}. {team}")
    while True:
        answer = input("\nSelect a team by number: ").strip()
        if answer.isdigit() and 1 <= int(answer) <= len(teams):
            return teams[int(answer) - 1]
        print(f"Enter a number from 1 to {len(teams)}.")


def find_font(bold: bool = False) -> str:
    preferred = FONT_BOLD if bold else FONT_REGULAR
    if Path(preferred).exists():
        return preferred
    query = "DejaVu Sans:style=Bold" if bold else "DejaVu Sans"
    if shutil.which("fc-match"):
        match = run_text(["fc-match", query, "-f", "%{file}\n"]).splitlines()
        if match and Path(match[0]).exists():
            return match[0]
    raise SystemExit("A TrueType font is required (DejaVu Sans is recommended).")


REGULAR_PATH = find_font(False)
BOLD_PATH = find_font(True)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(BOLD_PATH if bold else REGULAR_PATH, max(10, size))


class Renderer:
    def __init__(self, dpi: int, temp: Path):
        self.dpi = dpi
        self.temp = temp
        self.render_count = 0

    def page(self, pdf: Path, page_number: int) -> Image.Image:
        output = self.temp / f"render-{self.render_count:04d}"
        rendered_path = output.with_suffix(".png")
        self.render_count += 1
        subprocess.run(
            [
                "pdftoppm", "-f", str(page_number), "-l", str(page_number),
                "-singlefile", "-png", "-r", str(self.dpi), str(pdf), str(output),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            with Image.open(rendered_path) as rendered:
                return rendered.convert("RGB")
        finally:
            rendered_path.unlink(missing_ok=True)


def draw_header(
    image: Image.Image,
    team: str,
    report_kind: str,
    date: str,
    page_number: int,
    total_pages: int,
) -> Image.Image:
    out = image.copy()
    draw = ImageDraw.Draw(out)
    width, height = out.size
    band = int(height * .078)
    draw.rectangle((0, 0, width, band), fill=WHITE)
    title = f"{team.upper()} — {report_kind.upper()} REPORT"
    draw.text((int(width * .038), int(height * .018)), title, font=font(int(height * .023), True), fill=BLUE)
    draw.text(
        (int(width * .038), int(height * .049)),
        f"{date} · {team} statistics only",
        font=font(int(height * .014)),
        fill=MUTED,
    )
    page_label = f"{page_number} / {total_pages}"
    box = draw.textbbox((0, 0), page_label, font=font(int(height * .015), True))
    draw.text(
        (width - int(width * .038) - (box[2] - box[0]), int(height * .031)),
        page_label,
        font=font(int(height * .015), True),
        fill=TEXT,
    )
    draw.line((int(width * .038), band - 2, width - int(width * .038), band - 2), fill=LINE, width=2)
    return out


def make_cover(
    size: tuple[int, int],
    team: str,
    opponent: str,
    report_kind: str,
    date: str,
    items: list[str],
    total_pages: int,
) -> Image.Image:
    width, height = size
    image = Image.new("RGB", size, WHITE)
    draw = ImageDraw.Draw(image)
    margin = int(width * .07)
    draw.rectangle((0, 0, int(width * .025), height), fill=BLUE)
    team_size = int(height * .060)
    while draw.textbbox((0, 0), team.upper(), font=font(team_size, True))[2] > width - 2 * margin:
        team_size -= 2
    draw.text((margin, int(height * .10)), team.upper(), font=font(team_size, True), fill=BLUE)
    draw.text(
        (margin, int(height * .24)),
        f"{report_kind.upper()} REPORT",
        font=font(int(height * .044), True),
        fill=TEXT,
    )
    draw.text((margin, int(height * .31)), date, font=font(int(height * .025)), fill=MUTED)
    draw.text((margin, int(height * .36)), f"Opponent: {opponent}", font=font(int(height * .022)), fill=TEXT)
    draw.text(
        (margin, int(height * .425)),
        f"{team}-only filtered edition",
        font=font(int(height * .024), True),
        fill=BLUE,
    )
    draw.text(
        (margin, int(height * .47)),
        f"{opponent} team and player statistics have been omitted.",
        font=font(int(height * .018)),
        fill=MUTED,
    )

    start_y = int(height * .56)
    heading = f"{team} players included" if report_kind == "player" else "Contents"
    draw.text((margin, start_y), heading, font=font(int(height * .022), True), fill=TEXT)
    if report_kind == "player":
        columns = 3 if len(items) > 10 else 2
        rows = (len(items) + columns - 1) // columns
        column_width = int((width - 2 * margin) / columns)
        row_height = min(int(height * .038), int(height * .31 / max(rows, 1)))
        for index, item in enumerate(items):
            column, row = index // rows, index % rows
            draw.text(
                (margin + column * column_width, start_y + int(height * .05) + row * row_height),
                f"{index + 1}. {item}",
                font=font(int(height * .016)),
                fill=TEXT,
            )
    else:
        for index, item in enumerate(items, start=2):
            y = start_y + int(height * .05) + (index - 2) * int(height * .042)
            draw.text((margin, y), f"{index}. {item}", font=font(int(height * .018)), fill=TEXT)
    draw.text(
        (width - margin - int(width * .07), height - int(height * .07)),
        f"1 / {total_pages}",
        font=font(int(height * .018), True),
        fill=MUTED,
    )
    return image


def white_box(draw: ImageDraw.ImageDraw, size: tuple[int, int], box: tuple[float, float, float, float]) -> None:
    width, height = size
    draw.rectangle(
        (int(width * box[0]), int(height * box[1]), int(width * box[2]), int(height * box[3])),
        fill=WHITE,
    )


def filter_team_summary(
    source: Image.Image,
    selected_index: int,
    team: str,
    date: str,
    page_number: int,
    total_pages: int,
) -> Image.Image:
    """Mask the other team's columns on the standard InStat team-summary page."""
    out = source.copy()
    draw = ImageDraw.Draw(out)
    if selected_index == 0:
        masks = [
            (.258, .10, .332, .37), (.558, .10, .665, .37),
            (.558, .65, .665, .94),
            (.020, .705, .340, .990),
        ]
    else:
        masks = [
            (.203, .10, .258, .37), (.490, .10, .578, .37),
            (.490, .65, .558, .94),
            (.020, .400, .340, .705),
        ]
    for mask in masks:
        white_box(draw, out.size, mask)
    white_box(draw, out.size, (.345, .365, .665, .645))
    width, height = out.size
    draw.text(
        (int(width * .39), int(height * .48)),
        "Faceoff-zone comparison omitted\n(shared two-team statistic)",
        font=font(int(height * .018)),
        fill=MUTED,
        spacing=8,
        align="center",
    )
    leader_groups = [
        (.131, .147, .203, "INSTAT INDEX"),
        (.233, .248, .302, "TIME ON ICE"),
        (.334, .350, .404, "SHOTS"),
        (.436, .452, .506, "SHOTS ON GOAL"),
        (.538, .553, .607, "FACEOFFS WON"),
        (.640, .655, .709, "HITS"),
        (.741, .756, .811, "HITS AGAINST"),
    ]
    selected_x0, selected_x1 = ((.665, .810) if selected_index == 0 else (.810, .972))
    leader_rows = []
    for _, row_y0, row_y1, _ in leader_groups:
        leader_rows.append(
            (
                int(width * selected_x0),
                int(height * row_y0),
                source.crop(
                    (int(width * selected_x0), int(height * row_y0), int(width * selected_x1), int(height * row_y1))
                ),
            )
        )
    white_box(draw, out.size, (.665, .10, .972, .94))
    for paste_x, paste_y, strip in leader_rows:
        out.paste(strip, (paste_x, paste_y))
    draw = ImageDraw.Draw(out)
    for heading_y, _, _, label in leader_groups:
        draw.text(
            (int(width * selected_x0), int(height * (heading_y - .010))),
            label,
            font=font(int(height * .010)),
            fill=MUTED,
        )
    return draw_header(out, team, "match", date, page_number, total_pages)


def filter_player_table(
    source: Image.Image,
    selected_index: int,
    team: str,
    date: str,
    page_number: int,
    total_pages: int,
) -> Image.Image:
    width, height = source.size
    if selected_index == 0:
        crop = source.crop((int(width * .02), int(height * .075), int(width * .985), int(height * .405)))
    else:
        crop = source.crop((int(width * .02), int(height * .39), int(width * .985), int(height * .75)))
    page = Image.new("RGB", source.size, WHITE)
    page = draw_header(page, team, "match", date, page_number, total_pages)
    draw = ImageDraw.Draw(page)
    draw.text(
        (int(width * .038), int(height * .11)),
        f"{team} players’ statistics",
        font=font(int(height * .032), True),
        fill=TEXT,
    )
    max_width, max_height = int(width * .95), int(height * .72)
    scale = min(max_width / crop.width, max_height / crop.height)
    crop = crop.resize((int(crop.width * scale), int(crop.height * scale)), RESAMPLE_LANCZOS)
    page.paste(crop, ((width - crop.width) // 2, int(height * .18)))
    return page


def player_toc(pdf: Path, selected_team: str) -> list[tuple[int, str]]:
    xml = run_text(["pdftotext", "-f", "1", "-l", "1", "-bbox-layout", str(pdf), "-"])
    root = ET.fromstring(xml)
    page_node = root.find(".//x:page", XHTML)
    if page_node is None:
        raise ValueError("No first page in player report")
    page_width = float(page_node.attrib["width"])
    lines: list[tuple[float, float, str]] = []
    for node in page_node.findall(".//x:line", XHTML):
        words = [word.text or "" for word in node.findall("x:word", XHTML)]
        text = " ".join(words).strip()
        if text:
            lines.append((float(node.attrib["xMin"]), float(node.attrib["yMin"]), text))

    headings = [(x, y) for x, y, text in lines if normalize(text) == normalize(selected_team)]
    if not headings:
        raise ValueError(f"Could not locate {selected_team!r} on the player-report contents page")
    heading_x, heading_y = max(headings, key=lambda item: item[1])
    selected_half = 0 if heading_x < page_width / 2 else 1

    entries: list[tuple[int, str]] = []
    for number_x, number_y, text in lines:
        if number_y <= heading_y or not text.isdigit() or int(text) < 2:
            continue
        if (0 if number_x < page_width / 2 else 1) != selected_half:
            continue
        candidates = []
        for name_x, name_y, name in lines:
            if abs(name_y - number_y) > 2.0 or name_x >= number_x or not name.strip():
                continue
            if (0 if name_x < page_width / 2 else 1) != selected_half:
                continue
            # A contents column consists of a player name followed by its page
            # number. Some long names cross the nominal quarter-page boundary,
            # so proximity is more reliable than rigid column coordinates.
            if number_x - name_x > page_width * .27:
                continue
            if name.isdigit() or set(name.replace(" ", "")) <= {"."}:
                continue
            candidates.append((name_x, name))
        if candidates:
            name = max(candidates, key=lambda item: item[0])[1]
            entries.append((int(text), display_name(name)))

    entries = sorted(set(entries))
    if not entries:
        raise ValueError(f"No player pages found for {selected_team!r}")
    return entries


def save_pdf(pages: list[Image.Image], destination: Path, dpi: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    rgb_pages = [page.convert("RGB") for page in pages]
    rgb_pages[0].save(
        destination,
        "PDF",
        save_all=True,
        append_images=rgb_pages[1:],
        resolution=dpi,
        quality=88,
        optimize=True,
    )


def generate_match(report: Report, team: str, renderer: Renderer, output_dir: Path) -> Path:
    selected_index = 0 if normalize(report.team1) == normalize(team) else 1
    opponent = report.team2 if selected_index == 0 else report.team1
    team_pages = [4, 5, 6, 7] if selected_index == 0 else [8, 9, 10, 11]
    section_names = ["Lines statistics", "Game-time distribution", "Shots", "Hits distribution"]
    total_pages = 7
    sample = renderer.page(report.path, 2)
    pages = [
        make_cover(
            sample.size,
            team,
            opponent,
            "match",
            report.display_date,
            [f"{team} team summary", f"{team} players’ statistics", *section_names],
            total_pages,
        ),
        filter_team_summary(sample, selected_index, team, report.display_date, 2, total_pages),
        filter_player_table(renderer.page(report.path, 3), selected_index, team, report.display_date, 3, total_pages),
    ]
    for new_page, source_page in enumerate(team_pages, start=4):
        pages.append(
            draw_header(
                renderer.page(report.path, source_page),
                team,
                "match",
                report.display_date,
                new_page,
                total_pages,
            )
        )
    destination = output_dir / f"{report.path.stem}.{slug(team)}_only.pdf"
    save_pdf(pages, destination, renderer.dpi)
    return destination


def generate_player(report: Report, team: str, renderer: Renderer, output_dir: Path) -> Path:
    selected_index = 0 if normalize(report.team1) == normalize(team) else 1
    opponent = report.team2 if selected_index == 0 else report.team1
    entries = player_toc(report.path, team)
    total_pages = len(entries) + 1
    sample = renderer.page(report.path, entries[0][0])
    pages = [
        make_cover(
            sample.size,
            team,
            opponent,
            "player",
            report.display_date,
            [name for _, name in entries],
            total_pages,
        )
    ]
    for new_page, (source_page, _) in enumerate(entries, start=2):
        rendered = sample if new_page == 2 else renderer.page(report.path, source_page)
        pages.append(
            draw_header(
                rendered,
                team,
                "player",
                report.display_date,
                new_page,
                total_pages,
            )
        )
    destination = output_dir / f"{report.path.stem}.{slug(team)}_only.pdf"
    save_pdf(pages, destination, renderer.dpi)
    return destination


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory",
        nargs="?",
        default=".",
        type=Path,
        help="directory containing source PDFs (default: current directory)",
    )
    parser.add_argument("--team", help="team name (skips the interactive selection prompt)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="output directory (default: TEAM_only_reports inside the source directory)",
    )
    parser.add_argument("--dpi", type=int, default=180, help="rendering resolution; default: 180")
    parser.add_argument("--recursive", action="store_true", help="scan subdirectories too")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    directory = args.directory.expanduser().resolve()
    if not directory.is_dir():
        raise SystemExit(f"Not a directory: {directory}")
    if args.dpi < 96 or args.dpi > 600:
        raise SystemExit("--dpi must be between 96 and 600")
    for command in ("pdftotext", "pdftoppm"):
        if not shutil.which(command):
            raise SystemExit(f"Required command not found: {command}")

    reports = scan_reports(directory, args.recursive)
    if not reports:
        raise SystemExit("No supported InStat match/player report PDFs were found.")
    team_map: dict[str, str] = {}
    for report in reports:
        for team in report.teams:
            team_map.setdefault(normalize(team), team)
    teams = sorted(team_map.values(), key=str.casefold)
    team = select_team(teams, args.team)
    selected_reports = [
        report for report in reports if any(normalize(value) == normalize(team) for value in report.teams)
    ]
    output_dir = (args.output_dir or directory / f"{slug(team)}_only_reports").expanduser().resolve()

    print(f"\nSelected team: {team}")
    print(f"Supported reports found: {len(selected_reports)}")
    print(f"Output directory: {output_dir}\n")
    created: list[Path] = []
    failed: list[tuple[Path, str]] = []
    with tempfile.TemporaryDirectory(prefix="team-only-reports-") as temp_name:
        renderer = Renderer(args.dpi, Path(temp_name))
        for index, report in enumerate(selected_reports, start=1):
            print(f"[{index}/{len(selected_reports)}] {report.path.name}")
            try:
                relative_parent = report.path.relative_to(directory).parent
                report_output_dir = output_dir / relative_parent
                if report.kind == "match":
                    created.append(generate_match(report, team, renderer, report_output_dir))
                else:
                    created.append(generate_player(report, team, renderer, report_output_dir))
            except Exception as exc:  # keep processing other reports
                failed.append((report.path, str(exc)))
                print(f"  ERROR: {exc}", file=sys.stderr)

    print(f"\nCreated {len(created)} PDF(s):")
    for path in created:
        print(f"  {path}")
    if failed:
        print(f"\nFailed to process {len(failed)} PDF(s):", file=sys.stderr)
        for path, error in failed:
            print(f"  {path.name}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
