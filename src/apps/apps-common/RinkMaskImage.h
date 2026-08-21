#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hm::gpu_preview {

inline constexpr std::uint64_t kMaximumRinkMaskCompressedBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumRinkMaskDimension = 12288U;
inline constexpr std::uint64_t kMaximumRinkMaskTextureBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumRinkMaskResourceBytes = 96ULL * 1024ULL * 1024ULL;

struct RinkMaskImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> alpha;
};

enum class RinkMaskLoadStatus {
  kLoaded,
  kMissing,
  kCompressedFileTooLarge,
  kInvalidPngHeader,
  kDimensionsTooLarge,
  kTextureBudgetExceeded,
  kResourceBudgetExceeded,
  kDecodeFailed,
};

struct RinkMaskLoadResult {
  RinkMaskLoadStatus status{RinkMaskLoadStatus::kDecodeFailed};
  RinkMaskImage image;
  const char* message{"rink-mask decode failed"};

  explicit operator bool() const {
    return status == RinkMaskLoadStatus::kLoaded;
  }
};

using RinkMaskDecoder = std::function<RinkMaskImage(
    const std::vector<std::uint8_t>& compressed,
    std::uint32_t expected_width,
    std::uint32_t expected_height)>;

// Validates the compressed PNG and its IHDR before invoking OpenCV. The
// optional decoder exists for deterministic exception-containment tests.
RinkMaskLoadResult load_rink_mask_png(const std::string& path, const RinkMaskDecoder& decoder = {});
const char* rink_mask_load_status_name(RinkMaskLoadStatus status);

// First failure waits two seconds; repeated failures back off to 30 seconds.
unsigned rink_mask_retry_delay_seconds(unsigned consecutive_failures);

} // namespace hm::gpu_preview
