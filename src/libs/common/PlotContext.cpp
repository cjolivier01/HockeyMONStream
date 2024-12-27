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
    default:
      throw std::runtime_error("invalid variant index");
  }
}

PlotContex::PlotContex(NvDsFrameMeta* frame_meta, const std::string& font_name)
    : frame_meta_(frame_meta), font_name_(font_name) {}

PlotContex::~PlotContex() {
  for (NvDsDisplayMeta* display_meta : display_metas_) {
    // Attach display metadata to the frame
    assert(
        display_meta->num_rects + display_meta->num_lines + display_meta->num_circles + display_meta->num_arrows +
            display_meta->num_labels >
        0);
    nvds_add_display_meta_to_frame(frame_meta_, display_meta);
  }
}

void PlotContex::plot_rect(
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
  if (fill_color.has_value()) {
    rect_params.has_bg_color = true;
    rect_params.bg_color = to_color_params(*fill_color);
  }
}

void PlotContex::plot_line(const Point& from, const Point& to, int thickness, const ColorT& color) {
  std::pair<NvDsDisplayMeta*, size_t> meta = allocate_display_meta(PLOT_TYPE::LINE);
  NvOSD_LineParams& line_params = meta.first->line_params[meta.second];
  line_params.x1 = from.x;
  line_params.y1 = from.y;
  line_params.x2 = to.x;
  line_params.y2 = to.y;
  line_params.line_width = thickness;
  line_params.line_color = to_color_params(color);
}

void PlotContex::plot_circle(
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

void PlotContex::plot_arrow(
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

void PlotContex::plot_text(
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
    text_data.emplace_back(std::move(text));
  }
  // text_params.display_text
  if (bg_color.has_value()) {
    text_params.set_bg_clr = true;
    text_params.text_bg_clr = to_color_params(*bg_color);
  }
}

std::pair<NvDsDisplayMeta*, size_t> PlotContex::allocate_display_meta(PLOT_TYPE type) {
  std::unique_lock lk(mu_);
  size_t current_count = plot_type_counts_.at(type);
  size_t current_meta = current_count / kMaxElementsInDisplayMeta;
  size_t new_index = current_count % kMaxElementsInDisplayMeta == 0;
  if (!new_index) {
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

} // namespace utils
} // namespace hm
