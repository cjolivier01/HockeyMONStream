#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace hm::stitching {
namespace {
absl::StatusOr<cv::Mat> download_image(surface::Surface surf) {
  cv::Mat cpu_img;
  if (surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA) {
    cpu_img = cv::Mat(static_cast<int>(surf.height()), static_cast<int>(surf.width()), CV_8UC4);
    const cudaError_t copy_status = cudaMemcpy2D(
        cpu_img.data,
        cpu_img.step,
        surf.dataptr(),
        surf.pitch(),
        cpu_img.cols * cpu_img.elemSize(),
        cpu_img.rows,
        cudaMemcpyDeviceToHost);
    if (copy_status != cudaSuccess) {
      return absl::FailedPreconditionError(
          TO_STRING("Unable to download RGBA calibration image from GPU: " << cudaGetErrorString(copy_status)));
    }
    // This existing calibration-only readback feeds OpenCV encoders/inference,
    // which expect BGR(A), while the GPU surface stores R,G,B,A bytes.
    cv::cvtColor(cpu_img, cpu_img, cv::COLOR_RGBA2BGRA);
  } else if (
      surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709 ||
      surf.get()->colorFormat == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020) {
    std::vector<uint32_t> packed(static_cast<size_t>(surf.width()) * surf.height());
    const cudaError_t copy_status = cudaMemcpy2D(
        packed.data(),
        static_cast<size_t>(surf.width()) * sizeof(uint32_t),
        surf.dataptr(),
        surf.pitch(),
        static_cast<size_t>(surf.width()) * sizeof(uint32_t),
        surf.height(),
        cudaMemcpyDeviceToHost);
    if (copy_status != cudaSuccess) {
      return absl::FailedPreconditionError(
          TO_STRING("Unable to download RGB10 calibration image from GPU: " << cudaGetErrorString(copy_status)));
    }
    cpu_img = cv::Mat(static_cast<int>(surf.height()), static_cast<int>(surf.width()), CV_16UC3);
    constexpr uint32_t kRgb10Mask = 0x3ffu;
    for (int y = 0; y < cpu_img.rows; ++y) {
      auto* row = cpu_img.ptr<cv::Vec3w>(y);
      const auto* packed_row = packed.data() + static_cast<size_t>(y) * surf.width();
      for (int x = 0; x < cpu_img.cols; ++x) {
        const uint32_t pixel = packed_row[x];
        const uint16_t red = static_cast<uint16_t>(((pixel & kRgb10Mask) * 65535u + 511u) / 1023u);
        const uint16_t green = static_cast<uint16_t>(((((pixel >> 10) & kRgb10Mask) * 65535u + 511u) / 1023u));
        const uint16_t blue = static_cast<uint16_t>(((((pixel >> 20) & kRgb10Mask) * 65535u + 511u) / 1023u));
        row[x] = cv::Vec3w(blue, green, red);
      }
    }
  } else {
    return absl::InvalidArgumentError("Invalid image format");
  }
  if (cpu_img.empty()) {
    return absl::FailedPreconditionError("Unable to download image from GPU");
  }
  return cpu_img;
}

absl::Status save_image(surface::Surface surf, const std::string& filename) {
  cv::Mat cpu_img;
  HM_ASSIGN_OR_RETURN(cpu_img, download_image(surf));
  if (!cv::imwrite(filename, cpu_img)) {
    return absl::FailedPreconditionError("Unable to write image to file");
  }
  return absl::OkStatus();
}

CalibrationImageWriter Writer(surface::Surface surface) {
  return [surface](const std::filesystem::path& path) { return save_image(surface, path.string()); };
}
} // namespace

absl::Status save_stitched_image(const std::string& game_dir, surface::Surface surface, const std::string& generation) {
  return save_stitched_image(
      game_dir, surface.width(), surface.height(), [surface] { return download_image(surface); }, generation);
}

absl::Status create_field_mask(
    const std::string& game_dir,
    surface::Surface surface,
    const std::string& expected_output_generation,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    const std::string& expected_output_authorization_id,
    const RinkMaskFrameProvenance& frame) {
  return create_field_mask(
      game_dir,
      Writer(surface),
      expected_output_generation,
      expected_invalidation_id,
      is_cancelled,
      expected_output_authorization_id,
      frame);
}

absl::Status configure_stitching(
    const std::string& game_dir,
    const std::vector<StitchingCalibrationFramePair>& pairs,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    size_t max_output_width,
    const std::string& captured_frame_selection_fingerprint) {
  std::vector<CalibrationFrameInput> inputs;
  inputs.reserve(pairs.size());
  for (const auto& pair : pairs)
    inputs.push_back(
        {Writer(pair.left),
         Writer(pair.right),
         pair.left_source,
         pair.right_source,
         pair.left_image,
         pair.right_image});
  return configure_stitching(
      game_dir, inputs, expected_invalidation_id, is_cancelled, max_output_width, captured_frame_selection_fingerprint);
}

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    size_t max_output_width,
    const std::string& captured_frame_selection_fingerprint) {
  return configure_stitching(
      game_dir,
      std::vector<StitchingCalibrationFramePair>{{left_surface, right_surface}},
      expected_invalidation_id,
      is_cancelled,
      max_output_width,
      captured_frame_selection_fingerprint);
}

} // namespace hm::stitching
