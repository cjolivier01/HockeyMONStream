#include "src/apps/hstream-ui/CameraPlotSeries.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using hm::ui::PlotColumn;
using hm::ui::PlotColumnX;
using hm::ui::ReducePlotColumns;

using Range = std::optional<std::pair<double, double>>;

int failures = 0;

void require(bool condition, const std::string& what) {
  if (condition)
    return;
  std::cerr << "FAIL: " << what << '\n';
  ++failures;
}

void close(double actual, double expected, const std::string& what) {
  require(std::fabs(actual - expected) < 1e-9, what + " (got " + std::to_string(actual) + ")");
}

// A flat series: every frame reports the same single value.
Range flat(double value) {
  return std::pair<double, double>(value, value);
}

void positions() {
  close(PlotColumnX(0, 1, 10, 100), 60, "A lone frame sits in the middle");
  close(PlotColumnX(0, 4, 10, 90), 10, "The first frame sits at the left edge");
  close(PlotColumnX(3, 4, 10, 90), 100, "The last frame sits at the right edge");
  close(PlotColumnX(1, 4, 10, 90), 40, "Frames spread evenly across the width");
}

void empty_and_single() {
  require(ReducePlotColumns(0, 0, 100, [](std::size_t) { return flat(1); }).empty(), "No frames reduce to no columns");
  const auto one = ReducePlotColumns(1, 10, 100, [](std::size_t) { return flat(7); });
  require(one.size() == 1 && one[0].range && one[0].range->first == 7, "A single frame keeps its value");
  close(one[0].x, 60, "A single frame is centered");
}

void sparse_keeps_every_frame() {
  // Five frames across 400 pixels land in five distinct columns.
  const auto columns = ReducePlotColumns(5, 0, 400, [](std::size_t i) { return flat(double(i)); });
  require(columns.size() == 5, "Fewer frames than pixels keeps one column each");
  for (std::size_t i = 0; i < columns.size(); ++i)
    require(columns[i].range && columns[i].range->first == double(i), "Sparse frames keep their own values");
  close(columns.front().x, 0, "Sparse series starts at the left edge");
  close(columns.back().x, 400, "Sparse series ends at the right edge");
}

void dense_is_bounded_by_width() {
  const auto columns = ReducePlotColumns(20000, 0, 300, [](std::size_t i) { return flat(double(i)); });
  require(
      columns.size() <= 302,
      "A dense series is bound by the plot width, not the frame count: " + std::to_string(columns.size()));
  require(columns.size() >= 300, "A dense series still fills the plot: " + std::to_string(columns.size()));
  require(columns.front().range && columns.front().range->first == 0, "The first column keeps the first value");
  require(
      columns.back().range && columns.back().range->second == 19999, "The last column keeps the last value");
}

void column_covers_the_range_of_its_frames() {
  // 1000 frames over 10 pixels: each column swallows about 100 frames, and a
  // swing inside one column has to survive as that column's range.
  const auto columns = ReducePlotColumns(1000, 0, 10, [](std::size_t i) -> Range {
    return std::pair<double, double>(double(i), double(i) + 5);
  });
  require(columns.size() <= 12, "Dense reduction collapses to the pixel columns");
  for (const PlotColumn& c : columns)
    require(c.range && c.range->second - c.range->first >= 5, "A column spans at least one frame's own range");
  double low = columns.front().range->first;
  double high = columns.front().range->second;
  for (const PlotColumn& c : columns) {
    require(c.range->first >= low, "Column lows advance with the series");
    require(c.range->second >= high, "Column highs advance with the series");
    low = c.range->first;
    high = c.range->second;
  }
  require(columns.front().range->first == 0, "The widest low is kept");
  require(columns.back().range->second == 1004, "The widest high is kept");
}

void absent_frames_leave_a_gap() {
  // Frames 4 through 7 of 12 report nothing. Spread over a wide plot each
  // frame owns a column, so the gap has to appear as unset ranges.
  const auto columns = ReducePlotColumns(12, 0, 600, [](std::size_t i) -> Range {
    if (i >= 4 && i <= 7)
      return std::nullopt;
    return flat(double(i));
  });
  require(columns.size() == 12, "Every sparse frame still owns a column");
  for (std::size_t i = 0; i < columns.size(); ++i)
    require(columns[i].range.has_value() == (i < 4 || i > 7), "The gap is exactly the absent frames");
}

void all_absent_draws_nothing() {
  const auto columns = ReducePlotColumns(50, 0, 200, [](std::size_t) { return Range{}; });
  require(!columns.empty(), "An absent quantity still occupies its columns");
  for (const PlotColumn& c : columns)
    require(!c.range, "An absent quantity leaves every column unset");
}

void a_mixed_column_keeps_what_it_has() {
  // 100 frames over 2 pixels, only frame 50 present: the column that holds it
  // reports it rather than being treated as a gap, because a gap narrower than
  // a pixel cannot be drawn.
  const auto columns = ReducePlotColumns(100, 0, 2, [](std::size_t i) -> Range {
    return i == 50 ? flat(42) : Range{};
  });
  int with_range = 0;
  for (const PlotColumn& c : columns)
    if (c.range) {
      ++with_range;
      require(c.range->first == 42 && c.range->second == 42, "A mixed column reports the value it saw");
    }
  require(with_range == 1, "Exactly the column holding the value reports one");
}

} // namespace

int main() {
  positions();
  empty_and_single();
  sparse_keeps_every_frame();
  dense_is_bounded_by_width();
  column_covers_the_range_of_its_frames();
  absent_frames_leave_a_gap();
  all_absent_draws_nothing();
  a_mixed_column_keeps_what_it_has();
  if (failures) {
    std::cerr << failures << " camera plot series checks failed\n";
    return 1;
  }
  std::cout << "PASS: camera plot series positions, sparse and dense reduction, ranges, gaps\n";
  return 0;
}
