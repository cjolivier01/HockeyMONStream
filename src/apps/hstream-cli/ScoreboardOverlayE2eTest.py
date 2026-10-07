#!/usr/bin/env python3
"""Manual hardware regression for Program scoreboard pixels across all output branches."""

import ctypes
import os
from pathlib import Path
import pwd
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

import cv2
import numpy as np
import yaml


def make_window():
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
    x11.XDefaultScreen.restype = ctypes.c_int
    x11.XRootWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XRootWindow.restype = ctypes.c_ulong
    x11.XCreateSimpleWindow.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong,
    ]
    x11.XCreateSimpleWindow.restype = ctypes.c_ulong
    x11.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XDestroyWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display:
        raise RuntimeError("X11 display is required (run with xvfb-run -a)")
    screen = x11.XDefaultScreen(display)
    window = x11.XCreateSimpleWindow(display, x11.XRootWindow(display, screen), 0, 0, 1600, 900, 0, 0, 0)
    x11.XMapWindow(display, window)
    x11.XSync(display, 0)
    return x11, display, window


def link_fixture(source, destination):
    destination.mkdir()
    shutil.copy2(source / "config.yaml", destination / "config.yaml")
    if (source / "hstream-ui").is_dir():
        shutil.copytree(source / "hstream-ui", destination / "hstream-ui")
    if (source / "player-frame-inputs").is_dir():
        shutil.copytree(source / "player-frame-inputs", destination / "player-frame-inputs")
    for name in ("cam1", "cam2", "autooptimiser_out.pto", "hm_project.pto", "left.png", "right.png",
                 "panorama.tif", "rink_mask_0.png", "s.png", "seam_file.png",
                 "stitching_canvas_provenance", "stitching_generation_id"):
        item = source / name
        if item.exists():
            if item.is_dir():
                (destination / name).symlink_to(item.resolve(), target_is_directory=True)
            else:
                shutil.copy2(item, destination / name)
    for item in source.glob("mapping_*.tif"):
        shutil.copy2(item, destination / item.name)
    for item in source.glob("*.MP4"):
        (destination / item.name).symlink_to(item.resolve())


def decode_frame(video, image):
    subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", "2", "-i", str(video), "-frames:v", "1", "-y", str(image)],
        check=True, timeout=45,
    )
    frame = cv2.imread(str(image))
    if frame is None:
        raise AssertionError(f"could not decode {video}")
    return frame


def scoreboard_reference(source_game, stitched_frame, output_width, output_height):
    with (source_game / "s.png").open("rb") as image:
        png_header = image.read(24)
    canvas_width, canvas_height = struct.unpack(">II", png_header[16:24])
    polygon = np.array(yaml.safe_load((source_game / "config.yaml").read_text())["rink"]["scoreboard"]["perspective_polygon"],
                       dtype=np.float32)
    horizontal = (np.linalg.norm(polygon[1] - polygon[0]) + np.linalg.norm(polygon[2] - polygon[3])) / 2
    vertical = (np.linalg.norm(polygon[3] - polygon[0]) + np.linalg.norm(polygon[2] - polygon[1])) / 2
    width = round(output_width * 0.10)
    height = round(output_height * 0.20)
    revised_width = int(height * horizontal / vertical)
    revised_height = int(width * vertical / horizontal)
    if abs(width - revised_width) / width < abs(height - revised_height) / height:
        width = revised_width
    else:
        height = revised_height
    polygon[:, 0] *= stitched_frame.shape[1] / canvas_width
    polygon[:, 1] *= stitched_frame.shape[0] / canvas_height
    target = np.array([[0, 0], [width - 1, 0], [width - 1, height - 1], [0, height - 1]], dtype=np.float32)
    return cv2.warpPerspective(stitched_frame, cv2.getPerspectiveTransform(polygon, target), (width, height))


def assert_scoreboard(name, frame, reference):
    if frame.shape[1] < reference.shape[1] or frame.shape[0] < reference.shape[0]:
        raise AssertionError(f"{name} is smaller than the expected scoreboard")
    # Ignore warp edges, where each encoder's chroma subsampling differs.
    margin_x = max(8, reference.shape[1] // 10)
    margin_y = max(8, reference.shape[0] // 10)
    area = np.s_[margin_y:reference.shape[0] - margin_y, margin_x:reference.shape[1] - margin_x]
    actual = frame[:reference.shape[0], :reference.shape[1]][area].astype(np.float32)
    expected = reference[area].astype(np.float32)
    error = float(np.abs(actual - expected).mean())
    print(f"{name}: scoreboard mean absolute RGB error {error:.1f}")
    if error >= 20:
        raise AssertionError(f"{name} does not contain the source scoreboard at the Program top-left")


def main():
    if len(sys.argv) != 4:
        raise SystemExit("expected hstream-cli, videoprep plugin, pipeline config")
    real_home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    source = Path(os.environ.get("HSTREAM_SCOREBOARD_FIXTURE_ROOT", str(real_home / "Videos/blackhawks-p2")))
    if not (source / "s.png").is_file() or not (source / "config.yaml").is_file():
        raise RuntimeError(f"calibrated scoreboard fixture is unavailable at {source}")
    root = Path(tempfile.mkdtemp(prefix="hstream-scoreboard-e2e-"))
    game_root, output_root = root / "games", root / "output"
    game_root.mkdir()
    output_root.mkdir()
    link_fixture(source, game_root / source.name)
    plugin_dir = root / "plugins"
    plugin_dir.mkdir()
    (plugin_dir / "libnvdsgst_videoprep.so").symlink_to(Path(sys.argv[2]).resolve())
    x11, display, window = make_window()
    preview = root / "program-preview.png"
    environment = os.environ.copy()
    environment.update({
        "HOME": str(real_home),
        "HM_GAME_DIR": str(game_root),
        "HM_OUTPUT_WORK_DIR": str(output_root),
        "GST_PLUGIN_PATH": str(plugin_dir),
        "HSTREAM_RUNTIME_ENV_READY": "1",
        "USE_NEW_NVSTREAMMUX": "yes",
    })
    command = [
        str(Path(sys.argv[1]).resolve()), "-g", source.name, "-c", str(Path(sys.argv[3]).resolve()),
        "--enable-sources=URI-MULTIPLE",
        "--enable-sinks=ENCODE_FILE,ENCODE_PROGRAM_4K_FILE,ENCODE_STITCHED_FILE",
        f"--ui-preview-windows=program:{window}", "--ui-preview-active=program",
        "--options=pipeline.hmaudio0.enable=0",
        "--start-time=00:05:50", "--time-limit=15",
    ]
    print("Running", " ".join(command), flush=True)
    process = subprocess.Popen(command, cwd=Path(sys.argv[3]).resolve().parents[1], env=environment,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1)
    lines = queue.Queue()
    output = []

    def read_output():
        for line in process.stdout:
            output.append(line)
            lines.put(line)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    capture_sent = False
    captured = False
    deadline = time.monotonic() + 180
    try:
        while time.monotonic() < deadline and (process.poll() is None or not lines.empty()):
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            if "HSTREAM_PREVIEW channel=program status=ready" in line and not capture_sent:
                process.stdin.write(f"@capture-preview-frame program {preview}\n")
                process.stdin.flush()
                capture_sent = True
            if f"runtime preview frame unavailable channel=program path={preview}" in line:
                capture_sent = False
            if f"runtime preview frame channel=program path={preview}" in line:
                captured = True
        if process.poll() is None:
            raise TimeoutError("pipeline did not finish in 180 seconds")
        if process.returncode != 0 or not captured:
            raise AssertionError(f"pipeline exit={process.returncode}, preview captured={captured}")
        files = list(output_root.rglob("*.mkv"))
        def get_file(prefix):
            matches = [path for path in files if path.name.startswith(prefix)]
            if len(matches) != 1:
                raise AssertionError(f"expected one {prefix} archive, found {matches}")
            return matches[0]
        stitched = decode_frame(get_file("stitched_output"), root / "stitched.png")
        program = decode_frame(get_file("tracking_output"), root / "program.png")
        program_4k = decode_frame(get_file("program_4k_output"), root / "program-4k.png")
        preview_image = cv2.imread(str(preview))
        if preview_image is None:
            raise AssertionError("Program GPU preview capture is missing")
        reference = scoreboard_reference(source, stitched, program.shape[1], program.shape[0])
        assert_scoreboard("Program archive", program, reference)
        assert_scoreboard("4K Program archive", program_4k,
                          cv2.resize(reference, (round(reference.shape[1] * program_4k.shape[1] / program.shape[1]),
                                                 round(reference.shape[0] * program_4k.shape[0] / program.shape[0]))))
        assert_scoreboard("Program GPU preview", preview_image,
                          cv2.resize(reference, (round(reference.shape[1] * preview_image.shape[1] / program.shape[1]),
                                                 round(reference.shape[0] * preview_image.shape[0] / program.shape[0]))))
        print("Scoreboard visible in both Program archives and the GPU preview")
    except Exception:
        print("Fixture and output retained at", root, file=sys.stderr)
        print("".join(output[-100:]), file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        x11.XDestroyWindow(display, window)
        x11.XCloseDisplay(display)


if __name__ == "__main__":
    main()
