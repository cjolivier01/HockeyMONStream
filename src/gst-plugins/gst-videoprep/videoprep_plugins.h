#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/hmcustomlib_factory.hpp"

#include <string>

namespace hm {
namespace videoprep {

class VideoPrepLibrary_Factory : public DSCustomLibrary_Factory {
 public:
  VideoPrepLibrary_Factory() {}

  ~VideoPrepLibrary_Factory() {}

  IDSCustomLibrary* CreateCustomAlgoCtx(std::string libName, GObject* object, int gpu_id, size_t batch_size);

 public:
  void* m_libHandle;
  std::string m_libName;
};

} // namespace videoprep
} // namespace hm
