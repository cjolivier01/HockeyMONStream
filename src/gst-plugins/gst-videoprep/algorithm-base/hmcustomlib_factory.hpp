#pragma once

#include <dlfcn.h>

#include <functional>
#include <stdexcept>

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/hmcustomlib_interface.hpp"

namespace hm {

template <class T>
T* dlsym_ptr(void* handle, char const* name) {
  return reinterpret_cast<T*>(dlsym(handle, name));
}

class DSCustomLibrary_Factory {
 public:
  DSCustomLibrary_Factory() : m_libHandle(nullptr) {}

  ~DSCustomLibrary_Factory() {
    if (m_libHandle) {
      dlclose(m_libHandle);
      m_libHandle = NULL;
      m_libName.clear();
    }
  }

  IDSCustomLibrary* CreateCustomAlgoCtx(std::string libName, GObject* object) {
    m_libName.assign(libName);

    m_libHandle = dlopen(m_libName.c_str(), RTLD_NOW);
    std::function<IDSCustomLibrary*(GObject*)> createAlgoCtx = nullptr;
    if (m_libHandle) {
      createAlgoCtx = dlsym_ptr<IDSCustomLibrary*(GObject*)>(m_libHandle, "CreateCustomAlgoCtx");
      if (!createAlgoCtx) {
        throw std::runtime_error("createCustomAlgoCtx function not found in library");
      }
    } else {
      throw std::runtime_error(dlerror());
    }

    return createAlgoCtx ? createAlgoCtx(object) : nullptr;
  }

 public:
  void* m_libHandle;
  std::string m_libName;
};

} // namespace hm
