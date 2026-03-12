#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm {

struct OutputSizing {
  long content_width{0};
  long content_height{0};
  std::optional<long> canvas_width;
  std::optional<long> canvas_height;

  long final_width() const {
    return canvas_width.value_or(content_width);
  }

  long final_height() const {
    return canvas_height.value_or(content_height);
  }

  bool has_letterbox() const {
    return canvas_width.has_value() && canvas_height.has_value();
  }
};

inline long coerce_even_up(long value) {
  return (value % 2 == 0) ? value : (value + 1);
}

inline absl::StatusOr<OutputSizing> compute_output_sizing(
    long width,
    long height,
    const std::optional<int>& target_width,
    const std::optional<int>& target_height) {
  if (width <= 0 || height <= 0) {
    return absl::InvalidArgumentError("Base output dimensions must be positive");
  }

  auto validate_positive = [](long value, const char* label) -> absl::Status {
    if (value <= 0) {
      return absl::InvalidArgumentError(std::string(label) + " must be positive");
    }
    return absl::OkStatus();
  };

  OutputSizing sizing;
  if (!target_width.has_value() && !target_height.has_value()) {
    sizing.content_width = width;
    sizing.content_height = height;
    return sizing;
  }

  if (!target_width.has_value()) {
    absl::Status status = validate_positive(*target_height, "output_height");
    if (!status.ok()) {
      return status;
    }
    sizing.content_height = coerce_even_up(*target_height);
    sizing.content_width = coerce_even_up(std::lround(double(width) * double(sizing.content_height) / double(height)));
    return sizing;
  }

  if (!target_height.has_value()) {
    absl::Status status = validate_positive(*target_width, "output_width");
    if (!status.ok()) {
      return status;
    }
    sizing.content_width = coerce_even_up(*target_width);
    sizing.content_height = coerce_even_up(std::lround(double(height) * double(sizing.content_width) / double(width)));
    return sizing;
  }

  absl::Status width_status = validate_positive(*target_width, "output_width");
  if (!width_status.ok()) {
    return width_status;
  }
  absl::Status height_status = validate_positive(*target_height, "output_height");
  if (!height_status.ok()) {
    return height_status;
  }

  const long canvas_w = coerce_even_up(*target_width);
  const long canvas_h = coerce_even_up(*target_height);
  const double scale = std::min(double(canvas_w) / double(width), double(canvas_h) / double(height));
  sizing.content_width = coerce_even_up(std::max<long>(1, std::lround(double(width) * scale)));
  sizing.content_height = coerce_even_up(std::max<long>(1, std::lround(double(height) * scale)));
  sizing.content_width = std::min(sizing.content_width, canvas_w);
  sizing.content_height = std::min(sizing.content_height, canvas_h);

  if (sizing.content_width != canvas_w || sizing.content_height != canvas_h) {
    sizing.canvas_width = canvas_w;
    sizing.canvas_height = canvas_h;
  }
  return sizing;
}

inline std::string centered_dest_crop_string(const OutputSizing& sizing) {
  if (!sizing.has_letterbox()) {
    return {};
  }
  const long canvas_w = *sizing.canvas_width;
  const long canvas_h = *sizing.canvas_height;
  const long left = std::max<long>(0, (canvas_w - sizing.content_width) / 2);
  const long top = std::max<long>(0, (canvas_h - sizing.content_height) / 2);
  return std::to_string(left) + ":" + std::to_string(top) + ":" + std::to_string(sizing.content_width) + ":" +
      std::to_string(sizing.content_height);
}

} // namespace hm
