#include "libs/common/PlotContext.h"

#include <cassert>
#include <stdexcept>

namespace hm {

namespace utils {

NvOSD_ColorParams to_color_params(const ColorT& color) {
  NvOSD_ColorParams params;
  memset(&params, 0, sizeof(params));
  switch (color.index()) {
    case 0: {
      const ColorRGB& clr = std::get<ColorRGB>(color);
      return NvOSD_ColorParams{
          .red = double(clr[0]) / 255.0,
          .green = double(clr[1]) / 255.0,
          .blue = double(clr[2]) / 255.0,
          .alpha = 1.0,
      };
    }
    case 1: {
      const ColorRGBA& clr = std::get<ColorRGBA>(color);
      return NvOSD_ColorParams{
          .red = double(clr[0]) / 255.0,
          .green = double(clr[1]) / 255.0,
          .blue = double(clr[2]) / 255.0,
          .alpha = double(clr[3]) / 255.0,
      };
    }
    default:
      throw std::runtime_error("invalid variant index");
  }
}

PlotContext::PlotContext(NvDsFrameMeta* frame_meta, const std::string& font_name)
    : frame_meta_(frame_meta), font_name_(font_name) {
  reset();
}

PlotContext::~PlotContext() {
  apply();
}

void PlotContext::reset() {
  std::unique_lock lk(mu_);
  for (auto& v : plot_type_counts_) {
    v = 0;
  }
  display_metas_.clear();
  text_data_.clear();
}

void PlotContext::apply() {
  for (NvDsDisplayMeta* display_meta : display_metas_) {
    // Attach display metadata to the frame
    assert(
        (display_meta->num_rects + display_meta->num_lines + display_meta->num_circles + display_meta->num_arrows +
         display_meta->num_labels) > 0);
    nvds_add_display_meta_to_frame(frame_meta_, display_meta);
  }
  reset();
}

void PlotContext::plot_rect(
    const BBox& rect,
    int thickness,
    const ColorT& color,
    const std::optional<ColorT>& fill_color) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::RECT);
  NvOSD_RectParams& rect_params = meta.first->rect_params[meta.second];
  rect_params.border_color = to_color_params(color);
  rect_params.left = rect.left;
  rect_params.top = rect.top;
  rect_params.width = rect.width();
  rect_params.height = rect.height();
  rect_params.border_width = thickness;
  if (fill_color.has_value()) {
    rect_params.has_bg_color = true;
    rect_params.bg_color = to_color_params(*fill_color);
  }
}

void PlotContext::plot_line(const Point& from, const Point& to, int thickness, const ColorT& color) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::LINE);
  NvOSD_LineParams& line_params = meta.first->line_params[meta.second];
  line_params.x1 = from.x;
  line_params.y1 = from.y;
  line_params.x2 = to.x;
  line_params.y2 = to.y;
  line_params.line_width = thickness;
  line_params.line_color = to_color_params(color);
}

void PlotContext::plot_circle(
    const Point center,
    int radius,
    int thickness,
    const ColorT& color,
    const std::optional<ColorT>& fill_color) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::CIRCLE);
  NvOSD_CircleParams& circle_params = meta.first->circle_params[meta.second];
  circle_params.xc = center.x;
  circle_params.yc = center.y;
  circle_params.radius = radius;
  circle_params.circle_width = thickness;
  circle_params.circle_color = to_color_params(color);
  if (fill_color.has_value()) {
    circle_params.has_bg_color = true;
    circle_params.bg_color = to_color_params(*fill_color);
  }
}

void PlotContext::plot_arrow(
    const Point& from,
    const Point& to,
    int thickness,
    const ColorT& color,
    NvOSD_Arrow_Head_Direction arrow_direction) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::ARROW);
  NvOSD_ArrowParams& arrow_params = meta.first->arrow_params[meta.second];
  arrow_params.x1 = from.x;
  arrow_params.y1 = from.y;
  arrow_params.x2 = to.x;
  arrow_params.y2 = to.y;
  arrow_params.arrow_width = thickness;
  arrow_params.arrow_color = to_color_params(color);
  arrow_params.arrow_head = arrow_direction;
}

void PlotContext::plot_text(
    const std::string& label,
    const Point& top_left,
    int font_size,
    const ColorT& color,
    const std::optional<ColorT>& bg_color) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::TEXT);
  NvOSD_TextParams& text_params = meta.first->text_params[meta.second];
  text_params.x_offset = top_left.x;
  text_params.y_offset = top_left.y;
  text_params.font_params.font_name = const_cast<char*>(font_name_.c_str());
  text_params.font_params.font_color = to_color_params(color);
  std::unique_ptr<char[]> text = std::make_unique<char[]>(label.size() + 1);
  strcpy(text.get(), label.c_str());
  text_params.display_text = text.get();
  {
    std::unique_lock lk(mu_);
    text_data_.emplace_back(std::move(text));
  }
  // text_params.display_text
  if (bg_color.has_value()) {
    text_params.set_bg_clr = true;
    text_params.text_bg_clr = to_color_params(*bg_color);
  }
}

std::pair<NvDsDisplayMeta*, size_t> PlotContext::allocate_display_meta(PLOT_TYPE type) {
  std::unique_lock lk(mu_);
  size_t current_count = plot_type_counts_.at(type);
  size_t current_meta = current_count / kMaxElementsInDisplayMeta;
  size_t new_index = current_count % kMaxElementsInDisplayMeta;
  if (current_meta >= display_metas_.size() && !new_index) {
    // Allocate a new one
    NvDsDisplayMeta* display_meta = nvds_acquire_display_meta_from_pool(frame_meta_->base_meta.batch_meta);
    display_metas_.emplace_back(display_meta);
  }
  ++plot_type_counts_.at(type);
  NvDsDisplayMeta* display_meta = display_metas_.at(current_meta);
  switch (type) {
    case PLOT_TYPE::RECT:
      assert(new_index == display_meta->num_rects);
      ++display_meta->num_rects;
      break;
    case PLOT_TYPE::LINE:
      assert(new_index == display_meta->num_lines);
      ++display_meta->num_lines;
      break;
    case PLOT_TYPE::CIRCLE:
      assert(new_index == display_meta->num_circles);
      ++display_meta->num_circles;
      break;
    case PLOT_TYPE::ARROW:
      assert(new_index == display_meta->num_arrows);
      ++display_meta->num_arrows;
      break;
    case PLOT_TYPE::TEXT:
      assert(new_index == display_meta->num_labels);
      ++display_meta->num_labels;
      break;
    default:
      assert(false);
      break;
  }

  return std::make_pair(display_meta, new_index);
}

void PlotContext::plot_dashed_line(
    const Point& from,
    const Point& to,
    int thickness,
    const ColorT& color,
    int dash_length,
    int gap_length) {
  // Calculate the total line length
  float line_length = std::sqrt(std::pow(to.x - from.x, 2) + std::pow(to.y - from.y, 2));

  // Calculate the unit vector direction
  float dx = (to.x - from.x) / line_length;
  float dy = (to.y - from.y) / line_length;

  // Plot dashes and gaps
  float current_length = 0.0;
  Point start = from;
  while (current_length < line_length) {
    // Calculate end point of the current dash
    float dash_end_length = std::min(current_length + dash_length, line_length);
    Point end = {from.x + dx * dash_end_length, from.y + dy * dash_end_length};

    // Plot the dash
    plot_line(start, end, thickness, color);

    // Move to the next starting point (gap end)
    current_length = dash_end_length + gap_length;
    start = {from.x + dx * current_length, from.y + dy * current_length};
  }
}

void PlotContext::plot_dashed_rect(
    const BBox& rect,
    int thickness,
    const ColorT& color,
    int dash_length,
    int gap_length) {
  // Extract rectangle corners
  const Point top_left = {rect.left, rect.top};
  const Point top_right = {rect.right, rect.top};
  const Point bottom_left = {rect.left, rect.bottom};
  const Point bottom_right = {rect.right, rect.bottom};

  // Plot the dashed lines for each side of the rectangle
  plot_dashed_line(top_left, top_right, thickness, color, dash_length, gap_length); // Top edge
  plot_dashed_line(top_right, bottom_right, thickness, color, dash_length, gap_length); // Right edge
  plot_dashed_line(bottom_right, bottom_left, thickness, color, dash_length, gap_length); // Bottom edge
  plot_dashed_line(bottom_left, top_left, thickness, color, dash_length, gap_length); // Left edge
}

void PlotContext::plot_corner_rect(
    const BBox& rect,
    int thickness,
    const ColorT& color,
    float width_ratio,
    float height_ratio) {
  // Extract rectangle corners
  const Point top_left = {rect.left, rect.top};
  const Point top_right = {rect.right, rect.top};
  const Point bottom_left = {rect.left, rect.bottom};
  const Point bottom_right = {rect.right, rect.bottom};

  // Extract rectangle dimensions
  int width = bottom_right.x - top_left.x;
  int height = bottom_right.y - top_left.y;

  // Calculate lengths of corner segments
  int corner_width = static_cast<int>(width * width_ratio);
  int corner_height = static_cast<int>(height * height_ratio);

  // Draw the four corners
  // Top-left corner
  plot_line(top_left, {top_left.x + corner_width, top_left.y}, thickness, color); // Horizontal
  plot_line(top_left, {top_left.x, top_left.y + corner_height}, thickness, color); // Vertical

  // Top-right corner
  plot_line(top_right, {top_right.x - corner_width, top_right.y}, thickness, color); // Horizontal
  plot_line(top_right, {top_right.x, top_right.y + corner_height}, thickness, color); // Vertical

  // Bottom-left corner
  plot_line(bottom_left, {bottom_left.x + corner_width, bottom_left.y}, thickness, color); // Horizontal
  plot_line(bottom_left, {bottom_left.x, bottom_left.y - corner_height}, thickness, color); // Vertical

  // Bottom-right corner
  plot_line(bottom_right, {bottom_right.x - corner_width, bottom_right.y}, thickness, color); // Horizontal
  plot_line(bottom_right, {bottom_right.x, bottom_right.y - corner_height}, thickness, color); // Vertical
}

void PlotContext::plot_no_corner_rect(
    const BBox& rect,
    int thickness,
    const ColorT& color,
    float width_ratio,
    float height_ratio) {
  // Extract rectangle corners
  const Point top_left = {rect.left, rect.top};
  const Point top_right = {rect.right, rect.top};
  const Point bottom_left = {rect.left, rect.bottom};
  const Point bottom_right = {rect.right, rect.bottom};

  // Extract rectangle dimensions
  int width = bottom_right.x - top_left.x;
  int height = bottom_right.y - top_left.y;

  // Calculate lengths of corner segments
  int corner_width = static_cast<int>(width * width_ratio);
  int corner_height = static_cast<int>(height * height_ratio);

  // Draw the four corners
  // Top-left corner
  plot_line(
      {top_left.x + corner_width, top_left.y},
      {top_right.x - corner_width, top_right.y},
      thickness,
      color); // Top Horizontal
  plot_line(
      {top_left.x, top_left.y + corner_height},
      {bottom_left.x, bottom_left.y - corner_height},
      thickness,
      color); // Left Vertical
  plot_line(
      {bottom_left.x + corner_width, bottom_left.y},
      {bottom_right.x - corner_width, bottom_right.y},
      thickness,
      color); // Bottom Horizontal
  plot_line(
      {top_right.x, top_right.y + corner_height},
      {bottom_right.x, bottom_right.y - corner_height},
      thickness,
      color); // Right Vertical
}

} // namespace utils
} // namespace hm
