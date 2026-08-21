#include "hstream/src/apps/apps-common/RinkMaskImage.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

class TempDirectory {
 public:
  TempDirectory() {
    path_ = fs::temp_directory_path() /
        ("hstream-rink-mask-test-" + std::to_string(::getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const {
    return path_;
  }

 private:
  fs::path path_;
};

std::array<std::uint8_t, 24> png_header(std::uint32_t width, std::uint32_t height) {
  std::array<std::uint8_t, 24> header = {
      0x89,
      'P',
      'N',
      'G',
      0x0d,
      0x0a,
      0x1a,
      0x0a,
      0,
      0,
      0,
      13,
      'I',
      'H',
      'D',
      'R',
  };
  auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
    header[offset] = static_cast<std::uint8_t>(value >> 24U);
    header[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    header[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    header[offset + 3] = static_cast<std::uint8_t>(value);
  };
  write_u32(16, width);
  write_u32(20, height);
  return header;
}

void write_header(const fs::path& path, std::uint32_t width, std::uint32_t height) {
  const auto header = png_header(width, height);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(header.data()), header.size());
}

bool expect_status(
    const hm::gpu_preview::RinkMaskLoadResult& result,
    hm::gpu_preview::RinkMaskLoadStatus expected,
    const char* description) {
  if (result.status == expected)
    return true;
  std::cerr << description << " returned " << hm::gpu_preview::rink_mask_load_status_name(result.status)
            << " instead of " << hm::gpu_preview::rink_mask_load_status_name(expected) << ": " << result.message
            << '\n';
  return false;
}

} // namespace

int main() {
  TempDirectory temporary;

  const fs::path valid_png = temporary.path() / "valid.png";
  const cv::Mat valid_pixels(4, 5, CV_8UC1, cv::Scalar(173));
  if (!cv::imwrite(valid_png.string(), valid_pixels)) {
    std::cerr << "could not create valid rink-mask PNG fixture\n";
    return 1;
  }
  const auto valid_result = hm::gpu_preview::load_rink_mask_png(valid_png.string());
  if (!valid_result || valid_result.image.width != 5U || valid_result.image.height != 4U ||
      valid_result.image.alpha.size() != 20U || valid_result.image.alpha.front() != 173U) {
    std::cerr << "valid grayscale PNG did not survive bounded decode\n";
    return 1;
  }

  const fs::path oversized_file = temporary.path() / "oversized-file.png";
  {
    std::ofstream output(oversized_file, std::ios::binary);
  }
  fs::resize_file(oversized_file, hm::gpu_preview::kMaximumRinkMaskCompressedBytes + 1U);
  if (!expect_status(
          hm::gpu_preview::load_rink_mask_png(oversized_file.string()),
          hm::gpu_preview::RinkMaskLoadStatus::kCompressedFileTooLarge,
          "oversized compressed file")) {
    return 1;
  }

  const fs::path oversized_dimensions = temporary.path() / "oversized-dimensions.png";
  write_header(oversized_dimensions, hm::gpu_preview::kMaximumRinkMaskDimension + 1U, 1U);
  if (!expect_status(
          hm::gpu_preview::load_rink_mask_png(oversized_dimensions.string()),
          hm::gpu_preview::RinkMaskLoadStatus::kDimensionsTooLarge,
          "oversized dimensions")) {
    return 2;
  }

  const fs::path oversized_texture = temporary.path() / "oversized-texture.png";
  write_header(oversized_texture, 8192U, 4097U);
  if (!expect_status(
          hm::gpu_preview::load_rink_mask_png(oversized_texture.string()),
          hm::gpu_preview::RinkMaskLoadStatus::kTextureBudgetExceeded,
          "oversized texture")) {
    return 3;
  }

  const fs::path oversized_resources = temporary.path() / "oversized-resources.png";
  write_header(oversized_resources, 8192U, 4096U);
  if (!expect_status(
          hm::gpu_preview::load_rink_mask_png(oversized_resources.string()),
          hm::gpu_preview::RinkMaskLoadStatus::kResourceBudgetExceeded,
          "oversized combined resources")) {
    return 4;
  }

  const fs::path corrupt = temporary.path() / "corrupt.png";
  write_header(corrupt, 2U, 2U);
  if (!expect_status(
          hm::gpu_preview::load_rink_mask_png(corrupt.string()),
          hm::gpu_preview::RinkMaskLoadStatus::kDecodeFailed,
          "corrupt PNG")) {
    return 5;
  }

  const hm::gpu_preview::RinkMaskDecoder valid_decoder =
      [](const std::vector<std::uint8_t>&, std::uint32_t width, std::uint32_t height) {
        return hm::gpu_preview::RinkMaskImage{width, height, std::vector<std::uint8_t>(width * height, 255U)};
      };
  const auto loaded = hm::gpu_preview::load_rink_mask_png(corrupt.string(), valid_decoder);
  if (!loaded || loaded.image.width != 2U || loaded.image.height != 2U || loaded.image.alpha.size() != 4U) {
    std::cerr << "bounded valid decoder did not produce a rink-mask image\n";
    return 6;
  }

  const std::vector<hm::gpu_preview::RinkMaskDecoder> throwing_decoders = {
      [](const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t) -> hm::gpu_preview::RinkMaskImage {
        CV_Error(cv::Error::StsError, "injected OpenCV failure");
      },
      [](const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t) -> hm::gpu_preview::RinkMaskImage {
        throw std::bad_alloc();
      },
      [](const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t) -> hm::gpu_preview::RinkMaskImage {
        throw std::runtime_error("injected standard failure");
      },
      [](const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t) -> hm::gpu_preview::RinkMaskImage { throw 7; },
  };
  for (const auto& decoder : throwing_decoders) {
    if (!expect_status(
            hm::gpu_preview::load_rink_mask_png(corrupt.string(), decoder),
            hm::gpu_preview::RinkMaskLoadStatus::kDecodeFailed,
            "throwing decoder")) {
      return 7;
    }
  }

  const std::array<unsigned, 7> retry_delays = {0U, 2U, 4U, 8U, 16U, 30U, 30U};
  for (unsigned failures = 0; failures < retry_delays.size(); ++failures) {
    if (hm::gpu_preview::rink_mask_retry_delay_seconds(failures) != retry_delays[failures]) {
      std::cerr << "rink-mask retry backoff did not remain bounded\n";
      return 8;
    }
  }
  return 0;
}
