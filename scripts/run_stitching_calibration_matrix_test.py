#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock

import yaml

try:
  import run_stitching_calibration_matrix as matrix
except ModuleNotFoundError:
  from scripts import run_stitching_calibration_matrix as matrix


class IsolatedGameTest(unittest.TestCase):
  def setUp(self) -> None:
    self.temporary = tempfile.TemporaryDirectory(prefix="matrix-isolation-test-")
    self.addCleanup(self.temporary.cleanup)
    self.root = Path(self.temporary.name)
    self.source = self.root / "source-game"
    self.source.mkdir()
    self.config = self.source / "config.yaml"

  def touch(self, relative: str, contents: bytes = b"input") -> Path:
    path = self.source / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)
    return path

  def isolate(self, config: dict[str, object]) -> tuple[Path, Path]:
    self.config.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
    work_root, game_id = matrix.create_isolated_game(self.source, self.config)
    self.addCleanup(lambda: matrix.remove_work_root(work_root) if work_root.exists() else None)
    return work_root, work_root / game_id

  def test_cam_directories_are_real_and_only_inputs_are_linked(self) -> None:
    self.touch("cam1/GX010123.MP4")
    self.touch("cam1/GX020123.MP4")
    self.touch("cam2/VID_20260828_101500_001.mp4")
    self.touch("cam2/VID_20260828_101500_002.mp4")
    self.touch("left_calibration.json", b"{}")
    self.touch("cam1/camera-calibration-v2.yaml", b"camera: 1\n")
    for mutable in (
        "panorama.tif",
        "autooptimiser_out.pto",
        "hm_project.pto",
        "mapping_0000.tif",
        "left.png",
        "seam_file.png",
        "rink_mask_0.png",
        "stitching_canvas_provenance",
    ):
      self.touch(mutable)

    _, game = self.isolate({})

    self.assertTrue((game / "cam1").is_dir())
    self.assertFalse((game / "cam1").is_symlink())
    self.assertTrue((game / "cam2").is_dir())
    self.assertFalse((game / "cam2").is_symlink())
    self.assertTrue((game / "cam1/GX010123.MP4").is_symlink())
    self.assertTrue((game / "cam2/VID_20260828_101500_002.mp4").is_symlink())
    self.assertTrue((game / "left_calibration.json").is_symlink())
    self.assertTrue((game / "cam1/camera-calibration-v2.yaml").is_symlink())
    for mutable in (
        "panorama.tif",
        "autooptimiser_out.pto",
        "hm_project.pto",
        "mapping_0000.tif",
        "left.png",
        "seam_file.png",
        "rink_mask_0.png",
        "stitching_canvas_provenance",
    ):
      self.assertFalse((game / mutable).exists(), mutable)

  def test_root_chapters_and_vendor_recordings_are_discovered(self) -> None:
    for relative in ("left-2.mkv", "left-12.mkv", "right-3.m4v"):
      self.touch(relative)
    _, game = self.isolate({})
    self.assertTrue((game / "left-2.mkv").is_symlink())
    self.assertTrue((game / "left-12.mkv").is_symlink())
    self.assertTrue((game / "right-3.m4v").is_symlink())

    source = self.root / "vendor-game"
    source.mkdir()
    for name in ("GX010001.MP4", "GA020001.MP4", "GX010002.MP4"):
      (source / name).write_bytes(b"video")
    config = source / "config.yaml"
    config.write_text("{}\n", encoding="utf-8")
    work_root, game_id = matrix.create_isolated_game(source, config)
    self.addCleanup(lambda: matrix.remove_work_root(work_root) if work_root.exists() else None)
    self.assertTrue((work_root / game_id / "GA020001.MP4").is_symlink())

  def test_relative_configured_paths_and_contained_absolute_paths_are_normalized(self) -> None:
    left = self.touch(".hstream-ui/left/left-camera.mov")
    self.touch(".hstream-ui/right/right-camera.avi")
    config = {
        "game": {
            "videos": {
                "left": [str(left)],
                "right": [".hstream-ui/right/right-camera.avi"],
            }
        },
        "hstream_ui": {
            "video_roles": {
                "left": [".hstream-ui/left/left-camera.mov"],
                "right": [".hstream-ui/right/right-camera.avi"],
            }
        },
    }
    _, game = self.isolate(config)
    isolated_config = yaml.safe_load((game / "config.yaml").read_text(encoding="utf-8"))
    self.assertEqual(isolated_config["game"]["videos"]["left"], [".hstream-ui/left/left-camera.mov"])
    self.assertTrue((game / ".hstream-ui").is_dir())
    self.assertFalse((game / ".hstream-ui").is_symlink())
    self.assertTrue((game / ".hstream-ui/left/left-camera.mov").is_symlink())
    self.assertTrue((game / ".hstream-ui/right/right-camera.avi").is_symlink())

  def test_game_only_custom_paths_are_promoted_for_force_mode(self) -> None:
    self.touch("recordings/camera-a.mov")
    self.touch("recordings/camera-b.mov")
    _, game = self.isolate(
        {"game": {"videos": {"left": ["recordings/camera-a.mov"], "right": ["recordings/camera-b.mov"]}}}
    )
    isolated_config = yaml.safe_load((game / "config.yaml").read_text(encoding="utf-8"))
    self.assertEqual(isolated_config["hstream_ui"]["video_roles"]["left"], ["recordings/camera-a.mov"])
    self.assertEqual(isolated_config["hstream_ui"]["video_roles"]["right"], ["recordings/camera-b.mov"])

  def test_partial_roles_resolve_only_unambiguous_root_peer(self) -> None:
    self.touch("left-1.mp4")
    self.touch("left-2.mp4")
    self.touch("right-1.mp4")
    _, game = self.isolate({"hstream_ui": {"video_roles": {"left": ["left-1.mp4", "left-2.mp4"]}}})
    isolated_config = yaml.safe_load((game / "config.yaml").read_text(encoding="utf-8"))
    self.assertEqual(isolated_config["hstream_ui"]["video_roles"]["right"], ["right-1.mp4"])

    self.config.write_text(
        yaml.safe_dump({"hstream_ui": {"video_roles": {"left": ["right-1.mp4"]}}}), encoding="utf-8"
    )
    with self.assertRaisesRegex(ValueError, "playlists must be disjoint"):
      matrix.create_isolated_game(self.source, self.config)

  def test_partial_roles_with_cam_directories_are_rejected(self) -> None:
    self.touch("cam1/GX010123.MP4")
    self.touch("cam2/GX010124.MP4")
    self.config.write_text(
        yaml.safe_dump({"hstream_ui": {"video_roles": {"left": ["cam1/GX010123.MP4"]}}}), encoding="utf-8"
    )
    with self.assertRaisesRegex(ValueError, "cannot infer a side from camN"):
      matrix.create_isolated_game(self.source, self.config)

  def test_missing_peer_and_path_escape_are_rejected(self) -> None:
    self.touch("left.mp4")
    self.config.write_text("{}\n", encoding="utf-8")
    with self.assertRaisesRegex(ValueError, "two discoverable camera inputs"):
      matrix.create_isolated_game(self.source, self.config)

    outside = self.root / "outside.mp4"
    outside.write_bytes(b"video")
    self.config.write_text(
        yaml.safe_dump({"game": {"videos": {"left": ["../outside.mp4"], "right": ["left.mp4"]}}}),
        encoding="utf-8",
    )
    with self.assertRaisesRegex(ValueError, "normalized game-relative"):
      matrix.create_isolated_game(self.source, self.config)

    self.touch("left-camera.mov")
    self.touch("right-camera.mov")
    self.config.write_text(
        yaml.safe_dump(
            {
                "game": {
                    "videos": {
                        "left": ["left.mp4", "left-camera.mov"],
                        "right": ["left.mp4", "right-camera.mov"],
                    }
                }
            }
        ),
        encoding="utf-8",
    )
    with self.assertRaisesRegex(ValueError, "playlists must be disjoint"):
      matrix.create_isolated_game(self.source, self.config)


class ProcessGroupTest(unittest.TestCase):
  def run_state_args(self, pipeline: Path) -> argparse.Namespace:
    return argparse.Namespace(
        control_points=900,
        frame_count=4,
        max_live_canvas_dimension=2048,
        pipeline_app=pipeline,
        workspace=Path(__file__).resolve().parents[1],
        config_root=Path(__file__).resolve().parents[1] / "configs",
        pipeline_config=Path(__file__).resolve().parents[1] / "configs/ds_hockey_app_config.yaml",
        timeout=0.05,
        resource_check_interval=1,
        min_available_memory_gib=0,
        min_gpu_free_memory_gib=0,
        min_tmp_free_gib=0,
    )

  def test_timeout_kills_child_after_leader_exit_even_when_child_ignores_sigint(self) -> None:
    with tempfile.TemporaryDirectory(prefix="matrix-process-group-test-") as root_text:
      root = Path(root_text)
      game_id = "game"
      game = root / game_id
      game.mkdir()
      (game / "config.yaml").write_text("{}\n", encoding="utf-8")
      child_pid_path = root / "child.pid"
      pipeline = root / "leader-exits"
      pipeline.write_text(
          "#!/bin/sh\n"
          + "sh -c 'trap \"\" INT; echo $$ > \"$1\"; while :; do sleep 1; done' child \""
          + str(child_pid_path)
          + "\" &\nexit 0\n",
          encoding="utf-8",
      )
      pipeline.chmod(0o700)

      with mock.patch.object(matrix, "PROCESS_GROUP_INTERRUPT_GRACE_SECONDS", 0.1), mock.patch.object(
          matrix, "PROCESS_GROUP_FINAL_GRACE_SECONDS", 0.05
      ), mock.patch.object(matrix, "PROCESS_GROUP_KILL_WAIT_SECONDS", 1.0):
        result = matrix.run_state(
            self.run_state_args(pipeline), root, game_id, matrix.framing_states()[0], 1, root / "run.log"
        )

      self.assertTrue(result["timed_out"])
      child_pid = int(child_pid_path.read_text(encoding="utf-8"))
      deadline = time.monotonic() + 1
      while True:
        try:
          os.kill(child_pid, 0)
        except ProcessLookupError:
          break
        if time.monotonic() >= deadline:
          self.fail(f"orphan child {child_pid} survived process-group cleanup")
        time.sleep(0.02)

  def test_external_sigint_and_sigterm_are_forwarded(self) -> None:
    for forwarded_signal in (signal.SIGINT, signal.SIGTERM):
      with self.subTest(signal=forwarded_signal), tempfile.TemporaryDirectory(
          prefix="matrix-forward-signal-test-"
      ) as root_text:
        root = Path(root_text)
        marker = root / "forwarded"
        ready = root / "ready"
        pipeline_pid = root / "pipeline.pid"
        pipeline = root / "pipeline"
        pipeline.write_text(
            "#!/bin/sh\n"
            + f"echo $$ > '{pipeline_pid}'\n"
            + f"trap 'echo INT > {marker}; exit 0' INT\n"
            + f"trap 'echo TERM > {marker}; exit 0' TERM\n"
            + f"echo ready > '{ready}'\n"
            + "while :; do sleep 0.1; done\n",
            encoding="utf-8",
        )
        pipeline.chmod(0o700)
        game = root / "game"
        game.mkdir()
        (game / "config.yaml").write_text("{}\n", encoding="utf-8")
        harness = root / "harness.py"
        harness.write_text(
            textwrap.dedent(
                f"""
                import argparse
                from pathlib import Path
                import sys
                sys.path.insert(0, {str(Path(__file__).resolve().parent)!r})
                import run_stitching_calibration_matrix as matrix
                args = argparse.Namespace(
                    control_points=900, frame_count=4, max_live_canvas_dimension=2048,
                    pipeline_app=Path({str(pipeline)!r}), workspace=Path({str(Path(__file__).resolve().parents[1])!r}),
                    config_root=Path({str(Path(__file__).resolve().parents[1] / 'configs')!r}),
                    pipeline_config=Path({str(Path(__file__).resolve().parents[1] / 'configs/ds_hockey_app_config.yaml')!r}),
                    timeout=30, resource_check_interval=30,
                    min_available_memory_gib=0, min_gpu_free_memory_gib=0, min_tmp_free_gib=0)
                matrix.run_state(args, Path({str(root)!r}), 'game', matrix.framing_states()[0], 1,
                                 Path({str(root / 'run.log')!r}))
                """
            ),
            encoding="utf-8",
        )
        process = subprocess.Popen(
            [sys.executable, str(harness)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True
        )
        try:
          deadline = time.monotonic() + 5
          while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
          self.assertTrue(ready.exists())
          os.kill(process.pid, forwarded_signal)
          stdout, stderr = process.communicate(timeout=5)
          self.assertNotEqual(process.returncode, 0, (stdout, stderr))
          self.assertEqual(
              marker.read_text(encoding="utf-8").strip(), "INT" if forwarded_signal == signal.SIGINT else "TERM"
          )
          child_pid = int(pipeline_pid.read_text(encoding="utf-8"))
          with self.assertRaises(ProcessLookupError):
            os.kill(child_pid, 0)
        finally:
          if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)
          if pipeline_pid.is_file():
            child_pid = int(pipeline_pid.read_text(encoding="utf-8"))
            try:
              os.killpg(child_pid, signal.SIGKILL)
            except ProcessLookupError:
              pass


if __name__ == "__main__":
  unittest.main()
