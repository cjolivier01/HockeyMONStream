#!/usr/bin/env python3
"""Qualify real chapter replacement under GPU memory pressure.

Requires an already calibrated two-camera recording and ffprobe. Each case gets
an isolated game/config/output tree. Video files are symlinked, other inputs are
copied, and cases run sequentially so they do not compete for GPU memory.
"""

import argparse
from fractions import Fraction
import itertools
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time


def check_output(log, archive, duration, require_handoffs=True):
  if "App run successful" not in log:
    raise ValueError("pipeline did not report successful completion")
  if re.search(r"(?:FATAL:|ERROR from |GStreamer ERROR|CUDA_ERROR|out of memory)", log, re.I):
    raise ValueError("pipeline reported an error or watchdog")
  handoffs = set(re.findall(r"HSTREAM_URI_PLAYLIST_HANDOFF source=(\d+)", log))
  if require_handoffs and not {"0", "1"}.issubset(handoffs):
    raise ValueError("both cameras must cross a physical chapter boundary")
  probe = json.loads(subprocess.check_output([
      "ffprobe", "-v", "error", "-show_streams", "-show_packets", "-show_entries",
      "stream=index,codec_type,width,height,r_frame_rate:packet=stream_index,pts_time,duration_time",
      "-of", "json", str(archive)], text=True, timeout=60))
  streams = probe.get("streams", [])
  video = next((s for s in streams if s["codec_type"] == "video"), None)
  audio = next((s for s in streams if s["codec_type"] == "audio"), None)
  if not video or not audio or (video.get("width"), video.get("height")) != (3840, 2160):
    raise ValueError("expected Program 4K video and an audio stream")
  fps = float(Fraction(video["r_frame_rate"]))
  if fps <= 0:
    raise ValueError("invalid output frame rate")
  packets = probe.get("packets", [])
  def timestamps(stream):
    return sorted(float(p["pts_time"]) for p in packets
                  if p["stream_index"] == stream["index"] and "pts_time" in p)
  pts = timestamps(video)
  audio_pts = timestamps(audio)
  video_packets = [p for p in packets if p["stream_index"] == video["index"]]
  if len(pts) != len(video_packets) or any(not math.isfinite(p) for p in pts + audio_pts):
    raise ValueError("packets lack finite presentation timestamps")
  if len(pts) < math.floor(duration * fps) or len(pts) > math.ceil((duration + 0.25) * fps) + 2:
    raise ValueError(f"unexpected frame count: {len(pts)} for {duration}s at {fps}fps")
  if abs(pts[0]) > 0.05:
    raise ValueError("video does not start at the rebased archive origin")
  audio_packets = sorted((p for p in packets if p["stream_index"] == audio["index"]),
                         key=lambda p: float(p.get("pts_time", "nan")))
  if len(audio_pts) < 2 or len(audio_pts) != len(audio_packets):
    raise ValueError("audio packets lack a complete timestamped timeline")
  if abs(audio_pts[0] - pts[0]) > 0.05:
    raise ValueError("leading audio is missing")
  audio_duration = [float(p.get("duration_time", "0")) for p in audio_packets]
  if any(not math.isfinite(d) or d <= 0 or d > 0.05 for d in audio_duration):
    raise ValueError("audio packet durations are missing or invalid")
  audio_steps = [b - a for a, b in zip(audio_pts, audio_pts[1:])]
  if min(audio_steps) <= 0 or any(abs(step - d) > 0.002 for step, d in zip(audio_steps, audio_duration)):
    raise ValueError("audio contains missing, duplicate or overlapping packets")
  if abs(audio_pts[-1] + audio_duration[-1] - pts[-1] - 1 / fps) > 0.1:
    raise ValueError("audio did not cover the completed video timeline")
  # Matroska timestamps are quantized to milliseconds. Reject duplicates and
  # missing frames, rather than accepting a successful exit with truncated video.
  gaps = [b - a for a, b in zip(pts, pts[1:])]
  if not gaps or min(gaps) <= 0 or max(abs(g - 1 / fps) for g in gaps) > 0.0015:
    raise ValueError("video timestamps contain duplicate or missing frames")
  span = pts[-1] - pts[0] + 1 / fps
  if not duration <= span <= duration + 0.25:
    raise ValueError(f"completed video duration {span} does not match {duration}")
  return dict(frames=len(pts), video_seconds=span, fps=fps, handoffs=sorted(handoffs))


def copy_input(source, destination):
  source = Path(source).resolve()
  destination = Path(destination)
  if source.suffix.lower() in (".mp4", ".mkv", ".mov", ".avi"):
    destination.symlink_to(source)
  else:
    subprocess.run(["cp", "--preserve=mode,timestamps", "--reflink=auto", "--", str(source), str(destination)], check=True)
  return str(destination)


def run_case(args, root, name, profile, extra, latency):
  case = root / name
  case.mkdir()
  game = case / "games" / args.game_dir.name
  shutil.copytree(args.game_dir, game, copy_function=copy_input,
                  ignore=shutil.ignore_patterns("*.lock", "hstream_telemetry*.db*", "hstream-runtime"))
  env = dict(os.environ, HM_GAME_DIR=str(game.parent), HM_OUTPUT_WORK_DIR=str(case / "output"),
             HM_URI_PLAYLIST_BARRIER_STALL_SECONDS="15", HSTREAM_RENDER_AUDIO_MUTED="1")
  # A game may contain an absolute archive override. Keep the bundled Program
  # 4K sink inside this case's output root rather than touching that archive.
  options = [f"runtime.gpu_memory_profile={profile}", "pipeline.hmaudio.enable=1",
             "pipeline.sink6.output-file=program_4k_output.mkv"]
  for source in (0, 1):
    if extra != "default":
      options.append(f"pipeline.source{source}.num-extra-surfaces={extra}")
    if latency != "default":
      options.append(f"pipeline.source{source}.low-latency-mode={latency}")
  options.extend(args.options)
  command = [str(args.cli), "-g", game.name, f"--start-time={args.start_time}",
             f"-t={args.duration}", "--enable-sources=URI-MULTIPLE",
             "--enable-sinks=RENDER,ENCODE_PROGRAM_4K_FILE", "--options=" + ",".join(options)]
  if not args.native_render:
    command.append("--headless-render-video")
  (case / "command.json").write_text(json.dumps(command, indent=2) + "\n")
  start = time.monotonic()
  peak = 0
  timed_out = False
  with (case / "pipeline.log").open("w") as output:
    process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, start_new_session=True)
    try:
      while process.poll() is None:
        if time.monotonic() - start >= args.timeout:
          timed_out = True
          os.killpg(process.pid, signal.SIGCONT)
          os.killpg(process.pid, signal.SIGINT)
          try:
            process.wait(timeout=20)
          except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
          break
        try:
          usage = subprocess.check_output([
              "nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits"],
              text=True, timeout=3)
          for row in usage.splitlines():
            pid, memory = row.split(",")
            if int(pid) == process.pid:
              peak = max(peak, int(memory))
        except (OSError, ValueError, subprocess.SubprocessError):
          pass
        time.sleep(0.5)
    finally:
      if process.poll() is None:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
  result = dict(case=name, exit_code=process.returncode, timed_out=timed_out,
                elapsed_seconds=round(time.monotonic() - start, 2), peak_vram_mib=peak)
  try:
    if timed_out or process.returncode != 0:
      raise ValueError("external timeout" if timed_out else f"runner exited {process.returncode}")
    log = (case / "pipeline.log").read_text(errors="replace")
    archive = case / "output" / game.name / "program_4k_output-with-audio.mkv"
    result.update(check_output(log, archive, args.duration))
    result["passed"] = True
  except (ValueError, OSError, subprocess.SubprocessError) as error:
    result.update(passed=False, failure=str(error))
  (case / "result.json").write_text(json.dumps(result, indent=2) + "\n")
  print(json.dumps(result), flush=True)
  return result


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--game-dir", type=Path, required=True)
  parser.add_argument("--artifacts", type=Path, required=True, help="new directory; existing results are never replaced")
  parser.add_argument("--cli", type=Path, default=Path("bazel-bin/src/apps/hstream-cli/hstream-cli"))
  parser.add_argument("--start-time", required=True, help="HH:MM:SS before the chapter boundary")
  parser.add_argument("--duration", type=int, default=15)
  parser.add_argument("--timeout", type=float, default=240)
  parser.add_argument("--profiles", default="low", help="comma-separated auto,low,standard")
  parser.add_argument("--extra-surfaces", default="0,1,2", help="comma-separated nonnegative counts or default")
  parser.add_argument("--low-latency", default="default,0", help="comma-separated default,0,1")
  parser.add_argument("--native-render", action="store_true", help="also exercise the native display sink (requires DISPLAY)")
  parser.add_argument("--options", action="append", default=[], help="additional native leaf override(s)")
  args = parser.parse_args()
  args.cli = args.cli.resolve(strict=True)
  args.game_dir = args.game_dir.resolve(strict=True)
  args.artifacts = args.artifacts.resolve()
  if args.duration <= 0 or args.timeout <= 0:
    parser.error("duration and timeout must be positive")
  profiles, counts, latency = args.profiles.split(","), args.extra_surfaces.split(","), args.low_latency.split(",")
  if any(p not in ("auto", "low", "standard") for p in profiles):
    parser.error("invalid memory profile")
  if any(c != "default" and not c.isdecimal() for c in counts):
    parser.error("invalid decoder surface count")
  if any(v not in ("default", "0", "1") for v in latency):
    parser.error("invalid latency setting")
  args.artifacts.mkdir(parents=True, exist_ok=False)
  # Record the actual input workload and GPU. Output size alone cannot prove
  # the 8K/Main10 allocation-pressure case was exercised on a given machine.
  media = {}
  for path in sorted(args.game_dir.rglob("*")):
    if not path.is_file() or path.suffix.lower() != ".mp4":
      continue
    media[str(path.relative_to(args.game_dir))] = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=codec_name,profile,width,height,pix_fmt,r_frame_rate", "-of", "json", str(path)],
        text=True, timeout=30))
  gpu = subprocess.check_output([
      "nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader"], text=True, timeout=10)
  (args.artifacts / "workload.json").write_text(json.dumps(dict(inputs=media, gpu=gpu), indent=2) + "\n")
  results = []
  for profile, extra, mode in itertools.product(profiles, counts, latency):
    name = f"{profile}-extra{extra}-latency{mode}"
    results.append(run_case(args, args.artifacts, name, profile, extra, mode))
  (args.artifacts / "results.json").write_text(json.dumps(results, indent=2) + "\n")
  return 0 if all(r["passed"] for r in results) else 1


if __name__ == "__main__":
  raise SystemExit(main())
