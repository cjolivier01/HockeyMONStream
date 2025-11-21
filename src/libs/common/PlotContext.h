#pragma once

#include "hockeymom/csrc/play_tracker/BoxUtils.h"

#include "nvdsmeta.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace hm {
namespace utils {

using ColorRGB = std::array<uint8_t, 3>;
using ColorRGBA = std::array<uint8_t, 4>;
using ColorT = std::variant<ColorRGB, ColorRGBA>;

class PlotContext {
 public:
  PlotContext(NvDsFrameMeta* frame_meta, std::string font_name = "");
  virtual ~PlotContext();

  void plot_rect(
      const BBox& rect,
      int thickness,
      const ColorT& color,
      const std::optional<ColorT>& fill_color = std::nullopt);
  void plot_line(const Point& from, const Point& to, int thickness, const ColorT& color);
  void plot_circle(
      const Point center,
      int radius,
      int thickness,
      const ColorT& color,
      const std::optional<ColorT>& fill_color = std::nullopt);
  void plot_arrow(
      const Point& from,
      const Point& to,
      int thickness,
      const ColorT& color,
      NvOSD_Arrow_Head_Direction arrow_direction = END_HEAD);
  void plot_text(
      const std::string& label,
      const Point& top_left,
      int font_size,
      const ColorT& color,
      const std::optional<ColorT>& bg_color = std::nullopt);

  // Dashed
  void plot_dashed_line(
      const Point& from,
      const Point& to,
      int thickness,
      const ColorT& color,
      int dash_length,
      int gap_length);
  void plot_dashed_rect(const BBox& rect, int thickness, const ColorT& color, int dash_length, int gap_length);

  // Other misc
  void plot_corner_rect(const BBox& rect, int thickness, const ColorT& color, float width_ratio, float height_ratio);
  void plot_no_corner_rect(const BBox& rect, int thickness, const ColorT& color, float width_ratio, float height_ratio);

  // Apply/reset
  void apply();
  void reset();

 public:
  enum PLOT_TYPE { RECT = 0, CIRCLE, LINE, TEXT, ARROW, NR_PLOT_TYPES };

  static void nv_ds_release_func(gpointer data, gpointer user_data);

  std::pair<NvDsDisplayMeta*, size_t> allocate_display_meta(PLOT_TYPE type);

  static inline constexpr size_t kMaxElementsInDisplayMeta = MAX_ELEMENTS_IN_DISPLAY_META;
  static inline NvDsMetaReleaseFunc release_display_meta_fn{nullptr};
  NvDsFrameMeta* frame_meta_;
  const std::string font_name_;
  std::mutex mu_;
  std::array<size_t, NR_PLOT_TYPES> plot_type_counts_;
  std::vector<NvDsDisplayMeta*> display_metas_;
  // std::list<std::unique_ptr<char[]>> text_data_;
};

} // namespace utils
} // namespace hm
