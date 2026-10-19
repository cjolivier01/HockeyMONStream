"""Check geometry and actual FFmpeg frame pairing, trimming and audio duration."""

from fractions import Fraction
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import side_by_side_video as side


SCRIPT = Path(side.__file__).resolve()


def run(*args):
  return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class LayoutTest(unittest.TestCase):
  def video(self, width, height):
    return side.Video(width, height, Fraction(30), False, 10)

  def test_native_4k_pair_and_8k_cap(self):
    self.assertEqual(side.layout([self.video(3840, 2160)] * 2, (7680, 4320)),
                     (3840, 2160, [(3840, 2160)] * 2))
    self.assertEqual(side.layout([self.video(7680, 4320)] * 2, (7680, 4320)),
                     (3840, 2160, [(3840, 2160)] * 2))

  def test_portrait_and_landscape_fit_without_enlarging(self):
    width, height, sizes = side.layout([self.video(1920, 1080), self.video(720, 1280)], (7680, 4320))
    self.assertEqual((width, height, sizes), (1920, 1280, [(1920, 1080), (720, 1280)]))
    width, height, sizes = side.layout([self.video(8000, 10000)] * 2, (7680, 4320))
    self.assertLessEqual(width * 2, 7680)
    self.assertEqual(height, 4320)
    for fitted_width, fitted_height in sizes:
      self.assertAlmostEqual(fitted_width / fitted_height, 0.8, places=3)

  def test_hardware_failure_falls_back_but_explicit_request_fails(self):
    args = side.parse_args(["left.mp4", "right.mp4", "output.mp4"])
    # A driver failure can exit without stderr. It must still trigger fallback.
    with mock.patch.object(side.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, "", "")):
      encoder, options = side.encoder_options(args, 7680, 2160, Fraction(30))
      self.assertEqual(encoder, "CPU H.264")
      self.assertIn("veryfast", options)
      args.encoder = "nvenc"
      with self.assertRaisesRegex(ValueError, "NVIDIA encoding is unavailable"):
        side.encoder_options(args, 7680, 2160, Fraction(30))


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg is required for integration checks")
class EncodingTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    cls.temporary = tempfile.TemporaryDirectory()
    cls.directory = Path(cls.temporary.name)
    cls.left = cls.directory / "left clip.mp4"
    cls.right = cls.directory / "right clip.mp4"
    cls.slow = cls.directory / "slow.mp4"
    for path, width, fps, duration, audio in ((cls.left, 64, 10, 2, True),
                                             (cls.right, 32, 10, 3, False),
                                             (cls.slow, 32, 5, 2, False)):
      command = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                 f"nullsrc=s={width}x48:r={fps}:d={duration},geq=lum='16+N*8':cb=128:cr=128"]
      if audio:
        command += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.5", "-c:a", "aac"]
      run(*command, "-c:v", "libx264", "-crf", "0", "-preset", "ultrafast", "-pix_fmt", "yuv420p", str(path))

  @classmethod
  def tearDownClass(cls):
    cls.temporary.cleanup()

  def encode(self, name, *options, right=None):
    output = self.directory / name
    run(sys.executable, str(SCRIPT), str(self.left), str(right or self.right), str(output),
        "--encoder", "cpu", "--crf", "0", "--preset", "ultrafast", *options)
    info = json.loads(run("ffprobe", "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(output)).stdout)
    return output, info["streams"]

  def test_trim_pairs_frames_and_pads_mismatched_aspect(self):
    output, streams = self.encode("trim.mp4", "--start", "00:00.5", "--duration", "1")
    video = next(stream for stream in streams if stream["codec_type"] == "video")
    self.assertEqual((video["width"], video["height"], int(video["nb_read_frames"])), (128, 48, 10))
    frames = run("ffmpeg", "-v", "error", "-i", str(output), "-an", "-f", "rawvideo", "-pix_fmt", "yuv444p", "-").stdout
    frame_bytes = 128 * 48 * 3
    for frame in range(10):
      y = frames[frame * frame_bytes:(frame + 1) * frame_bytes]
      # Both views must show source frame 5..14, with black beside the narrow right view.
      self.assertEqual(y[24 * 128 + 32], 16 + (frame + 5) * 8)
      self.assertEqual(y[24 * 128 + 96], y[24 * 128 + 32])
      self.assertEqual(y[24 * 128 + 70], 16)

  def test_whole_shorter_video_and_short_audio(self):
    _, streams = self.encode("whole.mp4")
    video = next(stream for stream in streams if stream["codec_type"] == "video")
    audio = next(stream for stream in streams if stream["codec_type"] == "audio")
    self.assertEqual(int(video["nb_read_frames"]), 20)
    self.assertAlmostEqual(float(video["duration"]), 2, places=2)
    self.assertAlmostEqual(float(audio["duration"]), 2, delta=0.06)

  def test_different_rates_preserve_duration(self):
    output, streams = self.encode("mixed.mp4", "--audio", "none", right=self.slow)
    self.assertEqual(len(streams), 1)
    self.assertEqual(int(streams[0]["nb_read_frames"]), 20)
    self.assertEqual(Fraction(streams[0]["avg_frame_rate"]), 10)
    frames = run("ffmpeg", "-v", "error", "-i", str(output), "-f", "rawvideo", "-pix_fmt", "yuv444p", "-").stdout
    for frame in range(20):
      y = frames[frame * 128 * 48 * 3:]
      self.assertEqual(y[24 * 128 + 96], 16 + (frame // 2) * 8)

  def test_rotation_and_sample_aspect(self):
    rotated = self.directory / "rotated.mp4"
    run("ffmpeg", "-v", "error", "-i", str(self.right), "-c", "copy", "-metadata:s:v:0", "rotate=90", str(rotated))
    _, streams = self.encode("rotation.mp4", "--audio", "right", right=rotated)
    self.assertEqual((streams[0]["width"], streams[0]["height"]), (128, 48))
    self.assertEqual(len(streams), 1)
    anamorphic = self.directory / "anamorphic.mp4"
    run("ffmpeg", "-v", "error", "-i", str(self.right), "-vf", "setsar=2", "-c:v", "libx264", str(anamorphic))
    self.assertEqual(side.probe(anamorphic).width, 64)
    _, streams = self.encode("anamorphic-output.mp4", "--audio", "none", right=anamorphic)
    self.assertEqual(streams[0]["sample_aspect_ratio"], "1:1")

  def test_protect_inputs_and_existing_output_and_reject_empty_clip(self):
    before = self.left.read_bytes()
    existing = self.directory / "existing.mp4"
    existing.write_bytes(b"Keep this output")
    for output, options in ((self.left, ["--overwrite"]), (self.right, []),
                             (existing, []),
                             (self.directory / "past-end.mp4", ["--start", "2"]),
                             (self.directory / "empty.mp4", ["--duration", "0"])):
      result = subprocess.run([sys.executable, str(SCRIPT), str(self.left), str(self.right), str(output), *options],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
      self.assertNotEqual(result.returncode, 0)
    self.assertEqual(self.left.read_bytes(), before)
    self.assertEqual(existing.read_bytes(), b"Keep this output")


if __name__ == "__main__":
  unittest.main()
