"""Opt-in real-camera AMD Stop/abort and queued-seek qualification.

Requires HSTREAM_AMD_TEST_GAME_ID and HSTREAM_AMD_TEST_START_SECONDS for a
saved, unrotated calibrated game with at least two seconds remaining, plus
ffprobe/ffmpeg. Video readback here is solely an offline output verifier.
"""

import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


def verify_clip(path, frames):
    probe = subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(path)],
        text=True, timeout=20,
    )
    streams = json.loads(probe)["streams"]
    video = next(stream for stream in streams if stream["codec_type"] == "video")
    audio = next(stream for stream in streams if stream["codec_type"] == "audio")
    assert int(video["nb_frames"]) == frames, (video, frames)
    assert float(video["start_time"]) == float(audio["start_time"]) == 0, streams
    assert abs(float(video["duration"]) - float(audio["duration"])) < (
        1 / int(audio["sample_rate"]) + 0.000001
    ), streams
    subprocess.run(
        ["ffmpeg", "-v", "error", "-xerror", "-i", str(path), "-f", "null", "-"],
        stdout=subprocess.DEVNULL, check=True, timeout=30,
    )


def run_case(cli, game, start, directory, case):
    path = directory / (case + ".mp4")
    seek = case == "queued_end_seek"
    args = [
        cli, "-g", game, "--enable-sources=URI-MULTIPLE",
        "--enable-sinks=" + ("FAKE" if seek else "ENCODE_FILE"),
        "--start-time=" + start,
        "--options=pipeline.primary-gie.config-file=config_infer_yolov8_hockey_fp16.yaml",
        "--options=video_out.output_video_path=" + str(path),
    ]
    if case == "limit_one":
        args.append("-t=0.001")
    env = os.environ.copy()
    for name in ["HSTREAM_UI_PREVIEW_OVERLAYS", "HSTREAM_JOB_MANIFEST", "HSTREAM_UI_PARENT_PID"]:
        env.pop(name, None)
    process = subprocess.Popen(
        args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1,
    )
    lines = queue.Queue()

    def read():
        for line in process.stdout:
            lines.put(line)
        lines.put(None)

    reader = threading.Thread(target=read, daemon=True)
    reader.start()

    def send(command):
        process.stdin.write(command + "\n")
        process.stdin.flush()

    frames = 0
    acted = False
    pause_acknowledged = False
    seek_ok = False
    started = time.monotonic()
    log = []
    try:
        if seek:
            send("@seek-relative 9223372036854775807 1")
        elif case == "queued_quit":
            send("@quit")
        while time.monotonic() - started < 150:
            try:
                line = lines.get(timeout=0.1)
            except queue.Empty:
                continue
            if line is None:
                break
            log.append(line)
            seek_ok |= "HSTREAM_SEEK status=ok generation=1" in line
            if line.startswith("HSTREAM_AMD_FRAME "):
                frames += 1
                if not acted and frames >= (1 if case == "quit_one" else 20):
                    acted = True
                    if case == "quit_one":
                        send("@quit")
                    elif case == "sigint":
                        process.send_signal(signal.SIGINT)
                    elif case == "sigterm":
                        process.terminate()
                    elif case == "abort_then_stop":
                        os.kill(process.pid, signal.SIGSTOP)
                        os.waitpid(process.pid, os.WUNTRACED)
                        process.terminate()
                        process.send_signal(signal.SIGINT)
                        os.kill(process.pid, signal.SIGCONT)
                    elif case.startswith("paused_"):
                        send("@process-pause 9")
            if "HSTREAM_PROCESS_PAUSE status=prepared generation=9" in line:
                pause_acknowledged = True
                if case == "paused_quit":
                    send("@quit")
                elif case == "paused_eof":
                    process.stdin.close()
                elif case == "paused_sigint":
                    os.kill(process.pid, signal.SIGSTOP)
                    os.waitpid(process.pid, os.WUNTRACED)
                    os.kill(process.pid, signal.SIGCONT)
                    process.send_signal(signal.SIGINT)
        code = process.wait(timeout=5)
        expected = {"sigterm": 143, "abort_then_stop": 143, "paused_eof": 1, "queued_quit": 130}.get(case, 0)
        assert code == expected, (case, code, expected)
        if case not in ("queued_quit", "queued_end_seek", "limit_one"):
            assert acted, (case, "control request was never sent")
        if case.startswith("paused_"):
            assert pause_acknowledged, (case, "pause was never acknowledged")
        published = code == 0 and not seek
        assert path.exists() == published, (case, "publication")
        if published:
            assert frames > 0, (case, "no completed frames")
            if case == "limit_one":
                assert frames == 1, (case, frames)
            verify_clip(path, frames)
        if case in ("queued_quit", "queued_end_seek"):
            assert frames == 0, (case, "consumed a frame", frames)
        if seek:
            assert seek_ok, "Startup seek was not applied"
        print(json.dumps(dict(case=case, exit=code, frames=frames, published=published)), flush=True)
    except BaseException:
        print("".join(log), file=sys.stderr)
        raise
    finally:
        if process.poll() is None:
            os.kill(process.pid, signal.SIGCONT)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        reader.join(timeout=2)
        process.stdout.close()
        if not process.stdin.closed:
            process.stdin.close()


if __name__ == "__main__":
    game = os.environ["HSTREAM_AMD_TEST_GAME_ID"]
    start = os.environ["HSTREAM_AMD_TEST_START_SECONDS"]
    with tempfile.TemporaryDirectory(prefix="amd-program-lifecycle-") as directory:
        for case in [
            "queued_end_seek", "limit_one", "quit_one", "sigint", "paused_quit",
            "paused_sigint", "sigterm", "abort_then_stop", "paused_eof", "queued_quit",
        ]:
            run_case(os.path.abspath(sys.argv[1]), game, start, Path(directory), case)
