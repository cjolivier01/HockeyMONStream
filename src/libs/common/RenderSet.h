#pragma once

#include <cuda_runtime.h>

#include "hstream/src/libs/common/Surface.h"
#include "jetson-utils/display/glDisplay.h"

#include <map>
#include <mutex>

namespace hm {

class RenderSet {
 public:
  bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream);

 private:
  static std::unique_ptr<glDisplay> create_video_output(const std::string& name, const hm::surface::Surface& surface);
  videoOutput* get_video_output(const std::string& name, const hm::surface::Surface& surface);

  std::mutex mu_;
  std::map<std::string, std::unique_ptr<glDisplay>> video_outputs_;
};

} // namespace hm
