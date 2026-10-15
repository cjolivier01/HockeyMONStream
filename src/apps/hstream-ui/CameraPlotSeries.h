#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace hm::ui {

// One pixel column of a camera plot, holding the range its frames covered.
// An absent range is a column where the quantity was not available at all,
// which is where a drawing has to break rather than bridge the gap.
struct PlotColumn {
  double x{0};
  std::optional<std::pair<double, double>> range;
};

// Horizontal position of one frame in a plot of count frames spanning left to
// left + width. A lone frame sits in the middle, since it describes no span.
inline double PlotColumnX(std::size_t index, std::size_t count, double left, double width) {
  return count < 2 ? left + width / 2 : left + width * index / (count - 1);
}

// Reduces a per-frame series to one entry per pixel column. A two-minute clip
// holds thousands of frames while the plot is about a thousand pixels wide, so
// drawing every frame costs far more than it shows. Each column keeps the full
// range its frames covered, which preserves a fast swing instead of thinning
// it down to whichever frame happened to land on that pixel.
//
// extent reports one frame as a low and high value, or nothing when the
// quantity is absent for that frame. A column holding only absent frames keeps
// its range unset; a column holding both contributes what it has, because a
// gap narrower than a pixel cannot be drawn anyway.
template <typename Extent>
std::vector<PlotColumn> ReducePlotColumns(std::size_t count, double left, double width, Extent extent) {
  std::vector<PlotColumn> result;
  for (std::size_t i = 0; i < count; ++i) {
    const double x = PlotColumnX(i, count, left, width);
    if (result.empty() || static_cast<int>(result.back().x) != static_cast<int>(x))
      result.push_back({x, std::nullopt});
    const std::optional<std::pair<double, double>> range = extent(i);
    if (!range)
      continue;
    std::optional<std::pair<double, double>>& covered = result.back().range;
    covered = covered ? std::pair<double, double>(
                            std::min(covered->first, range->first), std::max(covered->second, range->second))
                      : *range;
  }
  return result;
}

} // namespace hm::ui
