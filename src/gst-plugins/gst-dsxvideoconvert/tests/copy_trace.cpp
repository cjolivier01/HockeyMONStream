/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

namespace {

thread_local int compute_mode = -1;

void trace(const char* text) {
  const char* path = std::getenv("DSX_COPY_TRACE_FILE");
  if (path == nullptr) {
    return;
  }
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return;
  }
  const size_t size = std::strlen(text);
  const ssize_t written = write(descriptor, text, size);
  static_cast<void>(written);
  close(descriptor);
}

template <typename Function>
Function library_symbol(const char* library, const char* name) {
  static void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
  void* symbol = handle == nullptr ? nullptr : dlsym(handle, name);
  if (symbol == nullptr) {
    std::abort();
  }
  return reinterpret_cast<Function>(symbol);
}

} // namespace

extern "C" int Raw2NvBufSurface(
    unsigned char* pointer,
    unsigned int index,
    unsigned int plane,
    unsigned int width,
    unsigned int height,
    NvBufSurface* surface) {
  trace("raw-to-surface\n");
  using Function = int (*)(unsigned char*, unsigned int, unsigned int, unsigned int, unsigned int, NvBufSurface*);
  return library_symbol<Function>("libnvbufsurface.so", "Raw2NvBufSurface")(
      pointer, index, plane, width, height, surface);
}

extern "C" int NvBufSurface2Raw(
    NvBufSurface* surface,
    unsigned int index,
    unsigned int plane,
    unsigned int width,
    unsigned int height,
    unsigned char* pointer) {
  trace("surface-to-raw\n");
  using Function = int (*)(NvBufSurface*, unsigned int, unsigned int, unsigned int, unsigned int, unsigned char*);
  return library_symbol<Function>("libnvbufsurface.so", "NvBufSurface2Raw")(
      surface, index, plane, width, height, pointer);
}

extern "C" NvBufSurfTransform_Error NvBufSurfTransformSetSessionParams(NvBufSurfTransformConfigParams* parameters) {
  compute_mode = parameters == nullptr ? -1 : static_cast<int>(parameters->compute_mode);
  using Function = NvBufSurfTransform_Error (*)(NvBufSurfTransformConfigParams*);
  return library_symbol<Function>("libnvbufsurftransform.so", "NvBufSurfTransformSetSessionParams")(parameters);
}

extern "C" NvBufSurfTransform_Error NvBufSurfTransform(
    NvBufSurface* source,
    NvBufSurface* destination,
    NvBufSurfTransformParams* parameters) {
  char message[32];
  std::snprintf(message, sizeof(message), "transform=%d\n", compute_mode);
  trace(message);
  using Function = NvBufSurfTransform_Error (*)(NvBufSurface*, NvBufSurface*, NvBufSurfTransformParams*);
  return library_symbol<Function>("libnvbufsurftransform.so", "NvBufSurfTransform")(source, destination, parameters);
}
