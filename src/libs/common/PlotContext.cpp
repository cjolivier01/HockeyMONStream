#include "libs/common/PlotContext.h"

#include <cassert>

namespace hm {
namespace utils {

PlotContex::PlotContex(NvDsFrameMeta* frame_meta) : frame_meta_(frame_meta) {}

PlotContex::~PlotContex() {
  for (NvDsDisplayMeta* display_meta : display_metas_) {
    // Attach display metadata to the frame
    nvds_add_display_meta_to_frame(frame_meta_, display_meta);
  }
}

std::pair<NvDsDisplayMeta*, size_t> PlotContex::allocate_display_meta(PLOT_TYPE type) {
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
