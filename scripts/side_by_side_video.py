#!/usr/bin/env python3
"""Make an aspect-preserving, synchronized side-by-side MP4 with FFmpeg.

Examples:
  side_by_side_video.py left.mp4 right.mp4 comparison.mp4
  side_by_side_video.py left.mp4 right.mp4 clip.mp4 --start 00:10:00 --duration 30

Both inputs start at the same requested playback time. Already synchronized
constant-frame-rate recordings retain frame pairing. Different/variable frame
rates are sampled on the left video's frame clock (override with --fps).
This does not discover timing offsets between independently recorded videos.
"""

import argparse
from dataclasses import dataclass
from fractions import Fraction
import json
import math
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


@dataclass(frozen=True)
class Video:
  width: float
  height: float
  fps: Fraction
  audio: bool
  duration: float | None


def time_seconds(value: str) -> float:
  """Accept seconds, MM:SS or HH:MM:SS, including fractional seconds."""
  try:
    parts = value.split(":")
    if not 1 <= len(parts) <= 3:
      raise ValueError()
    numbers = [float(part) for part in parts]
    if any(not math.isfinite(part) or part < 0 for part in numbers):
      raise ValueError()
    if len(parts) > 1 and (any(part >= 60 for part in numbers[1:]) or
                           any(not part.is_integer() for part in numbers[:-1])):
      raise ValueError()
    return sum(part * 60 ** index for index, part in enumerate(reversed(numbers)))
  except ValueError:
    raise argparse.ArgumentTypeError("use nonnegative seconds, MM:SS or HH:MM:SS") from None


def frame_rate(value: str) -> Fraction:
  try:
    result = Fraction(value)
    if not 0 < result <= 240:
      raise ValueError()
    return result
  except (ValueError, ZeroDivisionError):
    raise argparse.ArgumentTypeError("frame rate must be greater than 0 and at most 240 (e.g. 30 or 30000/1001)") from None


def max_size(value: str) -> tuple[int, int]:
  try:
    width, height = (int(part) for part in value.lower().split("x"))
    if not (4 <= width <= 7680 and 2 <= height <= 4320):
      raise ValueError()
    return width, height
  except ValueError:
    raise argparse.ArgumentTypeError("use WIDTHxHEIGHT, from 4x2 through 7680x4320") from None


def probe(path: Path) -> Video:
  result = subprocess.run(
      ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
      check=True, capture_output=True, text=True)
  data = json.loads(result.stdout)
  # Match FFmpeg's V:0 selector: skip attached cover art.
  streams = data.get("streams", [])
  video = next((stream for stream in streams if stream.get("codec_type") == "video" and
                not stream.get("disposition", {}).get("attached_pic", 0)), None)
  if video is None:
    raise ValueError(f"No video stream in {path}")
  width, height = float(video["width"]), float(video["height"])
  sar = video.get("sample_aspect_ratio", "1:1")
  if sar not in ("N/A", "0:1"):
    width *= float(Fraction(sar.replace(":", "/")))
  rotation = float(video.get("tags", {}).get("rotate", 0))
  for side_data in video.get("side_data_list", []):
    if "rotation" in side_data:
      rotation = float(side_data["rotation"])
  if not math.isclose(rotation % 90, 0, abs_tol=0.01):
    raise ValueError(f"Unsupported non-right-angle rotation in {path}: {rotation}")
  if round(rotation / 90) % 2:
    width, height = height, width
  if not (math.isfinite(width) and math.isfinite(height) and width >= 2 and height >= 2):
    raise ValueError(f"Invalid video dimensions in {path}")
  fps = Fraction(0)
  for key in ("avg_frame_rate", "r_frame_rate"):
    try:
      fps = Fraction(video.get(key, "0/1"))
    except (ValueError, ZeroDivisionError):
      continue
    if fps > 0:
      break
  if fps <= 0:
    raise ValueError(f"Cannot determine frame rate for {path}")
  duration_text = video.get("duration", data.get("format", {}).get("duration"))
  duration = float(duration_text) if duration_text not in (None, "N/A") else None
  return Video(width, height, fps, any(stream.get("codec_type") == "audio" for stream in streams), duration)


def layout(videos: list[Video], bounds: tuple[int, int]) -> tuple[int, int, list[tuple[int, int]]]:
  """Equal panes; shrink the complete canvas to fit the cap; never enlarge inputs."""
  width = max(video.width for video in videos)
  height = max(video.height for video in videos)
  scale = min(1.0, bounds[0] / (2 * width), bounds[1] / height)
  pane_width = max(2, 2 * math.floor(width * scale / 2))
  pane_height = max(2, 2 * math.floor(height * scale / 2))
  sizes = []
  for video in videos:
    fit = min(1.0, pane_width / video.width, pane_height / video.height)
    sizes.append((max(2, 2 * math.floor(video.width * fit / 2)),
                  max(2, 2 * math.floor(video.height * fit / 2))))
  return pane_width, pane_height, sizes


def parse_args(argv=None):
  parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument("left", type=Path)
  parser.add_argument("right", type=Path)
  parser.add_argument("output", type=Path, help="Output .mp4 (existing files are protected by default)")
  parser.add_argument("--start", type=time_seconds, default=0.0, help="Start in both inputs; seconds or HH:MM:SS (default: 0)")
  parser.add_argument("--duration", type=time_seconds, help="Output length; seconds or HH:MM:SS (default: until the shorter input ends)")
  parser.add_argument("--max-size", type=max_size, default=(7680, 4320), help="Maximum canvas WIDTHxHEIGHT (default: 7680x4320); no upscaling")
  parser.add_argument("--fps", type=frame_rate, help="Output frame rate (default: left input); mismatched rates duplicate/drop frames")
  parser.add_argument("--audio", choices=("left", "right", "none"), default="left", help="Audio source (default: left; silent if it has no audio)")
  parser.add_argument("--encoder", choices=("auto", "nvenc", "cpu"), default="auto",
                      help="auto tries NVIDIA HEVC, otherwise CPU H.264; nvenc requires a usable NVIDIA encoder")
  parser.add_argument("--quality", "--crf", type=int, default=18, choices=range(0, 52), metavar="0..51",
                      help="Quality target, lower is better/larger (default: 18); CPU CRF or NVENC CQ (0 means automatic for NVENC)")
  parser.add_argument("--preset", help="Encoding speed/compression preset (default: p2 for NVENC, veryfast for CPU)")
  parser.add_argument("--overwrite", action="store_true", help="Allow replacing an existing output")
  parser.add_argument("--dry-run", action="store_true", help="Probe inputs and print the FFmpeg command without encoding")
  args = parser.parse_args(argv)
  if args.duration is not None and args.duration <= 0:
    parser.error("--duration must be greater than zero")
  return args


def encoder_options(args, width: int, height: int, fps: Fraction) -> tuple[str, list[str]]:
  cpu_presets = ("ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow")
  nvenc_presets = tuple(f"p{index}" for index in range(1, 8))
  # An explicit CPU preset also makes auto choose CPU, so --preset ultrafast
  # works consistently on machines with and without NVIDIA drivers.
  try_nvenc = args.encoder != "cpu" and not (args.encoder == "auto" and args.preset in cpu_presets)
  if try_nvenc:
    preset = args.preset or "p2"
    if preset not in nvenc_presets:
      raise ValueError("NVENC presets are p1 (fastest) through p7 (slowest)")
    options = ["-c:v", "hevc_nvenc", "-preset", preset, "-rc", "vbr", "-cq", str(args.quality), "-b:v", "0"]
    # Encoder listings do not prove a usable driver or support for this size.
    # Probe one synthetic frame at the actual output size before opening output.
    try:
      result = subprocess.run(
          ["ffmpeg", "-v", "error", "-nostdin", "-f", "lavfi", "-i",
           f"color=black:s={width}x{height}:r={fps}", "-frames:v", "1", *options,
           "-pix_fmt", "yuv420p", "-f", "null", "-"],
          capture_output=True, text=True, timeout=30)
      error = (result.stderr.strip() or f"FFmpeg exited with status {result.returncode}") if result.returncode else ""
    except subprocess.TimeoutExpired:
      error = "NVIDIA encoder initialization timed out"
    if not error:
      return "NVIDIA HEVC", options + ["-tag:v", "hvc1"]
    if args.encoder == "nvenc":
      raise ValueError(f"NVIDIA encoding is unavailable for {width}x{height}: {error}")
    print(f"NVIDIA encoding unavailable; using CPU H.264. {error.splitlines()[0]}", flush=True)
  preset = args.preset or "veryfast"
  if preset not in cpu_presets:
    if args.encoder == "auto" and preset in nvenc_presets:
      preset = "veryfast"
    else:
      raise ValueError("CPU presets: " + ", ".join(cpu_presets))
  return "CPU H.264", ["-c:v", "libx264", "-crf", str(args.quality), "-preset", preset]


def run(args) -> None:
  for program in ("ffmpeg", "ffprobe"):
    if not shutil.which(program):
      raise ValueError(f"{program} is required; install FFmpeg and put it on PATH")
  inputs = [args.left.resolve(), args.right.resolve()]
  output = args.output.absolute()
  for path in inputs:
    if not path.is_file():
      raise ValueError(f"Input does not exist: {path}")
    if output.resolve() == path or (output.exists() and output.samefile(path)):
      raise ValueError("Output must be different from both inputs, even with --overwrite")
  if output.suffix.lower() != ".mp4":
    raise ValueError("Output filename must end in .mp4")
  if output.exists() and not args.overwrite:
    raise ValueError(f"Output already exists: {output}; use --overwrite to replace it")
  if not output.parent.is_dir():
    raise ValueError(f"Output directory does not exist: {output.parent}")
  videos = [probe(path) for path in inputs]
  for path, video in zip(inputs, videos):
    if video.duration is not None and args.start >= video.duration:
      raise ValueError(f"--start is at or beyond the end of {path} ({video.duration:g} seconds)")
  fps = args.fps or videos[0].fps
  pane_width, pane_height, sizes = layout(videos, args.max_size)
  encoder, encoding_options = encoder_options(args, pane_width * 2, pane_height, fps)
  command = ["ffmpeg", "-hide_banner", "-nostdin", "-y" if args.overwrite else "-n"]
  for path in inputs:
    command += ["-ss", str(args.start), "-i", str(path)]
  filters = []
  for index, (width, height) in enumerate(sizes):
    filters.append(
        f"[{index}:V:0]setpts=PTS-STARTPTS,fps={fps}:start_time=0,"
        f"scale={width}:{height}:flags=lanczos,setsar=1,"
        f"pad={pane_width}:{pane_height}:(ow-iw)/2:(oh-ih)/2:color=black[v{index}]")
  filters.append("[v0][v1]hstack=inputs=2:shortest=1[v]")
  audio_index = 0 if args.audio == "left" else 1
  has_audio = args.audio != "none" and videos[audio_index].audio
  if has_audio:
    # Retain the source's audio timing relative to the requested seek. Padding
    # avoids truncating the video when the selected audio stream ends early.
    filters.append(f"[{audio_index}:a:0]aresample=async=1:first_pts=0,apad[a]")
  command += ["-filter_complex", ";".join(filters), "-map", "[v]"]
  if has_audio:
    command += ["-map", "[a]", "-c:a", "aac", "-b:a", "192k", "-shortest"]
  else:
    command += ["-an"]
  if args.duration is not None:
    command += ["-t", str(args.duration)]
  command += encoding_options + ["-pix_fmt", "yuv420p", "-movflags", "+faststart", str(output)]
  print(f"Output: {pane_width * 2}x{pane_height}, {fps} fps; {encoder}; quality {args.quality}; "
        f"audio: {args.audio if has_audio else 'none'}", flush=True)
  print("Timing: align the first retained frames; stop at the shorter video or requested duration.", flush=True)
  if any(video.fps != fps for video in videos):
    print("Frame rates differ: frames will be duplicated/dropped to preserve playback speed.", flush=True)
  if args.dry_run:
    print(shlex.join(command))
  else:
    subprocess.run(command, check=True)


def main(argv=None) -> int:
  args = parse_args(argv)
  try:
    run(args)
    return 0
  except (ValueError, ZeroDivisionError, OSError, subprocess.CalledProcessError) as error:
    detail = error.stderr if isinstance(error, subprocess.CalledProcessError) and error.stderr else str(error)
    print(f"Error: {detail}", file=sys.stderr)
    return 1
  except KeyboardInterrupt:
    return 130


if __name__ == "__main__":
  sys.exit(main())
