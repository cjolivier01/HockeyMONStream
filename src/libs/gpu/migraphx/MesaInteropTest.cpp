#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <migraphx/migraphx.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "hstream/src/libs/gpu/Runtime.h"
void check(bool ok) {
  if (!ok) {
    fprintf(stderr, "MIGraphX/VA/EGL/GL coexistence check failed: %x\n", eglGetError());
    exit(1);
  }
}
void gpu() {
  migraphx_target_t target{};
  check(migraphx_target_create(&target, "gpu") == migraphx_status_success);
  migraphx_target_destroy(target);
}
int main(int argc, char**) {
  if (argc == 1)
    gpu();
  int device;
  char pci[32]{};
  check(hipGetDevice(&device) == hipSuccess);
  check(hipDeviceGetPCIBusId(pci, sizeof(pci), device) == hipSuccess);
  const std::string path = std::string("/dev/dri/by-path/pci-") + pci + "-render";
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  check(fd >= 0);
  auto va = vaGetDisplayDRM(fd);
  int va_major, va_minor;
  check(va && vaInitialize(va, &va_major, &va_minor) == VA_STATUS_SUCCESS);
  auto* gbm = gbm_create_device(fd);
  check(gbm);
  auto display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  EGLint major, minor, count;
  check(eglInitialize(display, &major, &minor));
  check(eglBindAPI(EGL_OPENGL_ES_API));
  EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
  EGLConfig config;
  check(eglChooseConfig(display, attrs, &config, 1, &count) && count);
  EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  check(context != EGL_NO_CONTEXT);
  check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
  if (argc != 1)
    gpu();
  GLuint framebuffer, texture;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glClearColor(.25, .5, .75, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  // Test-only four-byte readback of a synthetic FBO, never a video frame.
  unsigned char pixel[4]{};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  check(glGetError() == GL_NO_ERROR);
  check(pixel[0] >= 63 && pixel[0] <= 64 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] >= 191 && pixel[2] <= 192);
  printf("MIGraphX and %s rendered correctly\n", glGetString(GL_RENDERER));
  glDeleteTextures(1, &texture);
  glDeleteFramebuffers(1, &framebuffer);
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglTerminate(display);
  gbm_device_destroy(gbm);
  check(vaTerminate(va) == VA_STATUS_SUCCESS);
  close(fd);
}
