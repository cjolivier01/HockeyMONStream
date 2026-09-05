#include "hstream/src/apps/apps-common/RinkMaskImage.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::gpu_preview {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
constexpr std::size_t kPngHeaderBytes = 24;
constexpr std::uint64_t kResidentCopiesPerPixel = 3;

std::uint32_t read_big_endian_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) << 24U | static_cast<std::uint32_t>(bytes[1]) << 16U |
      static_cast<std::uint32_t>(bytes[2]) << 8U | static_cast<std::uint32_t>(bytes[3]);
}

RinkMaskImage copy_grayscale_mat(
    const cv::Mat& input,
    std::uint32_t canvas_width,
    std::uint32_t canvas_height,
    std::uint32_t texture_width,
    std::uint32_t texture_height) {
  const std::size_t texture_bytes = static_cast<std::size_t>(texture_width) * texture_height;
  RinkMaskImage image;
  image.canvas_width = canvas_width;
  image.canvas_height = canvas_height;
  image.width = texture_width;
  image.height = texture_height;
  image.alpha.resize(texture_bytes);
  if (input.isContinuous()) {
    std::copy_n(input.ptr<std::uint8_t>(), texture_bytes, image.alpha.begin());
  } else {
    for (std::uint32_t row = 0; row < texture_height; ++row) {
      std::copy_n(
          input.ptr<std::uint8_t>(static_cast<int>(row)),
          texture_width,
          image.alpha.begin() + static_cast<std::size_t>(row) * texture_width);
    }
  }
  return image;
}

RinkMaskImage downsample_rink_mask_image(
    const RinkMaskImage& source,
    std::uint32_t texture_width,
    std::uint32_t texture_height) {
  if (source.width == texture_width && source.height == texture_height) {
    RinkMaskImage image = source;
    image.canvas_width = image.canvas_width ? image.canvas_width : image.width;
    image.canvas_height = image.canvas_height ? image.canvas_height : image.height;
    return image;
  }
  if (source.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      source.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      texture_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      texture_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  cv::Mat source_view(
      static_cast<int>(source.height),
      static_cast<int>(source.width),
      CV_8UC1,
      const_cast<std::uint8_t*>(source.alpha.data()));
  cv::Mat resized;
  cv::resize(
      source_view,
      resized,
      cv::Size(static_cast<int>(texture_width), static_cast<int>(texture_height)),
      0.0,
      0.0,
      cv::INTER_AREA);
  return copy_grayscale_mat(
      resized,
      source.canvas_width ? source.canvas_width : source.width,
      source.canvas_height ? source.canvas_height : source.height,
      texture_width,
      texture_height);
}

RinkMaskImage decode_with_opencv(
    const std::vector<std::uint8_t>& compressed,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    std::uint32_t texture_width,
    std::uint32_t texture_height) {
  const cv::Mat decoded = cv::imdecode(compressed, cv::IMREAD_GRAYSCALE);
  if (decoded.empty() || decoded.cols != static_cast<int>(expected_width) ||
      decoded.rows != static_cast<int>(expected_height) || decoded.type() != CV_8UC1) {
    return {};
  }
  if (texture_width != expected_width || texture_height != expected_height) {
    cv::Mat resized;
    cv::resize(
        decoded,
        resized,
        cv::Size(static_cast<int>(texture_width), static_cast<int>(texture_height)),
        0.0,
        0.0,
        cv::INTER_AREA);
    return copy_grayscale_mat(resized, expected_width, expected_height, texture_width, texture_height);
  }
  return copy_grayscale_mat(decoded, expected_width, expected_height, texture_width, texture_height);
}

std::pair<std::uint32_t, std::uint32_t> bounded_texture_dimensions(
    std::uint32_t width,
    std::uint32_t height,
    const RinkMaskLoadOptions& options) {
  if (!options.downsample_to_texture_budget)
    return {width, height};
  if (width == 0 || height == 0 || options.maximum_texture_dimension == 0 || options.maximum_texture_bytes == 0)
    return {0, 0};
  const long double source_pixels = static_cast<long double>(width) * height;
  long double scale = 1.0L;
  scale = std::min(scale, static_cast<long double>(options.maximum_texture_dimension) / width);
  scale = std::min(scale, static_cast<long double>(options.maximum_texture_dimension) / height);
  if (source_pixels > static_cast<long double>(options.maximum_texture_bytes)) {
    scale = std::min(scale, std::sqrt(static_cast<long double>(options.maximum_texture_bytes) / source_pixels));
  }
  if (scale >= 1.0L)
    return {width, height};
  std::uint32_t texture_width = std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(std::floor(width * scale)));
  std::uint32_t texture_height = std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(std::floor(height * scale)));
  texture_width = std::min(texture_width, options.maximum_texture_dimension);
  texture_height = std::min(texture_height, options.maximum_texture_dimension);
  if (static_cast<std::uint64_t>(texture_width) * texture_height > options.maximum_texture_bytes) {
    if (texture_width >= texture_height) {
      texture_width =
          std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(options.maximum_texture_bytes / texture_height));
    } else {
      texture_height =
          std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(options.maximum_texture_bytes / texture_width));
    }
  }
  return {texture_width, texture_height};
}

RinkMaskLoadResult failed(RinkMaskLoadStatus status, const char* message) {
  return RinkMaskLoadResult{status, {}, message};
}

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0)
      ::close(value_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const {
    return value_;
  }

 private:
  int value_{-1};
};

} // namespace

RinkMaskLoadResult load_rink_mask_png(
    const std::string& path,
    const RinkMaskDecoder& decoder,
    const RinkMaskOpenObserver& open_observer,
    const RinkMaskLoadOptions& options) {
  try {
    const int opened = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (opened < 0) {
      if (errno == ELOOP)
        return failed(RinkMaskLoadStatus::kUnsafeFileType, "symbolic links are not accepted");
      return failed(RinkMaskLoadStatus::kMissing, "file could not be opened");
    }
    FileDescriptor input(opened);
    struct stat attributes{};
    if (::fstat(input.get(), &attributes) != 0)
      return failed(RinkMaskLoadStatus::kMissing, "file metadata is unavailable");
    if (!S_ISREG(attributes.st_mode))
      return failed(RinkMaskLoadStatus::kUnsafeFileType, "file is not a regular file");
    if (attributes.st_size < 0)
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "compressed PNG has an invalid size");
    const std::uint64_t file_size = static_cast<std::uint64_t>(attributes.st_size);
    if (file_size > kMaximumRinkMaskCompressedBytes)
      return failed(RinkMaskLoadStatus::kCompressedFileTooLarge, "compressed PNG exceeds 8 MiB");
    if (file_size < kPngHeaderBytes)
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "PNG header is truncated");

    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(file_size));
    if (open_observer)
      open_observer();
    std::size_t offset = 0;
    while (offset < compressed.size()) {
      const ssize_t bytes = ::read(input.get(), compressed.data() + offset, compressed.size() - offset);
      if (bytes > 0) {
        offset += static_cast<std::size_t>(bytes);
        continue;
      }
      if (bytes < 0 && errno == EINTR)
        continue;
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "compressed PNG could not be read completely");
    }
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), compressed.begin()) ||
        read_big_endian_u32(compressed.data() + 8) != 13U || std::memcmp(compressed.data() + 12, "IHDR", 4) != 0) {
      return failed(RinkMaskLoadStatus::kInvalidPngHeader, "PNG signature or IHDR is invalid");
    }

    const std::uint32_t width = read_big_endian_u32(compressed.data() + 16);
    const std::uint32_t height = read_big_endian_u32(compressed.data() + 20);
    if (width == 0 || height == 0 || width > options.maximum_source_dimension ||
        height > options.maximum_source_dimension) {
      return failed(RinkMaskLoadStatus::kDimensionsTooLarge, "PNG dimensions exceed the rink-mask source limit");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (options.downsample_to_texture_budget && pixels > options.maximum_source_pixels)
      return failed(RinkMaskLoadStatus::kResourceBudgetExceeded, "source PNG pixels exceed the preview decode budget");
    const auto [texture_width, texture_height] = bounded_texture_dimensions(width, height, options);
    if (texture_width == 0 || texture_height == 0)
      return failed(RinkMaskLoadStatus::kTextureBudgetExceeded, "GL_ALPHA8 texture exceeds 32 MiB");
    const std::uint64_t texture_pixels = static_cast<std::uint64_t>(texture_width) * texture_height;
    if (texture_width > options.maximum_texture_dimension || texture_height > options.maximum_texture_dimension ||
        texture_pixels > options.maximum_texture_bytes) {
      return failed(RinkMaskLoadStatus::kTextureBudgetExceeded, "GL_ALPHA8 texture exceeds 32 MiB");
    }
    const std::uint64_t resident_bytes = options.downsample_to_texture_budget
        ? pixels + texture_pixels * 2ULL
        : texture_pixels * kResidentCopiesPerPixel;
    if (resident_bytes > options.maximum_resource_bytes || file_size > options.maximum_resource_bytes - resident_bytes) {
      return failed(
          RinkMaskLoadStatus::kResourceBudgetExceeded,
          "compressed, decoded, upload, and texture resources exceed the rink-mask preview budget");
    }

    RinkMaskImage image = decoder ? decoder(compressed, width, height)
                                  : decode_with_opencv(compressed, width, height, texture_width, texture_height);
    if (decoder) {
      image.canvas_width = image.canvas_width ? image.canvas_width : image.width;
      image.canvas_height = image.canvas_height ? image.canvas_height : image.height;
      if (image.width == width && image.height == height && image.alpha.size() == pixels &&
          (texture_width != width || texture_height != height)) {
        image = downsample_rink_mask_image(image, texture_width, texture_height);
      }
    }
    if (image.canvas_width == 0)
      image.canvas_width = image.width;
    if (image.canvas_height == 0)
      image.canvas_height = image.height;
    if (image.canvas_width != width || image.canvas_height != height || image.width != texture_width ||
        image.height != texture_height || image.alpha.size() != texture_pixels) {
      return failed(RinkMaskLoadStatus::kDecodeFailed, "decoder returned empty or inconsistent grayscale pixels");
    }
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

bool rink_mask_dimensions_match(
    const RinkMaskImage& image,
    std::uint32_t expected_width,
    std::uint32_t expected_height) {
  const std::uint32_t canvas_width = image.canvas_width ? image.canvas_width : image.width;
  const std::uint32_t canvas_height = image.canvas_height ? image.canvas_height : image.height;
  return expected_width > 0 && expected_height > 0 && canvas_width == expected_width && canvas_height == expected_height;
}

const char* rink_mask_load_status_name(RinkMaskLoadStatus status) {
  switch (status) {
    case RinkMaskLoadStatus::kLoaded:
      return "loaded";
    case RinkMaskLoadStatus::kMissing:
      return "missing";
    case RinkMaskLoadStatus::kUnsafeFileType:
      return "unsafe-file-type";
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
