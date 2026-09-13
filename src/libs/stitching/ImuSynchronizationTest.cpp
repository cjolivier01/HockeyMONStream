#include "hstream/src/libs/stitching/ImuSynchronization.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>

namespace {
using hm::stitching::imu::Sample;
bool expect(bool ok, const char* message) {
  if (!ok)
    std::cerr << message << '\n';
  return ok;
}
double motion(double t) {
  return 10 + std::sin(t * 13 + 0.4 * t * t) + 0.7 * std::cos(29 * t) + 0.2 * std::sin(67 * t);
}
std::vector<Sample> signal(double delay, double start = 0, double rate = 200) {
  std::vector<Sample> result;
  for (int i = 0; i < int(15 * rate); ++i) {
    const double t = start + i / rate;
    result.push_back({t, motion(t + delay), 0, 0});
  }
  return result;
}
void be(std::string& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i)
    bytes += static_cast<char>(value >> ((count - i - 1) * 8));
}
std::string tag(const char* key, char type, unsigned width, unsigned count, std::string data) {
  std::string result = key;
  result += type;
  result += char(width);
  be(result, count, 2);
  while (data.size() % 4)
    data += '\0';
  return result + data;
}
} // namespace

int main(int argc, char** argv) {
  using namespace hm::stitching;
  // Real-recording smoke/diagnostic entry point, with no image decode or audio dependency.
  if (argc == 2 || argc == 3) {
    if (argc == 3) {
      const auto result = synchronize_by_imu(argv[1], argv[2]);
      if (!result.ok()) {
        std::cerr << result.status() << '\n';
        return 1;
      }
      std::cout << "Frame skips: left=" << result->first << " right=" << result->second << '\n';
    } else {
      const auto result = imu::Read(argv[1]);
      if (!result.ok()) {
        std::cerr << result.status() << '\n';
        return 1;
      }
      std::cout << result->gyro.size() << " samples, " << result->fps << " fps, time " << result->gyro.front().seconds
                << ".." << result->gyro.back().seconds << '\n';
    }
    return 0;
  }
  bool ok = true;
  ok &= expect(*parse_synchronization_method("audio") == SynchronizationMethod::kAudio, "Default audio selection");
  ok &= expect(*parse_synchronization_method("imu") == SynchronizationMethod::kImu, "IMU selection");
  ok &= expect(*parse_synchronization_method("auto") == SynchronizationMethod::kAuto, "Fallback selection");
  ok &= expect(!parse_synchronization_method("typo").ok(), "Reject unknown method");
  for (double delay : {-1.375, 0.0, 2.125}) {
    auto right = signal(delay, 0.035, 400);
    // A rotated and uniformly scaled sensor must retain the same magnitude timing.
    for (auto& sample : right) {
      sample.z = -3 * sample.x;
      sample.x = 0;
    }
    const auto offset = imu::EstimateOffset(signal(0), right);
    ok &= expect(offset.ok() && std::abs(*offset - delay) < 0.006, "Correct skip sign, sample origins, rates and axes");
  }
  auto constant = signal(0);
  for (auto& sample : constant)
    sample.x = 1;
  ok &= expect(!imu::EstimateOffset(constant, constant).ok(), "Reject stationary cameras");
  auto noise = signal(0);
  std::mt19937 random(123);
  std::uniform_real_distribution<double> distribution(0, 1);
  for (auto& sample : noise)
    sample.x = distribution(random);
  ok &= expect(!imu::EstimateOffset(signal(0), noise).ok(), "Reject unrelated movement");
  auto periodic = signal(0);
  for (auto& sample : periodic)
    sample.x = 2 + std::sin(sample.seconds * 10);
  ok &= expect(!imu::EstimateOffset(periodic, periodic).ok(), "Reject ambiguous periodic motion");
  auto invalid = signal(0);
  invalid[10].seconds = invalid[9].seconds;
  ok &= expect(!imu::EstimateOffset(invalid, signal(0)).ok(), "Reject duplicate timestamps");
  invalid[10].seconds = std::numeric_limits<double>::quiet_NaN();
  ok &= expect(!imu::EstimateOffset(invalid, signal(0)).ok(), "Reject NaN timestamps");
  ok &= expect(!imu::EstimateOffset({}, signal(0)).ok(), "Reject missing telemetry");
  std::string values, scale;
  for (int value : {100, -200, 300, 400, -500, 600})
    be(values, static_cast<uint16_t>(value), 2);
  be(scale, 100, 4);
  const auto gyro = tag("SCAL", 'l', 4, 1, scale) + tag("GYRO", 's', 6, 2, values);
  const auto unrelated = tag("SCAL", 'f', 4, 5, std::string(20, '\0'));
  const auto payload = tag("STRM", 0, 1, unrelated.size(), unrelated) + tag("STRM", 0, 1, gyro.size(), gyro);
  auto packet = tag("DEVC", 0, 1, payload.size(), payload);
  auto parsed = imu::ParseGpmf(reinterpret_cast<const unsigned char*>(packet.data()), packet.size(), 2, 1);
  ok &= expect(
      parsed.ok() && parsed->size() == 2 && parsed->at(0).y == -2 && parsed->at(1).z == 6 &&
          parsed->at(1).seconds == 2.5,
      "Parse nested GPMF, scale signed axes, retain packet timing and ignore unrelated scales");
  packet.pop_back();
  ok &= expect(
      !imu::ParseGpmf(reinterpret_cast<const unsigned char*>(packet.data()), packet.size(), 0, 1).ok(),
      "Reject truncated GPMF");
  return ok ? 0 : 1;
}
