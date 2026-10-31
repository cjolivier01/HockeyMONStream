#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/stitching/CrossCorrelation.h"
#include "hstream/src/libs/stitching/Synchronization.h"

namespace {
void le(std::string& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i)
    bytes += static_cast<char>(value >> (i * 8));
}
void real(std::string& bytes, double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  le(bytes, bits, 8);
}
void varint(std::string& bytes, uint64_t value) {
  do {
    const unsigned byte = value & 127;
    value >>= 7;
    bytes += static_cast<char>(byte | (value ? 128 : 0));
  } while (value);
}
void field(std::string& bytes, unsigned tag, uint64_t value) {
  varint(bytes, tag << 3);
  varint(bytes, value);
}
void append_gyro(const std::filesystem::path& path, bool raw, double delay, bool stationary = false) {
  std::string gyro;
  for (int i = 0; i <= 3200; ++i) {
    const double t = i / 200.0;
    le(gyro, static_cast<uint64_t>(std::llround(1000000 + (t + 0.125) * (raw ? 1000000 : 1000))), 8);
    const double shifted = t + delay;
    const double x = stationary ? 10
                                : 10 + std::sin(13 * shifted + 0.4 * shifted * shifted) + 0.7 * std::cos(29 * shifted) +
            0.2 * std::sin(67 * shifted);
    for (unsigned axis = 0; axis < 6; ++axis) {
      if (raw)
        le(gyro, 32768 + (axis == 3 ? static_cast<unsigned>(std::llround(x * 100)) : 0), 2);
      else
        real(gyro, axis == 3 ? x : 0);
    }
  }
  std::string trailer = gyro;
  le(trailer, 0x300, 2);
  le(trailer, gyro.size(), 4);
  std::string info;
  field(info, 24, 1000000);
  field(info, 62, raw);
  varint(info, (28 << 3) | 1);
  real(info, 125); // ms, gyro-only correction
  field(info, 29, 1);
  trailer += info;
  le(trailer, 0x100, 2);
  le(trailer, info.size(), 4);
  trailer += std::string(32, '\0');
  le(trailer, trailer.size() + 40, 4);
  le(trailer, 1, 4);
  trailer += "8db42d694ccc418790edff439fe026bf";
  std::ofstream(path, std::ios::binary | std::ios::app).write(trailer.data(), trailer.size());
}
bool expect(bool ok, const char* message) {
  if (!ok)
    std::cerr << message << '\n';
  return ok;
}
} // namespace

int main() {
  using namespace hm::stitching;
  bool correlation_ok = true;
  const std::vector<float> x{0, 2, -1, 3}, y{1, -2, 4};
  const auto direct = full_correlate(x, y), fft = full_correlate_fft(x, y);
  for (size_t i = 0; i < direct.size(); ++i)
    correlation_ok &= expect(std::abs(direct[i] - fft[i]) < 1e-5, "FFT lag must match direct correlation");
  if (!correlation_ok)
    return 1;
  const auto ffmpeg = hm::findExecutable("ffmpeg", {"PATH"});
  if (!ffmpeg) {
    std::cerr << "Synchronization integration test requires ffmpeg\n";
    return 1;
  }
  const auto dir = std::filesystem::temp_directory_path() / ("hstream-imu-" + std::to_string(getpid()));
  std::filesystem::create_directory(dir);
  bool ok = true;
  for (const auto& [name, rate] : {std::pair{"left", "25"}, std::pair{"right", "30"}}) {
    ok &= expect(
        hm::run_command(
            {*ffmpeg,
             "-v",
             "error",
             "-y",
             "-f",
             "lavfi",
             "-i",
             std::string("color=s=16x16:r=") + rate,
             "-f",
             "lavfi",
             "-i",
             "anoisesrc=r=8000:seed=42",
             "-t",
             "16",
             "-c:v",
             "mpeg4",
             "-c:a",
             "aac",
             (dir / (std::string(name) + ".mp4")).string()},
            "",
            {{"LC_ALL", "C"}},
            [](const std::string& error, const std::string&) {
              if (!error.empty())
                std::cerr << error << '\n';
            }) == 0,
        "Create camera fixture");
  }
  if (!ok)
    return 1;
  const auto continuous = (dir / "continuous.mp4").string();
  ok &= expect(
      hm::run_command(
          {*ffmpeg,
           "-v",
           "error",
           "-f",
           "lavfi",
           "-i",
           "color=s=16x16:r=60000/1001",
           "-f",
           "lavfi",
           "-i",
           "anoisesrc=r=48000:seed=42",
           "-t",
           "16",
           "-g",
           "363",
           "-c:v",
           "mpeg4",
           "-c:a",
           "aac",
           continuous},
          "",
          {},
          {}) == 0,
      "Create fractional-rate continuous fixture");
  ok &= expect(
      hm::run_command(
          {*ffmpeg,
           "-v",
           "error",
           "-i",
           continuous,
           "-map",
           "0",
           "-c",
           "copy",
           "-f",
           "segment",
           "-segment_frames",
           "363,726",
           "-reset_timestamps",
           "1",
           (dir / "part%02d.mp4").string()},
          "",
          {},
          {}) == 0,
      "Split identical audio at fractional PCM chapter boundaries");
  if (!ok)
    return 1;
  const auto whole = CameraRecordingWindowAt({continuous}, 0, 35);
  const auto split = CameraRecordingWindowAt(
      {(dir / "part00.mp4").string(), (dir / "part01.mp4").string(), (dir / "part02.mp4").string()}, 0, 35);
  if (!expect(whole.ok() && split.ok(), "Read fractional-rate chapter windows"))
    return 1;
  const auto segmented = synchronize_by_audio(*whole, *split);
  ok &= expect(segmented.ok(), "Identical chapter audio must occupy one cumulative PCM grid");
  if (!segmented.ok())
    std::cerr << segmented.status() << '\n';
  const auto left = (dir / "left.mp4").string(), right = (dir / "right.mp4").string();
  const auto audio = calculate_stitching_synchronization(left, left);
  const auto fallback = calculate_stitching_synchronization(left, left, SynchronizationMethod::kAuto);
  ok &= expect(
      audio.ok() && fallback.ok() && audio->video1_frame_offset < 0.01 && audio->video2_frame_offset < 0.01 &&
          fallback->video1_frame_offset == audio->video1_frame_offset &&
          fallback->video2_frame_offset == audio->video2_frame_offset,
      "Default audio and automatic fallback must use the audio tracks");
  ok &= expect(
      !calculate_stitching_synchronization(left, right, SynchronizationMethod::kImu).ok(),
      "Strict IMU must report unavailable telemetry");
  const auto still = dir / "still.mp4";
  std::filesystem::copy_file(left, still);
  append_gyro(still, true, 0, true);
  ok &= expect(
      !calculate_stitching_synchronization(still, still, SynchronizationMethod::kImu).ok(),
      "Strict IMU must reject constant motion");
  ok &= expect(
      calculate_stitching_synchronization(still, still, SynchronizationMethod::kAuto).ok(),
      "Automatic synchronization must also fall back on weak gyro signals");
  for (bool chapters : {false, true}) {
    const double chapter_base = chapters ? 16 : 0;
    const auto trimmed = calculate_stitching_synchronization(
        chapters ? std::vector<std::string>{left, left} : std::vector<std::string>{left},
        chapters ? std::vector<std::string>{right, right} : std::vector<std::string>{right},
        {chapter_base + 5, chapter_base + 6.25},
        SynchronizationMethod::kAudio);
    ok &= expect(
        trimmed.ok() && std::abs(trimmed->video1_frame_offset / 25 - chapter_base - 6.25) < 0.002 &&
            std::abs(trimmed->video2_frame_offset / 30 - chapter_base - 6.25) < 0.002,
        "Source trims precede audio sync and span complete chapter playlists");
    if (!trimmed.ok())
      std::cerr << trimmed.status() << '\n';
  }
  const auto too_late = calculate_stitching_synchronization(
      std::vector<std::string>{left}, std::vector<std::string>{right}, {16, 0}, SynchronizationMethod::kAuto);
  ok &= expect(absl::IsOutOfRange(too_late.status()), "Start at recording end must report a range error");
  append_gyro(left, true, 0);
  append_gyro(right, false, 1.25);
  const auto raw = imu::Read(left), legacy = imu::Read(right);
  ok &= expect(
      raw.ok() && legacy.ok() && std::abs(raw->gyro.front().seconds) < 1e-8 &&
          std::abs(legacy->gyro.front().seconds) < 1e-8,
      "Both Insta360 encodings must subtract first-frame and gyro-only clock offsets");
  const auto tail = imu::Read(left, 14.9, 0.1);
  ok &= expect(
      tail.ok() && !tail->gyro.empty() && tail->gyro.size() < 100,
      "A short chapter slice must remain available for concatenated IMU windows");
  const auto across_chapters = calculate_stitching_synchronization(
      std::vector<std::string>{left, left},
      std::vector<std::string>{left, left},
      {15, 15},
      SynchronizationMethod::kImu);
  ok &= expect(
      across_chapters.ok() && std::abs(across_chapters->video1_frame_offset - 375) < 0.01 &&
          std::abs(across_chapters->video2_frame_offset - 375) < 0.01,
      "Positioned Insta360 windows must exclude chapter endpoints before concatenating the next chapter");
  if (!across_chapters.ok())
    std::cerr << across_chapters.status() << '\n';
  const auto sync = calculate_stitching_synchronization(left, right, SynchronizationMethod::kImu);
  const auto reverse = calculate_stitching_synchronization(right, left, SynchronizationMethod::kAuto);
  ok &= expect(
      sync.ok() && reverse.ok() && std::abs(sync->video1_frame_offset - 31.25) < 0.13 &&
          sync->video2_frame_offset == 0 && reverse->video1_frame_offset == 0 &&
          std::abs(reverse->video2_frame_offset - 31.25) < 0.13,
      "IMU dispatch must convert the signed delay using the skipped camera's FPS");
  const auto imu_trimmed = calculate_stitching_synchronization(
      std::vector<std::string>{left}, std::vector<std::string>{right}, {5, 3.75}, SynchronizationMethod::kImu);
  ok &= expect(
      imu_trimmed.ok() && std::abs(imu_trimmed->video1_frame_offset - 125) < 0.13 &&
          std::abs(imu_trimmed->video2_frame_offset - 112.5) < 0.16,
      "IMU windows must seek before comparing, including Insta360 trailer data");
  if (!imu_trimmed.ok())
    std::cerr << imu_trimmed.status() << '\n';
  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
