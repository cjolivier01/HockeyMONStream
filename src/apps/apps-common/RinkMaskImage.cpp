#include "hstream/src/apps/apps-common/RinkMaskImage.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <utility>

namespace hm::gpu_preview {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
constexpr std::size_t kPngHeaderBytes = 24;
constexpr std::uint64_t kResidentCopiesPerPixel = 3;

std::uint32_t read_big_endian_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) << 24U | static_cast<std::uint32_t>(bytes[1]) << 16U |
      static_cast<std::uint32_t>(bytes[2]) << 8U | static_cast<std::uint32_t>(bytes[3]);
}

RinkMaskImage decode_with_opencv(
    const std::vector<std::uint8_t>& compressed,
    std::uint32_t expected_width,
    std::uint32_t expected_height) {
  const cv::Mat decoded = cv::imdecode(compressed, cv::IMREAD_GRAYSCALE);
  if (decoded.empty() || decoded.cols != static_cast<int>(expected_width) ||
      decoded.rows != static_cast<int>(expected_height) || decoded.type() != CV_8UC1) {
    return {};
  }
  const std::size_t decoded_bytes = static_cast<std::size_t>(expected_width) * expected_height;
  RinkMaskImage image;
  image.width = expected_width;
  image.height = expected_height;
  image.alpha.resize(decoded_bytes);
  if (decoded.isContinuous()) {
    std::copy_n(decoded.ptr<std::uint8_t>(), decoded_bytes, image.alpha.begin());
  } else {
    for (std::uint32_t row = 0; row < expected_height; ++row) {
      std::copy_n(
          decoded.ptr<std::uint8_t>(static_cast<int>(row)),
          expected_width,
          image.alpha.begin() + static_cast<std::size_t>(row) * expected_width);
    }
  }
  return image;
}

RinkMaskLoadResult failed(RinkMaskLoadStatus status, const char* message) {
  return RinkMaskLoadResult{status, {}, message};
}

} // namespace

RinkMaskLoadResult load_rink_mask_png(const std::string& path, const RinkMaskDecoder& decoder) {
  try {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
      return failed(RinkMaskLoadStatus::kMissing, "file is missing or is not regular");
    const std::uintmax_t file_size = fs::file_size(path, error);
    if (error)
      return failed(RinkMaskLoadStatus::kMissing, "file size is unavailable");
    if (file_size > kMaximumRinkMaskCompressedBytes)
      return failed(RinkMaskLoadStatus::kCompressedFileTooLarge, "compressed PNG exceeds 8 MiB");
    if (file_size < kPngHeaderBytes)
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "PNG header is truncated");

    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(file_size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size())))
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "compressed PNG could not be read completely");
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), compressed.begin()) ||
        read_big_endian_u32(compressed.data() + 8) != 13U || std::memcmp(compressed.data() + 12, "IHDR", 4) != 0) {
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "PNG signature or IHDR is invalid");
    }

    const std::uint32_t width = read_big_endian_u32(compressed.data() + 16);
    const std::uint32_t height = read_big_endian_u32(compressed.data() + 20);
    if (width == 0 || height == 0 || width > kMaximumRinkMaskDimension || height > kMaximumRinkMaskDimension)
      return failed(RinkMaskLoadStatus::kDimensionsTooLarge, "PNG dimensions exceed the 12288 pixel limit");
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > kMaximumRinkMaskTextureBytes)
      return failed(RinkMaskLoadStatus::kTextureBudgetExceeded, "GL_ALPHA8 texture exceeds 32 MiB");
    if (file_size > kMaximumRinkMaskResourceBytes - pixels * kResidentCopiesPerPixel) {
      return failed(
          RinkMaskLoadStatus::kResourceBudgetExceeded,
          "compressed, decoded, upload, and texture resources exceed 96 MiB");
    }

    RinkMaskImage image = decoder ? decoder(compressed, width, height) : decode_with_opencv(compressed, width, height);
    if (image.width != width || image.height != height || image.alpha.size() != pixels)
      return failed(RinkMaskLoadStatus::kDecodeFailed, "decoder returned empty or inconsistent grayscale pixels");
    return RinkMaskLoadResult{RinkMaskLoadStatus::kLoaded, std::move(image), ""};
  } catch (const cv::Exception&) {
    return failed(RinkMaskLoadStatus::kDecodeFailed, "OpenCV decode threw an exception");
  } catch (const std::bad_alloc&) {
    return failed(RinkMaskLoadStatus::kDecodeFailed, "rink-mask allocation failed");
  } catch (const std::exception&) {
    return failed(RinkMaskLoadStatus::kDecodeFailed, "rink-mask decoder threw a standard exception");
  } catch (...) {
    return failed(RinkMaskLoadStatus::kDecodeFailed, "rink-mask decode failed with an unknown exception");
  }
}

const char* rink_mask_load_status_name(RinkMaskLoadStatus status) {
  switch (status) {
    case RinkMaskLoadStatus::kLoaded:
      return "loaded";
    case RinkMaskLoadStatus::kMissing:
      return "missing";
    case RinkMaskLoadStatus::kCompressedFileTooLarge:
      return "compressed-file-too-large";
    case RinkMaskLoadStatus::kInvalidPngHeader:
      return "invalid-png-header";
    case RinkMaskLoadStatus::kDimensionsTooLarge:
      return "dimensions-too-large";
    case RinkMaskLoadStatus::kTextureBudgetExceeded:
      return "texture-budget-exceeded";
    case RinkMaskLoadStatus::kResourceBudgetExceeded:
      return "resource-budget-exceeded";
    case RinkMaskLoadStatus::kDecodeFailed:
      return "decode-failed";
  }
  return "unknown";
}

unsigned rink_mask_retry_delay_seconds(unsigned consecutive_failures) {
  if (consecutive_failures == 0)
    return 0;
  constexpr unsigned kMaximumDelaySeconds = 30;
  const unsigned shift = std::min(consecutive_failures - 1U, 4U);
  return std::min(2U << shift, kMaximumDelaySeconds);
}

} // namespace hm::gpu_preview
