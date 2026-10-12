"""Make sure successful-but-truncated archives cannot pass memory qualification."""

import copy
import json
import unittest
from unittest.mock import patch

from test_uri_playlist_memory import check_output


class ArchiveCheckerTest(unittest.TestCase):
  def setUp(self):
    self.log = ("HSTREAM_URI_PLAYLIST_HANDOFF source=0 next_uri=1\n"
                "HSTREAM_URI_PLAYLIST_HANDOFF source=1 next_uri=1\nApp run successful\n")
    self.probe = dict(streams=[
        dict(index=0, codec_type="video", width=3840, height=2160, r_frame_rate="25/1"),
        dict(index=1, codec_type="audio")], packets=[
            dict(stream_index=0, pts_time=str(i * 0.04), duration_time="0.04") for i in range(50)] + [
            dict(stream_index=1, pts_time=str(i * 0.02), duration_time="0.02") for i in range(100)])

  def check(self, probe=None, log=None):
    with patch("test_uri_playlist_memory.subprocess.check_output",
               return_value=json.dumps(self.probe if probe is None else probe)):
      return check_output(self.log if log is None else log, "archive.mkv", 2)

  def test_complete_archive(self):
    self.assertEqual(self.check()["frames"], 50)

  def test_missing_audio_prefix_and_single_final_packet(self):
    for retained in (list(range(60, 100)), [99]):
      probe = copy.deepcopy(self.probe)
      probe["packets"] = probe["packets"][:50] + [probe["packets"][50 + i] for i in retained]
      with self.subTest(retained=len(retained)), self.assertRaises(ValueError):
        self.check(probe)

  def test_missing_duplicate_and_overlapping_packets(self):
    for stream_start, index in ((0, 20), (50, 70)):
      for corruption in ("missing", "duplicate", "overlap"):
        probe = copy.deepcopy(self.probe)
        if corruption == "missing":
          del probe["packets"][index]
        elif corruption == "duplicate":
          probe["packets"].append(copy.deepcopy(probe["packets"][index]))
        else:
          previous = float(probe["packets"][index - 1]["pts_time"])
          probe["packets"][index]["pts_time"] = str(previous + 0.001)
        with self.subTest(stream=stream_start, corruption=corruption), self.assertRaises(ValueError):
          self.check(probe)

  def test_invalid_timestamps_and_audio_duration(self):
    for index, field in ((20, "pts_time"), (70, "pts_time"), (70, "duration_time")):
      for value in (None, "nan", "inf"):
        probe = copy.deepcopy(self.probe)
        if value is None:
          del probe["packets"][index][field]
        else:
          probe["packets"][index][field] = value
        with self.subTest(index=index, field=field, value=value), self.assertRaises(ValueError):
          self.check(probe)

  def test_truncated_video_or_audio_tail(self):
    for end in (40, 130):
      probe = copy.deepcopy(self.probe)
      if end == 40:
        probe["packets"] = probe["packets"][:end] + probe["packets"][50:]
      else:
        probe["packets"] = probe["packets"][:end]
      with self.subTest(end=end), self.assertRaises(ValueError):
        self.check(probe)

  def test_shifted_archive_origin(self):
    for offset in (-1, 1):
      probe = copy.deepcopy(self.probe)
      for packet in probe["packets"]:
        packet["pts_time"] = str(float(packet["pts_time"]) + offset)
      with self.subTest(offset=offset), self.assertRaises(ValueError):
        self.check(probe)

  def test_failed_runner_and_missing_chapter_handoff(self):
    logs = [self.log.replace("App run successful", ""),
            self.log.replace("source=1", "source=2"),
            self.log + "FATAL: Camera chapter reader retirement timed out\n",
            self.log + "CUDA_ERROR_OUT_OF_MEMORY\n"]
    for log in logs:
      with self.subTest(log=log), self.assertRaises(ValueError):
        self.check(log=log)


if __name__ == "__main__":
  unittest.main()
