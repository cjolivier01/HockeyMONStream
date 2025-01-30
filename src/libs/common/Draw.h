/**
 * @file Draw.h
 * @brief Draws on a Surface
 */
#pragma once

#include "Surface.h"

#include <optional>

namespace hm {
namespace surface {

cudaError_t draw_rect(
    surface::Surface& surface,
    const hm::BBox& rect,
    const float4& color,
    int thickness,
    cudaStream_t stream,
    const std::optional<float4>& fill_color = std::nullopt);

} // namespace surface
} // namespace hm
