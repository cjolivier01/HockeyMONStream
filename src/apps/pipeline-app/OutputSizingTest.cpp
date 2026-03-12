#include "OutputSizing.h"

#include <iostream>

int main() {
  {
    auto sizing = hm::compute_output_sizing(1920, 1080, 1280, std::nullopt);
    if (!sizing.ok() || sizing->content_width != 1280 || sizing->content_height != 720 || sizing->has_letterbox()) {
      std::cerr << "Width-only sizing mismatch" << std::endl;
      return 1;
    }
  }

  {
    auto sizing = hm::compute_output_sizing(1920, 1080, std::nullopt, 720);
    if (!sizing.ok() || sizing->content_width != 1280 || sizing->content_height != 720 || sizing->has_letterbox()) {
      std::cerr << "Height-only sizing mismatch" << std::endl;
      return 1;
    }
  }

  {
    auto sizing = hm::compute_output_sizing(1920, 1080, 1000, 1000);
    if (!sizing.ok() || sizing->content_width != 1000 || sizing->content_height != 564 ||
        sizing->final_width() != 1000 || sizing->final_height() != 1000 || !sizing->has_letterbox()) {
      std::cerr << "Square letterbox sizing mismatch" << std::endl;
      return 1;
    }
    if (hm::centered_dest_crop_string(*sizing) != "0:218:1000:564") {
      std::cerr << "Square letterbox crop mismatch" << std::endl;
      return 1;
    }
  }

  {
    auto sizing = hm::compute_output_sizing(1920, 1080, 1001, std::nullopt);
    if (!sizing.ok() || sizing->content_width != 1002 || sizing->content_height != 564) {
      std::cerr << "Odd width coercion mismatch" << std::endl;
      return 1;
    }
  }

  return 0;
}
