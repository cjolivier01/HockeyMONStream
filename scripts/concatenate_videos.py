#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any, Dict, List

import yaml


def parse_config(config_path: str) -> Dict[str, Any]:
    """Parse the YAML configuration file."""
    config_file = pathlib.Path(config_path)
    if not config_file.is_file():
        raise FileNotFoundError(f"Config file '{config_path}' not found.")
    try:
        with config_file.open("r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
    except yaml.YAMLError as e:
        raise ValueError(f"Error parsing YAML file '{config_path}': {e}")
    return data


def get_video_list(data: Dict[str, Any], side: str) -> List[str]:
    """
    Retrieve the list of video file paths for a given side ('left' or 'right').
    Expects the YAML structure to contain: game -> videos -> side.
    """
    try:
        videos = data["game"]["videos"][side]
    except KeyError as e:
        raise KeyError(f"Missing expected key in YAML for '{side}' videos: {e}")
    if isinstance(videos, str):
        return [videos]
    if not isinstance(videos, list):
        raise TypeError(f"Expected a list for '{side}' videos, got {type(videos)}.")
    return videos


def create_concat_file(video_files: List[str], base_dir: pathlib.Path) -> str:
    """
    Create a temporary file containing the FFmpeg concat demuxer list.
    Each line is of the form: file '/absolute/path/to/video'
    """
    temp = tempfile.NamedTemporaryFile(
        delete=False, mode="w", encoding="utf-8", suffix=".txt"
    )
    try:
        for vf in video_files:
            video_path = (base_dir / vf).resolve()
            if not video_path.is_file():
                raise FileNotFoundError(f"Video file '{video_path}' not found.")
            temp.write(f"file '{video_path.as_posix()}'\n")
    finally:
        temp.close()
    return temp.name


def get_video_duration(video_path: pathlib.Path) -> float:
    """
    Use ffprobe to get the duration of the video in seconds.
    """
    command = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(video_path),
    ]
    try:
        result = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        duration_str = result.stdout.strip()
        return float(duration_str)
    except Exception as e:
        raise RuntimeError(f"Failed to get duration for '{video_path}': {e}")


def run_ffmpeg_concat(input_list_file: str, output_file: str) -> None:
    """
    Run FFmpeg to concatenate videos using the concat demuxer with stream copy.
    FFmpeg’s output is printed directly to stdout.
    """
    command = [
        "ffmpeg",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        input_list_file,
        "-c:a",
        "copy",
        "-c:v",
        "copy",
        "-y",  # Overwrite output if necessary.
        output_file,
    ]
    print("Running FFmpeg command:")
    print(" ".join(command))
    result = subprocess.run(command)
    if result.returncode != 0:
        raise RuntimeError(
            f"ffmpeg command failed with return code {result.returncode}"
        )


def process_side(
    side: str, data: Dict[str, Any], base_dir: pathlib.Path, force: bool
) -> None:
    """
    Process one side ('left' or 'right'): if the output file does not exist or
    if force is True, compute the total duration then concatenate the videos
    listed in the YAML file into an output file (side.mp4) using FFmpeg.
    """
    output_path = base_dir / f"{side}.mp4"
    if output_path.exists():
        if force:
            print(f"Overwriting existing file '{output_path}' (--force specified).")
            output_path.unlink()
        else:
            print(
                f"Output file '{output_path}' already exists; skipping {side} concatenation."
            )
            return

    video_list = get_video_list(data, side)
    if not video_list:
        raise ValueError(f"No videos specified for side '{side}' in config.")

    # Compute total duration (for error checking, if needed)
    total_duration: float = 0.0
    for vf in video_list:
        video_path = (base_dir / vf).resolve()
        duration = get_video_duration(video_path)
        total_duration += duration
    if total_duration <= 0.0:
        raise RuntimeError(f"Total duration for {side} videos is non-positive.")

    concat_list_file = create_concat_file(video_list, base_dir)
    try:
        print(f"Starting FFmpeg concatenation for '{side}'...")
        run_ffmpeg_concat(concat_list_file, str(output_path))
        print(f"Successfully created '{output_path}'.")
    finally:
        os.remove(concat_list_file)


def main(game_id: str, force: bool, parallel: bool) -> None:
    home_dir = os.environ.get("HOME")
    if home_dir is None:
        raise EnvironmentError("HOME environment variable is not set.")
    config_path = os.path.join(home_dir, "Videos", game_id, "config.yaml")
    base_dir = pathlib.Path(config_path).parent.resolve()
    data = parse_config(config_path)

    if parallel:
        # Launch left and right concatenations in parallel.
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = {
                executor.submit(process_side, "left", data, base_dir, force): "left",
                executor.submit(process_side, "right", data, base_dir, force): "right",
            }
            for future in concurrent.futures.as_completed(futures):
                side = futures[future]
                try:
                    future.result()
                except Exception as exc:
                    print(f"Error processing {side} videos: {exc}", file=sys.stderr)
    else:
        # Process left and right sequentially.
        try:
            process_side("left", data, base_dir, force)
        except Exception as exc:
            print(f"Error processing left videos: {exc}", file=sys.stderr)
        try:
            process_side("right", data, base_dir, force)
        except Exception as exc:
            print(f"Error processing right videos: {exc}", file=sys.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Concatenate left and right MP4 files as defined in a YAML config."
    )
    parser.add_argument(
        "--game-id",
        required=True,
        help="Game identifier used to locate the config file in $HOME/Videos/<game-id>/config.yaml",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing left.mp4 and right.mp4 if they exist.",
    )
    parser.add_argument(
        "--no-parallel",
        action="store_true",
        help="Run concatenations sequentially rather than in parallel.",
    )
    args = parser.parse_args()
    try:
        main(args.game_id, args.force, parallel=not args.no_parallel)
    except Exception as e:
        print(f"Fatal error: {e}", file=sys.stderr)
        sys.exit(1)
