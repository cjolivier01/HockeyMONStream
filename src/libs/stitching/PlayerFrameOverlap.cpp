#include "hstream/src/libs/stitching/PlayerFrameOverlap.h"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "hstream/src/libs/stitching/ConfigureStitching.h"

namespace hm::stitching {
namespace {
constexpr uint64_t kMaximumPixels = 256ULL * 1024 * 1024;
constexpr int kMaximumDimension = 65536;
constexpr uint16_t kUnmapped = 65535;
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

struct Placement {
  cv::Point2d position;
  cv::Size size;
};

absl::Status invalid(const std::string& message) {
  return absl::FailedPreconditionError("Player overlap: " + message);
}

absl::Status validate_size(cv::Size size) {
  if (size.width <= 0 || size.height <= 0 || size.width > kMaximumDimension || size.height > kMaximumDimension ||
      static_cast<uint64_t>(size.width) * size.height > kMaximumPixels)
    return invalid("mapping dimensions exceed bounds");
  return absl::OkStatus();
}

absl::StatusOr<cv::Size> tiff_size(TIFF* tiff) {
  uint32_t width = 0, height = 0;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) ||
      width > kMaximumDimension || height > kMaximumDimension)
    return invalid("missing or oversized TIFF dimensions");
  cv::Size size(static_cast<int>(width), static_cast<int>(height));
  const auto status = validate_size(size);
  if (!status.ok())
    return status;
  return size;
}

absl::StatusOr<Placement> read_placement(const std::filesystem::path& path) {
  Tiff tiff(TIFFOpen(path.c_str(), "r"), &TIFFClose);
  if (!tiff)
    return invalid("cannot open placement TIFF " + path.string());
  auto size = tiff_size(tiff.get());
  if (!size.ok())
    return size.status();
  float x = 0, y = 0, rx = 0, ry = 0;
  if (!TIFFGetField(tiff.get(), TIFFTAG_XPOSITION, &x) || !TIFFGetField(tiff.get(), TIFFTAG_YPOSITION, &y) ||
      !TIFFGetField(tiff.get(), TIFFTAG_XRESOLUTION, &rx) || !TIFFGetField(tiff.get(), TIFFTAG_YRESOLUTION, &ry) ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(rx) || !std::isfinite(ry) || rx <= 0 || ry <= 0)
    return invalid("invalid placement TIFF offsets/resolution");
  // The production placement reader multiplies these TIFF float tags as floats.
  const float px = x * rx, py = y * ry;
  if (!std::isfinite(px) || !std::isfinite(py) || std::abs(px) > 4.0 * kMaximumDimension ||
      std::abs(py) > 4.0 * kMaximumDimension)
    return invalid("placement offset exceeds bounds");
  return Placement{{px, py}, *size};
}

absl::StatusOr<cv::Size> normalize_placements(std::array<Placement, 2>* placements) {
  const double min_x = std::min((*placements)[0].position.x, (*placements)[1].position.x);
  const double min_y = std::min((*placements)[0].position.y, (*placements)[1].position.y);
  double width = 0, height = 0;
  for (auto& placement : *placements) {
    auto status = validate_size(placement.size);
    if (!status.ok())
      return status;
    placement.position.x -= min_x;
    placement.position.y -= min_y;
    if (!std::isfinite(placement.position.x) || !std::isfinite(placement.position.y))
      return invalid("nonfinite mapping placement");
    width = std::max(width, placement.position.x + placement.size.width);
    height = std::max(height, placement.position.y + placement.size.height);
  }
  if (width < 1 || height < 1 || width > kMaximumDimension || height > kMaximumDimension)
    return invalid("placed mapping canvas exceeds bounds");
  // Match ControlMasks::canvas_width/height and ConfigureStitching's truncation.
  const cv::Size size(static_cast<int>(width), static_cast<int>(height));
  const auto status = validate_size(size);
  if (!status.ok())
    return status;
  return size;
}

absl::StatusOr<cv::Size> mask_size(cv::Size canvas, size_t maximum) {
  auto status = validate_size(canvas);
  if (!status.ok())
    return status;
  if (maximum == 0 || maximum > 2048)
    return invalid("mask dimension limit must be in [1, 2048]");
  const double scale = std::min(1.0, static_cast<double>(maximum) / std::max(canvas.width, canvas.height));
  return cv::Size(
      std::max(1, static_cast<int>(std::ceil(canvas.width * scale))),
      std::max(1, static_cast<int>(std::ceil(canvas.height * scale))));
}

// Each reduced cell is valid only if its entire native-camera footprint is valid.
// An unmapped pixel marks every reduced cell it intersects, so small holes cannot
// disappear as they can with nearest-neighbor mask resizing.
class ValidityReducer {
 public:
  ValidityReducer(Placement placement, cv::Size native, cv::Size reduced)
      : placement_(placement), native_(native), mask_(reduced, CV_8UC1, cv::Scalar(255)) {
    for (int y = 0; y < mask_.rows; ++y) {
      auto* row = mask_.ptr<uint8_t>(y);
      const double top = static_cast<double>(y) * native.height / mask_.rows;
      const double bottom = static_cast<double>(y + 1) * native.height / mask_.rows;
      for (int x = 0; x < mask_.cols; ++x) {
        const double left = static_cast<double>(x) * native.width / mask_.cols;
        const double right = static_cast<double>(x + 1) * native.width / mask_.cols;
        if (left < placement.position.x || top < placement.position.y ||
            right > placement.position.x + placement.size.width ||
            bottom > placement.position.y + placement.size.height)
          row[x] = 0;
      }
    }
  }

  void invalid_pixel(int x, int y) {
    const double left = (placement_.position.x + x) * mask_.cols / native_.width;
    const double top = (placement_.position.y + y) * mask_.rows / native_.height;
    const double right = (placement_.position.x + x + 1) * mask_.cols / native_.width;
    const double bottom = (placement_.position.y + y + 1) * mask_.rows / native_.height;
    const int x0 = std::clamp(static_cast<int>(std::floor(left)), 0, mask_.cols);
    const int x1 = std::clamp(static_cast<int>(std::ceil(right)), 0, mask_.cols);
    const int y0 = std::clamp(static_cast<int>(std::floor(top)), 0, mask_.rows);
    const int y1 = std::clamp(static_cast<int>(std::ceil(bottom)), 0, mask_.rows);
    for (int yy = y0; yy < y1; ++yy)
      std::fill(mask_.ptr<uint8_t>(yy) + x0, mask_.ptr<uint8_t>(yy) + x1, 0);
  }

  const cv::Mat& mask() const {
    return mask_;
  }

 private:
  Placement placement_;
  cv::Size native_;
  cv::Mat mask_;
};

absl::Status read_remap_validity(
    const std::filesystem::path& path,
    cv::Size expected,
    int source_dimension,
    ValidityReducer* reducer) {
  Tiff tiff(TIFFOpen(path.c_str(), "r"), &TIFFClose);
  if (!tiff)
    return invalid("cannot open remap TIFF " + path.string());
  auto size = tiff_size(tiff.get());
  if (!size.ok())
    return size.status();
  uint16_t samples = 0, bits = 0, format = 0, orientation = 0, planar = 0;
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ORIENTATION, &orientation);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  if (*size != expected || samples != 1 || bits != 16 || format != SAMPLEFORMAT_UINT ||
      orientation != ORIENTATION_TOPLEFT || planar != PLANARCONFIG_CONTIG)
    return invalid("remap TIFF format/size differs from placement");
  const auto inspect = [&](const uint16_t* values, int count, int x, int y) {
    for (int i = 0; i < count; ++i) {
      if (values[i] == kUnmapped || values[i] >= source_dimension)
        reducer->invalid_pixel(x + i, y);
    }
  };
  if (TIFFIsTiled(tiff.get())) {
    uint32_t width = 0, height = 0;
    TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &width);
    TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &height);
    const auto bytes = TIFFTileSize(tiff.get());
    if (!width || !height || width > kMaximumDimension || height > kMaximumDimension || bytes <= 0 ||
        static_cast<uint64_t>(bytes) > 16 * 1024 * 1024 || static_cast<uint64_t>(bytes) != width * uint64_t(height) * 2)
      return invalid("remap tile exceeds bounded reader capacity");
    std::vector<uint16_t> tile(static_cast<size_t>(bytes) / 2);
    for (uint32_t y = 0; y < static_cast<uint32_t>(expected.height); y += height) {
      for (uint32_t x = 0; x < static_cast<uint32_t>(expected.width); x += width) {
        if (TIFFReadTile(tiff.get(), tile.data(), x, y, 0, 0) < bytes)
          return invalid("cannot decode complete remap tile");
        for (uint32_t yy = 0; yy < std::min(height, expected.height - y); ++yy)
          inspect(tile.data() + yy * width, std::min(width, expected.width - x), x, y + yy);
      }
    }
  } else {
    const auto bytes = TIFFScanlineSize(tiff.get());
    if (bytes != expected.width * 2)
      return invalid("invalid remap scanline size");
    std::vector<uint16_t> row(expected.width);
    for (int y = 0; y < expected.height; ++y) {
      if (TIFFReadScanline(tiff.get(), row.data(), static_cast<uint32_t>(y), 0) < 0)
        return invalid("cannot decode remap scanline");
      inspect(row.data(), expected.width, 0, y);
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<cv::Mat> rotate_validity(const cv::Mat& mask, cv::Size canvas, double rotation) {
  if (!std::isfinite(rotation) || std::abs(rotation) > 360)
    return invalid("invalid post-stitch rotation");
  if (std::abs(rotation) < 1e-6)
    return mask.clone();
  cv::Mat result(mask.size(), CV_8UC1, cv::Scalar(0));
  const double radians = -rotation * std::acos(-1.0) / 180;
  const double c = std::cos(radians), s = std::sin(radians);
  const double cx = (canvas.width - 1.0) / 2, cy = (canvas.height - 1.0) / 2;
  for (int y = 0; y < result.rows; ++y) {
    auto* row = result.ptr<uint8_t>(y);
    for (int x = 0; x < result.cols; ++x) {
      const double output_x = (x + 0.5) * canvas.width / mask.cols - 0.5;
      const double output_y = (y + 0.5) * canvas.height / mask.rows - 0.5;
      // Exact WarpAffineBack convention from StitcherPriv::apply_post_stitch_rotation.
      const double input_x = c * output_x + s * output_y + (1 - c) * cx - s * cy;
      const double input_y = -s * output_x + c * output_y + s * cx + (1 - c) * cy;
      double mx = (input_x + 0.5) * mask.cols / canvas.width - 0.5;
      double my = (input_y + 0.5) * mask.rows / canvas.height - 0.5;
      // Remove roundoff at exact rotations without rounding interpolated boundaries.
      if (std::abs(mx - std::round(mx)) < 1e-9)
        mx = std::round(mx);
      if (std::abs(my - std::round(my)) < 1e-9)
        my = std::round(my);
      const int x0 = static_cast<int>(std::floor(mx)), y0 = static_cast<int>(std::floor(my));
      const int x1 = static_cast<int>(std::ceil(mx)), y1 = static_cast<int>(std::ceil(my));
      if (x0 >= 0 && y0 >= 0 && x1 < mask.cols && y1 < mask.rows && mask.at<uint8_t>(y0, x0) &&
          mask.at<uint8_t>(y0, x1) && mask.at<uint8_t>(y1, x0) && mask.at<uint8_t>(y1, x1))
        row[x] = 255;
    }
  }
  return result;
}

absl::Status validate_sources(const std::array<cv::Size, 2>& sources) {
  for (const auto& source : sources) {
    if (source.width <= 0 || source.height <= 0 || source.width >= kUnmapped || source.height >= kUnmapped)
      return invalid("invalid raw camera dimensions");
  }
  return absl::OkStatus();
}
} // namespace

absl::StatusOr<PlayerFrameOverlap> BuildPlayerFrameOverlap(
    const std::array<PlayerFrameRemap, 2>& maps,
    cv::Size effective_canvas,
    double post_stitch_rotation_degrees,
    size_t maximum_mask_dimension) {
  auto status = validate_sources({maps[0].source_size, maps[1].source_size});
  if (!status.ok())
    return status;
  std::array<Placement, 2> placements;
  for (size_t camera = 0; camera < 2; ++camera) {
    if (maps[camera].x.empty() || maps[camera].x.type() != CV_16UC1 || maps[camera].y.type() != CV_16UC1 ||
        maps[camera].x.size() != maps[camera].y.size())
      return invalid("invalid X/Y remap planes");
    placements[camera] = {maps[camera].placement, maps[camera].x.size()};
  }
  auto native = normalize_placements(&placements);
  if (!native.ok())
    return native.status();
  auto reduced = mask_size(effective_canvas, maximum_mask_dimension);
  if (!reduced.ok())
    return reduced.status();
  if (effective_canvas.width > native->width || effective_canvas.height > native->height ||
      std::abs(static_cast<double>(effective_canvas.width) * native->height / native->width - effective_canvas.height) >
          1.01)
    return invalid("effective canvas must be a proportional capped native canvas");
  cv::Mat shared(*reduced, CV_8UC1, cv::Scalar(255));
  for (size_t camera = 0; camera < 2; ++camera) {
    ValidityReducer reducer(placements[camera], *native, *reduced);
    for (int y = 0; y < maps[camera].x.rows; ++y) {
      const auto* xs = maps[camera].x.ptr<uint16_t>(y);
      const auto* ys = maps[camera].y.ptr<uint16_t>(y);
      for (int x = 0; x < maps[camera].x.cols; ++x) {
        if (xs[x] == kUnmapped || ys[x] == kUnmapped || xs[x] >= maps[camera].source_size.width ||
            ys[x] >= maps[camera].source_size.height)
          reducer.invalid_pixel(x, y);
      }
    }
    cv::bitwise_and(shared, reducer.mask(), shared);
  }
  auto rotated = rotate_validity(shared, effective_canvas, post_stitch_rotation_degrees);
  if (!rotated.ok())
    return rotated.status();
  return PlayerFrameOverlap{std::move(*rotated), effective_canvas, {}, {}};
}

absl::StatusOr<PlayerFrameOverlap> LoadPlayerFrameOverlap(
    const std::filesystem::path& game_directory,
    const std::array<cv::Size, 2>& source_sizes,
    size_t max_output_width,
    double post_stitch_rotation_degrees,
    size_t maximum_mask_dimension) {
  auto status = validate_sources(source_sizes);
  if (!status.ok())
    return status;
  auto artifacts = lock_stitching_artifacts_for_load(game_directory.string(), max_output_width);
  if (!artifacts.ok())
    return artifacts.status();
  if (!artifacts->artifact_lock || !artifacts->load_snapshot || !artifacts->content_validated)
    return invalid("analysis requires preexisting validated calibration artifacts");
  const auto& directory = artifacts->load_snapshot->directory();
  std::array<Placement, 2> placements;
  for (size_t camera = 0; camera < 2; ++camera) {
    auto placement = read_placement(directory / (camera == 0 ? "mapping_0000.tif" : "mapping_0001.tif"));
    if (!placement.ok())
      return placement.status();
    placements[camera] = *placement;
  }
  auto native = normalize_placements(&placements);
  if (!native.ok())
    return native.status();
  if (artifacts->canvas_size.width != static_cast<size_t>(native->width) ||
      artifacts->canvas_size.height != static_cast<size_t>(native->height))
    return invalid("mapping canvas must already match its validated effective canvas");
  auto reduced = mask_size(*native, maximum_mask_dimension);
  if (!reduced.ok())
    return reduced.status();
  cv::Mat shared(*reduced, CV_8UC1, cv::Scalar(255));
  for (size_t camera = 0; camera < 2; ++camera) {
    ValidityReducer reducer(placements[camera], *native, *reduced);
    const std::string prefix = camera == 0 ? "mapping_0000" : "mapping_0001";
    status = read_remap_validity(
        directory / (prefix + "_x.tif"), placements[camera].size, source_sizes[camera].width, &reducer);
    if (!status.ok())
      return status;
    status = read_remap_validity(
        directory / (prefix + "_y.tif"), placements[camera].size, source_sizes[camera].height, &reducer);
    if (!status.ok())
      return status;
    cv::bitwise_and(shared, reducer.mask(), shared);
  }
  auto rotated = rotate_validity(shared, *native, post_stitch_rotation_degrees);
  if (!rotated.ok())
    return rotated.status();
  status = artifacts->load_snapshot->verify();
  if (!status.ok())
    return status;
  return PlayerFrameOverlap{std::move(*rotated), *native, artifacts->generation_id, artifacts->artifact_revision};
}

} // namespace hm::stitching
