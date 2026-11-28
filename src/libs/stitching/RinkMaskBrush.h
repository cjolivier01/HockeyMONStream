#pragma once

#include <cstdint>

#include <opencv2/core.hpp>

namespace hm::stitching {

// The bounded preview and brush use the same native sample at each preview pixel center.
cv::Mat SampleIceBrushPreview(const cv::Mat& mask, cv::Size preview);

// Both points and the positive diameter are in preview pixels. When supplied, erased
// collects the native footprint to clear from secondary detection layers.
void DrawIceBrushSegment(
    cv::Mat& mask,
    cv::Mat* erased,
    cv::Size preview,
    cv::Point start,
    cv::Point end,
    int diameter,
    uint8_t value);

} // namespace hm::stitching
