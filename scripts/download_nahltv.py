#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["playwright>=1.45", "yt-dlp>=2025.10.14"]
# ///
"""Save an NAHLTV replay at its highest available resolution.

Run ./download_nahltv.py https://www.nahltv.com/watch/44257
Requires uv (or Python with playwright and yt-dlp), Chrome or Chromium, and
ffmpeg/ffprobe on PATH. The script prompts for your email and password.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from getpass import getpass
from pathlib import Path
from urllib.parse import urlsplit

from playwright.sync_api import Error as BrowserError
from playwright.sync_api import sync_playwright
from yt_dlp import YoutubeDL
from yt_dlp.utils import DownloadError


def replay_url(value):
    match = re.fullmatch(r"https://(?:www\.)?nahltv\.com/watch/([0-9]+)/?(?:[?#].*)?", value)
    if not match:
        raise argparse.ArgumentTypeError("Expected https://www.nahltv.com/watch/<game number>")
    return f"https://www.nahltv.com/watch/{match[1]}"


def sign_in(page, email):
    address = email or input("NAHLTV email: ").strip()
    password = os.environ.get("NAHLTV_PASSWORD") or getpass("NAHLTV password: ")
    try:
        page.locator("input[type=email]:visible").fill(address)
        box = page.locator("input[type=password]:visible")
        box.fill(password)
        box.press("Enter")
        page.wait_for_url(
            lambda u: urlsplit(u).path.rstrip("/") != "/sign-in",
            timeout=25000,
            wait_until="commit",
        )
    except BrowserError:
        raise RuntimeError("Sign-in did not complete. Check your NAHLTV login.") from None


def find_stream(url, email):
    """Read the HLS playlist that the signed-in replay player actually loads."""
    master = []
    single = []
    with sync_playwright() as playwright:
        chrome = (
            shutil.which("google-chrome")
            or shutil.which("chromium")
            or shutil.which("chromium-browser")
        )
        browser = playwright.chromium.launch(
            executable_path=chrome, args=["--autoplay-policy=no-user-gesture-required"]
        )
        page = browser.new_page(viewport={"width": 1920, "height": 1080})

        def collect(req):
            if page.url.split("?")[0].rstrip("/") != url:
                return
            response = req.response()
            if not response or response.status != 200:
                return
            if not (
                urlsplit(req.url).path.lower().endswith(".m3u8")
                or "mpegurl" in response.headers.get("content-type", "").lower()
            ):
                return
            playlist = response.text()
            if not playlist.lstrip().startswith("#EXTM3U"):
                return
            choices = master if "#EXT-X-STREAM-INF:" in playlist else single
            headers = {
                k: v
                for k, v in req.all_headers().items()
                if k in ("referer", "origin", "user-agent")
            }
            choices.append((req.url, headers))

        page.on("requestfinished", collect)
        page.goto(url, wait_until="domcontentloaded")
        password_box = page.locator("input[type=password]:visible")
        deadline = time.monotonic() + 70
        first_single = None
        tried_play = False
        signed_in = False
        while time.monotonic() < deadline:
            if master:
                source = master[0]
                break
            if single:
                if first_single is None:
                    first_single = time.monotonic()
                if time.monotonic() - first_single >= 5:
                    source = single[0]
                    break
            location = urlsplit(page.url)
            if (
                location.scheme == "https"
                and location.netloc == "www.nahltv.com"
                and location.path.rstrip("/") == "/sign-in"
                and password_box.is_visible()
            ):
                if signed_in:
                    raise RuntimeError("NAHLTV requested sign-in again. Check your account access.")
                sign_in(page, email)
                signed_in = True
                if urlsplit(page.url).path != urlsplit(url).path:
                    page.goto(url, wait_until="domcontentloaded")
                deadline = time.monotonic() + 70
            elif not tried_play and page.locator("video").count():
                page.locator("video").first.evaluate(
                    "v => { v.muted = true; v.play().catch(() => {}); }"
                )
                tried_play = True
            page.wait_for_timeout(300)
        else:
            raise RuntimeError(
                "No replay stream appeared. Check that this game plays in your account."
            )
        duration = page.locator("video").evaluate_all(
            "vs => vs.map(v => v.duration).find(d => Number.isFinite(d) && d > 0) || null"
        )
        playlist_url, headers = source
        return playlist_url, headers, duration


def download(url, email, destination):
    source, headers, player_duration = find_stream(url, email)
    with tempfile.TemporaryDirectory(prefix=".nahltv-", dir=destination.parent) as staging:
        previous = -1

        def progress(info):
            nonlocal previous
            count = info.get("fragment_count") or 0
            if count and info.get("status") == "downloading":
                percent = 100 * (info.get("fragment_index") or 0) // count
                if percent // 10 > previous:
                    previous = percent // 10
                    print(f"Downloaded {percent}%", flush=True)

        options = {
            "format": "bv*+ba/b",
            "format_sort": ["res", "fps", "br"],
            "http_headers": headers,
            "outtmpl": str(Path(staging) / "replay.%(ext)s"),
            "concurrent_fragment_downloads": 8,
            "fragment_retries": 10,
            "skip_unavailable_fragments": False,
            "hls_use_mpegts": False,
            "merge_output_format": "mp4",
            "postprocessors": [{"key": "FFmpegVideoRemuxer", "preferedformat": "mp4"}],
            "progress_hooks": [progress],
            "quiet": True,
            "noprogress": True,
        }
        with YoutubeDL(options) as downloader:
            info = downloader.extract_info(source, download=False)
            print(
                "Selected stream:",
                info.get("resolution") or "source resolution",
                f"({info['tbr']:.0f} kb/s)" if info.get("tbr") else "",
                flush=True,
            )
            downloader.process_info(info)

        video = Path(staging) / "replay.mp4"
        probe = subprocess.check_output(
            ["ffprobe", "-v", "error", "-show_format", "-show_streams", "-of", "json", str(video)],
            text=True,
        )
        meta = json.loads(probe)
        tracks = meta["streams"]
        picture = next((s for s in tracks if s["codec_type"] == "video"), None)
        if not picture or not any(s["codec_type"] == "audio" for s in tracks):
            raise RuntimeError("The downloaded file is missing its video or audio track.")
        if "mp4" not in meta["format"]["format_name"].split(","):
            raise RuntimeError("The downloaded file is not an MP4 container.")
        duration = float(meta["format"]["duration"])
        if player_duration and abs(duration - player_duration) > 3:
            raise RuntimeError(
                f"Duration mismatch: saved {duration:.1f}s, expected {player_duration:.1f}s."
            )
        # Publish only after validation. An existing recording is never overwritten.
        os.link(video, destination)
        print(
            f"Saved {destination} ({picture['width']}x{picture['height']}, {duration:.1f} seconds)"
        )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("url", type=replay_url)
    parser.add_argument(
        "--email", default=os.getenv("NAHLTV_EMAIL"), help="NAHLTV email (default: prompt)"
    )
    parser.add_argument(
        "-o", "--output", type=Path, help="output MP4 (default: NAHLTV-<game number>.mp4)"
    )
    args = parser.parse_args()
    dest = (args.output or Path(f"NAHLTV-{args.url.rsplit('/', 1)[1]}.mp4")).expanduser().absolute()
    if dest.suffix.lower() != ".mp4":
        parser.error("The output filename must end in .mp4")
    if dest.exists():
        parser.error(f"File already exists: {dest}")
    if not all(shutil.which(tool) for tool in ("ffmpeg", "ffprobe")):
        parser.error("Install ffmpeg and ffprobe first.")
    dest.parent.mkdir(parents=True, exist_ok=True)
    download(args.url, args.email, dest)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("Cancelled.")
    except (BrowserError, DownloadError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(f"Download failed: {exc}")
