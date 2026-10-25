#pragma once

#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace hm::model_infer {
constexpr uint32_t kMaximumBatch = 32;
constexpr size_t kInputFloats = 3 * 256 * 128;
constexpr size_t kOutputFloats = 256;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "Protocol requires IEEE float32");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Appearance IPC float payloads require little-endian hosts"
#endif
inline bool ReadExact(int fd, void* destination, size_t bytes, bool allow_eof = false) {
  auto* data = static_cast<char*>(destination);
  size_t done = 0;
  while (done < bytes) {
    const auto count = ::read(fd, data + done, bytes - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count == 0 && done == 0 && allow_eof)
      return false;
    if (count <= 0)
      throw std::runtime_error("Truncated or unreadable model inference request");
    done += static_cast<size_t>(count);
  }
  return true;
}
inline void WriteExact(int fd, const void* source, size_t bytes) {
  const auto* data = static_cast<const char*>(source);
  size_t done = 0;
  while (done < bytes) {
    const auto count = ::write(fd, data + done, bytes - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("Cannot write model inference response");
    done += static_cast<size_t>(count);
  }
}
inline uint32_t ReadBatch(int fd) {
  uint8_t header[4];
  if (!ReadExact(fd, header, sizeof(header), true))
    return 0;
  const uint32_t count =
      uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
  if (count > kMaximumBatch)
    throw std::runtime_error("Model inference batch exceeds the 32-crop limit");
  return count;
}
inline void WriteBatch(int fd, uint32_t count) {
  const uint8_t header[4] = {uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
  WriteExact(fd, header, sizeof(header));
}
inline void Finite(const std::vector<float>& values) {
  for (const auto value : values)
    if (!std::isfinite(value))
      throw std::runtime_error("Model inference tensors must contain only finite values");
}
} // namespace hm::model_infer
