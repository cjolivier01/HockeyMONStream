#include "hstream/src/libs/common/RenderSet.h"

namespace hm {

std::unique_ptr<glDisplay> RenderSet::create_video_output(
    const std::string& name,
    const hm::surface::Surface& surface) {
  videoOptions vo;
  vo.width = (int)surface.pitch_width();
  vo.height = (int)surface.height();
  auto video_output = std::unique_ptr<glDisplay>(glDisplay::Create(vo));
  video_output->SetTitle(name.c_str());
  return video_output;
}

videoOutput* RenderSet::get_video_output(const std::string& name, const hm::surface::Surface& surface) {
  std::unique_lock lk(mu_);
  auto found = video_outputs_.find(name);
  if (found == video_outputs_.end()) {
    found = video_outputs_.emplace(name, create_video_output(name, surface)).first;
  }
  return found->second.get();
}

bool RenderSet::render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
  return get_video_output(name, surface)
      ->Render(surface.dataptr(), surface.pitch_width(), surface.height(), surface.get_image_format(), stream);
}

} // namespace hm
