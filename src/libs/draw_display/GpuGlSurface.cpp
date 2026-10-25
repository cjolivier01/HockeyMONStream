#include "GpuGlSurface.h"

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glx.h>

#if defined(HSTREAM_USE_HIP)
#include <fcntl.h>
#include <gbm.h>
#include <hip/hip_gl_interop.h>
#include <unistd.h>
#define cudaGraphicsResource_t hipGraphicsResource_t
#define cudaArray_t hipArray_t
#define cudaGraphicsGLRegisterImage hipGraphicsGLRegisterImage
#define cudaGraphicsMapResources hipGraphicsMapResources
#define cudaGraphicsSubResourceGetMappedArray hipGraphicsSubResourceGetMappedArray
#define cudaGraphicsUnmapResources hipGraphicsUnmapResources
#define cudaGraphicsUnregisterResource hipGraphicsUnregisterResource
#define cudaGraphicsRegisterFlagsNone hipGraphicsRegisterFlagsNone
#define cudaGLGetDevices hipGLGetDevices
#define cudaGLDeviceListAll hipGLDeviceListAll
#define cudaMemcpy2DToArrayAsync hipMemcpy2DToArrayAsync
#define cudaMemcpy2DFromArrayAsync hipMemcpy2DFromArrayAsync
#else
#include <cuda_gl_interop.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <string>

namespace hm::draw_display {
namespace {

// EGL device displays are process-wide, including across independent preview
// targets. Keep a bounded set of initialized connections for process lifetime;
// calling eglTerminate in one painter would invalidate another owner's context.
// All per-painter contexts, textures and registrations are destroyed.
#if defined(HSTREAM_USE_HIP)
EGLDisplay HipDeviceDisplay(int device) {
  struct Entry {
    std::string pci;
    EGLDisplay display{EGL_NO_DISPLAY};
  };
  static std::mutex mutex;
  static std::array<Entry, 16> displays{};
  char pci[32]{};
  if (hipDeviceGetPCIBusId(pci, sizeof(pci), device) != hipSuccess)
    return EGL_NO_DISPLAY;
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& entry : displays)
    if (entry.display != EGL_NO_DISPLAY && entry.pci == pci)
      return entry.display;
  for (auto& entry : displays) {
    if (entry.display != EGL_NO_DISPLAY)
      continue;
    // Bind Mesa to exactly the HIP GPU, as the existing AMD VA/EGL path does.
    // Do not initialize unrelated NVIDIA/software EGL device backends.
    const std::string path = std::string("/dev/dri/by-path/pci-") + pci + "-render";
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
      return EGL_NO_DISPLAY;
    auto* gbm = gbm_create_device(fd);
    const auto display = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr) : EGL_NO_DISPLAY;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
      if (gbm)
        gbm_device_destroy(gbm);
      close(fd);
      return EGL_NO_DISPLAY;
    }
    // The device/display connection (including its GBM owner and DRM fd) is
    // shared for process lifetime; each painter owns only its GL resources.
    entry = {pci, display};
    return display;
  }
  return EGL_NO_DISPLAY;
}
#else
EGLDisplay DeviceDisplay(EGLDeviceEXT device) {
  static std::mutex mutex;
  static std::array<std::pair<EGLDeviceEXT, EGLDisplay>, 16> displays{};
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& entry : displays)
    if (entry.first == device && entry.second != EGL_NO_DISPLAY)
      return entry.second;
  const auto get_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!get_display)
    return EGL_NO_DISPLAY;
  for (auto& entry : displays) {
    if (entry.second != EGL_NO_DISPLAY)
      continue;
    const auto display = get_display(EGL_PLATFORM_DEVICE_EXT, device, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
      return EGL_NO_DISPLAY;
    entry = {device, display};
    return display;
  }
  return EGL_NO_DISPLAY;
}
#endif

struct ContextGuard {
  EGLDisplay display{eglGetCurrentDisplay()};
  EGLContext context{eglGetCurrentContext()};
  EGLSurface draw{eglGetCurrentSurface(EGL_DRAW)}, read{eglGetCurrentSurface(EGL_READ)};
  EGLenum api{eglQueryAPI()};
  Display* xdisplay{glXGetCurrentDisplay()};
  GLXContext xcontext{glXGetCurrentContext()};
  GLXDrawable xdraw{glXGetCurrentDrawable()}, xread{glXGetCurrentReadDrawable()};
  bool valid{false}, restored{false}, detached{false};
  ContextGuard() {
    if (xcontext) {
      detached = glXMakeContextCurrent(xdisplay, None, None, nullptr);
      if (!detached)
        return;
    }
    valid = eglBindAPI(EGL_OPENGL_API);
  }
  bool Restore() {
    if (restored)
      return true;
    restored = true;
    bool ok = true;
    const auto current = eglGetCurrentDisplay();
    if (valid && current != EGL_NO_DISPLAY)
      ok = eglMakeCurrent(current, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    ok = eglBindAPI(api) && ok;
    if (context != EGL_NO_CONTEXT)
      ok = eglMakeCurrent(display, draw, read, context) && ok;
    if (detached)
      ok = glXMakeContextCurrent(xdisplay, xdraw, xread, xcontext) && ok;
    return ok;
  }
  ~ContextGuard() {
    Restore();
  }
};

bool Valid(const analytics::ImageView& image) {
  return image.format == analytics::PixelFormat::kRgba8 && image.data && image.width && image.height &&
      image.width <= 16384 && image.height <= 16384 &&
      uint64_t(image.width) * image.height <= GpuGlSurface::kMaximumPixels && image.pitch >= size_t(image.width) * 4 &&
      image.pitch % 4 == 0 && reinterpret_cast<uintptr_t>(image.data) % 4 == 0 &&
      image.pitch <= std::numeric_limits<size_t>::max() / image.height;
}

struct CallGuard {
  std::atomic_flag& busy;
  bool acquired;
  explicit CallGuard(std::atomic_flag& flag) : busy(flag), acquired(!busy.test_and_set(std::memory_order_acquire)) {}
  ~CallGuard() {
    if (acquired)
      busy.clear(std::memory_order_release);
  }
};

} // namespace

struct GpuGlSurface::State {
  EGLDisplay display{EGL_NO_DISPLAY};
  EGLContext context{EGL_NO_CONTEXT};
  GLuint texture[2]{}, framebuffer[2]{};
  cudaGraphicsResource_t resource{nullptr};
  bool mapped{false}, retired{false};
  int device{-1};
  cudaStream_t stream{nullptr};
  unsigned width{0}, height{0};
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  GpuGlCounters counters;

  void Release(bool allow_binding = true) {
    if (context != EGL_NO_CONTEXT) {
      const bool current = allow_binding && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
      if (mapped && resource) {
        (void)cudaGraphicsUnmapResources(1, &resource, stream);
        mapped = false;
      }
      if (resource)
        (void)cudaGraphicsUnregisterResource(resource);
      resource = nullptr;
      // Never delete GL names in an unrelated caller context when binding
      // failed. Destroying our private context reclaims its remaining objects.
      if (current) {
        glDeleteFramebuffers(2, framebuffer);
        glDeleteTextures(2, texture);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      }
      eglDestroyContext(display, context);
      context = EGL_NO_CONTEXT;
    }
    std::fill(std::begin(texture), std::end(texture), 0);
    std::fill(std::begin(framebuffer), std::end(framebuffer), 0);
    display = EGL_NO_DISPLAY;
  }

  ~State() {
    if (context == EGL_NO_CONTEXT)
      return;
    ContextGuard guard;
    int previous = -1;
    (void)cudaGetDevice(&previous);
    (void)cudaSetDevice(device);
    Release(guard.valid);
    if (previous >= 0)
      (void)cudaSetDevice(previous);
  }

  GpuGlResult Initialize(const analytics::ImageView& image, int requested_device, cudaStream_t requested_stream) {
    device = requested_device;
#if defined(HSTREAM_USE_HIP)
    constexpr int count = 1;
#else
    const auto query = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    std::array<EGLDeviceEXT, 16> devices{};
    EGLint count = 0;
    if (!query || !query(devices.size(), devices.data(), &count) || count < 0 ||
        count > static_cast<EGLint>(devices.size()))
      return {GpuGlStatus::kUnavailable, cudaSuccess, static_cast<unsigned>(eglGetError())};
#endif
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE,
        0,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE};
    for (int index = 0; index < count; ++index) {
#if defined(HSTREAM_USE_HIP)
      display = HipDeviceDisplay(requested_device);
#else
      display = DeviceDisplay(devices[index]);
#endif
      EGLConfig config{};
      EGLint configs = 0;
      if (display == EGL_NO_DISPLAY || !eglChooseConfig(display, config_attributes, &config, 1, &configs) || !configs)
        continue;
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
      if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        Release();
        continue;
      }
#if !defined(HSTREAM_USE_HIP)
      unsigned gpu_count = 0;
      int gpu_devices[16]{};
      const auto result = cudaGLGetDevices(&gpu_count, gpu_devices, 16, cudaGLDeviceListAll);
      if (result != cudaSuccess || gpu_count > 16 ||
          std::find(gpu_devices, gpu_devices + gpu_count, requested_device) == gpu_devices + gpu_count) {
        Release();
        continue;
      }
#endif
      device = requested_device;
      stream = requested_stream;
      width = image.width;
      height = image.height;
      glGenTextures(2, texture);
      glGenFramebuffers(2, framebuffer);
      for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
          return {GpuGlStatus::kGlError, cudaSuccess, 0, static_cast<unsigned>(glGetError())};
      }
      const auto gl_error = glGetError();
      if (gl_error != GL_NO_ERROR)
        return {GpuGlStatus::kGlError, cudaSuccess, 0, gl_error};
      const auto registered =
          cudaGraphicsGLRegisterImage(&resource, texture[0], GL_TEXTURE_2D, cudaGraphicsRegisterFlagsNone);
      if (registered != cudaSuccess)
        return {GpuGlStatus::kUnavailable, registered};
      counters.texture_allocations = 2;
      counters.texture_bytes = uint64_t(width) * height * 8;
      return {};
    }
    return {GpuGlStatus::kUnavailable};
  }

  GpuGlResult Bind(const analytics::ImageView& image, cudaStream_t requested_stream) {
    int requested_device = -1;
    const auto error = cudaGetDevice(&requested_device);
    if (error != cudaSuccess)
      return {GpuGlStatus::kGpuError, error};
    if (context != EGL_NO_CONTEXT &&
        (device != requested_device || stream != requested_stream || width != image.width || height != image.height))
      return {GpuGlStatus::kInvalidArgument};
    if (context == EGL_NO_CONTEXT) {
      const auto initialized = Initialize(image, requested_device, requested_stream);
      if (initialized.status != GpuGlStatus::kOk) {
        retired = true;
        return initialized;
      }
    } else if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
      retired = true;
      return {GpuGlStatus::kGlError, cudaSuccess, static_cast<unsigned>(eglGetError())};
    }
    return {};
  }

  cudaError_t Map(cudaArray_t* array) {
    auto result = cudaGraphicsMapResources(1, &resource, stream);
    if (result != cudaSuccess)
      return result;
    mapped = true;
    return cudaGraphicsSubResourceGetMappedArray(array, resource, 0, 0);
  }
  cudaError_t Unmap() {
    const auto result = cudaGraphicsUnmapResources(1, &resource, stream);
    if (result == cudaSuccess)
      mapped = false;
    return result;
  }
};

GpuGlSurface::GpuGlSurface() : state_(std::make_unique<State>()) {}
GpuGlSurface::~GpuGlSurface() = default;
const GpuGlCounters& GpuGlSurface::counters() const {
  return state_->counters;
}

GpuGlResult GpuGlSurface::Prepare(const analytics::ImageView& image, cudaStream_t stream) {
  auto& state = *state_;
  CallGuard lock(state.busy);
  if (!lock.acquired || !Valid(image) || state.retired)
    return {GpuGlStatus::kInvalidArgument};
  ContextGuard guard;
  if (!guard.valid)
    return {GpuGlStatus::kGlError, cudaSuccess, static_cast<unsigned>(eglGetError())};
  const auto bound = state.Bind(image, stream);
  if (!guard.Restore()) {
    state.retired = true;
    return {GpuGlStatus::kGlError, cudaSuccess, static_cast<unsigned>(eglGetError())};
  }
  return bound;
}

GpuGlResult GpuGlSurface::Paint(
    const analytics::ImageView& image,
    cudaStream_t stream,
    const std::function<bool()>& draw) {
  auto& state = *state_;
  CallGuard lock(state.busy);
  if (!lock.acquired || !Valid(image) || !draw || state.retired)
    return {GpuGlStatus::kInvalidArgument};
  ContextGuard guard;
  if (!guard.valid)
    return {GpuGlStatus::kGlError, cudaSuccess, static_cast<unsigned>(eglGetError())};
  const auto bound = state.Bind(image, stream);
  if (bound.status != GpuGlStatus::kOk)
    return bound;
  cudaError_t gpu_error = cudaSuccess;
  auto failure = [&](cudaError_t error) {
    state.retired = true;
    if (state.mapped)
      state.Unmap();
    return GpuGlResult{GpuGlStatus::kGpuError, error};
  };
  cudaArray_t array{};
  if ((gpu_error = state.Map(&array)) != cudaSuccess)
    return failure(gpu_error);
  gpu_error = cudaMemcpy2DToArrayAsync(
      array, 0, 0, image.data, image.pitch, size_t(image.width) * 4, image.height, cudaMemcpyDeviceToDevice, stream);
  if (gpu_error != cudaSuccess)
    return failure(gpu_error);
  if ((gpu_error = state.Unmap()) != cudaSuccess)
    return failure(gpu_error);
  // Interop unmap orders prior stream work before subsequent GL accesses.
  // Flip GPU storage rows into native GL orientation without sampling video.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, state.framebuffer[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.framebuffer[1]);
  glBlitFramebuffer(0, 0, state.width, state.height, 0, state.height, state.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer[1]);
  glViewport(0, 0, state.width, state.height);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_LINE_SMOOTH);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  bool drawn = false;
  try {
    drawn = draw();
  } catch (...) {
  }
  if (!drawn) {
    state.retired = true;
    return {GpuGlStatus::kCallbackFailed};
  }
  // GL's native path blend updates alpha differently from opaque video. Keep
  // its exact RGB and restore opaque alpha for subsequent portable overlays.
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, state.framebuffer[1]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.framebuffer[0]);
  glBlitFramebuffer(0, 0, state.width, state.height, 0, state.height, state.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  const auto gl_error = glGetError();
  if (gl_error != GL_NO_ERROR) {
    state.retired = true;
    return {GpuGlStatus::kGlError, cudaSuccess, 0, gl_error};
  }
  // Interop map waits for the preceding GL writes. Only device memory moves.
  if ((gpu_error = state.Map(&array)) != cudaSuccess)
    return failure(gpu_error);
  gpu_error = cudaMemcpy2DFromArrayAsync(
      image.data, image.pitch, array, 0, 0, size_t(image.width) * 4, image.height, cudaMemcpyDeviceToDevice, stream);
  if (gpu_error != cudaSuccess)
    return failure(gpu_error);
  if ((gpu_error = state.Unmap()) != cudaSuccess)
    return failure(gpu_error);
  ++state.counters.frames;
  state.counters.copied_bytes += uint64_t(image.width) * image.height * 8;
  if (!guard.Restore()) {
    state.retired = true;
    return {GpuGlStatus::kGlError, cudaSuccess, static_cast<unsigned>(eglGetError())};
  }
  return {};
}

} // namespace hm::draw_display
