#pragma once

#include "nvdsmeta.h"

#include <array>
#include <vector>

namespace hm {
namespace utils {

class PlotContex {
 public: 
  PlotContex(NvDsFrameMeta* frame_meta);
  virtual ~PlotContex();
 public:

  enum PLOT_TYPE {
    RECT = 0,
    CIRCLE,
    LINE,
    TEXT,
    ARROW,
    NR_PLOT_TYPES
  };

  std::pair<NvDsDisplayMeta*, size_t> allocate_display_meta(PLOT_TYPE type);


  static inline constexpr size_t kMaxElementsInDisplayMeta = MAX_ELEMENTS_IN_DISPLAY_META;
  NvDsFrameMeta* frame_meta_;
  std::array<size_t, NR_PLOT_TYPES> plot_type_counts_;
  std::vector<NvDsDisplayMeta*> display_metas_;
};

} // namespace utils
} // namespace hm
