#include "hstream/src/apps/apps-common/PreviewOverlayGlPaths.h"
#include "hstream/src/libs/draw_display/GpuGlSurface.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace p = hm::gpu_preview;
namespace d = hm::draw_display;
namespace {
void Check(bool okay, const char* message) {
  if (!okay)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t error) {
  if (error != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(error));
}
struct Context {
  Display* display{};
  GLXContext context{};
  Window window{};
  Colormap colormap{};
  Context() {
    display = XOpenDisplay(nullptr);
    Check(display, "native GLX display unavailable");
    int attributes[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
    auto* visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    Check(visual, "native GLX visual unavailable");
    colormap = XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    XSetWindowAttributes options{};
    options.colormap = colormap;
    window = XCreateWindow(
        display,
        RootWindow(display, visual->screen),
        0,
        0,
        192,
        128,
        0,
        visual->depth,
        InputOutput,
        visual->visual,
        CWColormap,
        &options);
    context = glXCreateContext(display, visual, nullptr, True);
    XFree(visual);
    Check(context && glXMakeCurrent(display, window, context), "native GLX context unavailable");
  }
  ~Context() {
    glXMakeCurrent(display, None, nullptr);
    glXDestroyContext(display, context);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
  }
};
struct Target {
  static constexpr unsigned width = 192, height = 128;
  GLuint texture{}, framebuffer{};
  Target() {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "native target incomplete");
  }
  ~Target() {
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
  }
  void Clear() {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(32.0F / 255, 64.0F / 255, 96.0F / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  std::vector<uint8_t> Read() {
    std::vector<uint8_t> raw(width * height * 4), result(raw.size());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    for (unsigned y = 0; y < height; ++y)
      std::memcpy(result.data() + y * width * 4, raw.data() + (height - y - 1) * width * 4, width * 4);
    Check(glGetError() == GL_NO_ERROR, "native GL error");
    return result;
  }
};

void Compare(Context& context, unsigned left, unsigned top, unsigned width, unsigned height) {
  Target target;
  const size_t pitch = (Target::width + 19) * 4;
  std::vector<uint8_t> initial(pitch * Target::height, 0xa5), actual(initial.size());
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const uint32_t color = 32U | (64U << 8) | (96U << 16) | (255U << 24);
      std::memcpy(initial.data() + (y + top) * pitch + (x + left) * 4, &color, 4);
    }
  cudaStream_t stream{};
  void* allocation{};
  Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  Gpu(cudaMalloc(&allocation, initial.size()));
  std::mt19937 random(12345);
  unsigned maximum = 0;
  {
    d::GpuGlSurface renderer;
    for (unsigned trial = 0; trial < 30; ++trial) {
      p::PreviewOverlayScene scene;
      scene.coordinate_width = trial % 2 ? 20457 : 1280;
      scene.coordinate_height = trial % 2 ? 5256 : 720;
      std::uniform_real_distribution<float> x(-scene.coordinate_width * .1F, scene.coordinate_width * 1.1F);
      std::uniform_real_distribution<float> y(-scene.coordinate_height * .1F, scene.coordinate_height * 1.1F);
      for (unsigned i = 0; i < 60; ++i) {
        p::OverlayPath path;
        path.width = (i % 8) + 1.0F;
        path.color = {float(i % 3) / 2, float(i % 5) / 4, float(i % 7) / 6, .3F + .1F * (i % 6)};
        for (unsigned point = 0; point < 2 + i % 4; ++point)
          path.points.push_back({x(random), y(random)});
        path.closed = i % 3 == 0;
        scene.paths.push_back(std::move(path));
      }
      scene.paths.push_back(
          {{{0, 0}, {scene.coordinate_width, scene.coordinate_height}}, {1, 0, 0, .5F}, 3, false, false});
      scene.paths.push_back(
          {{{scene.coordinate_width * .25F, scene.coordinate_height * .25F},
            {scene.coordinate_width * .75F, scene.coordinate_height * .25F},
            {scene.coordinate_width * .5F, scene.coordinate_height * .75F}},
           {0, 1, 0, .3F},
           1,
           true,
           true});
      target.Clear();
      glViewport(left, Target::height - top - height, width, height);
      p::DrawPreviewOverlayGlPaths(scene);
      const auto expected = target.Read();
      Gpu(cudaMemcpyAsync(allocation, initial.data(), initial.size(), cudaMemcpyHostToDevice, stream));
      const auto result = renderer.Paint(
          {static_cast<uint8_t*>(allocation) + top * pitch + left * 4, pitch, width, height}, stream, [&] {
            p::DrawPreviewOverlayGlPaths(scene);
            return true;
          });
      Check(result.status == d::GpuGlStatus::kOk, "headless GPU interop unavailable");
      Check(glXGetCurrentContext() == context.context, "native Qt GLX context was not restored");
      Gpu(cudaMemcpyAsync(actual.data(), allocation, actual.size(), cudaMemcpyDeviceToHost, stream));
      Gpu(cudaStreamSynchronize(stream));
      for (unsigned row = 0; row < Target::height; ++row)
        for (unsigned byte = 0; byte < pitch; ++byte) {
          const auto index = row * pitch + byte;
          if (row < top || row >= top + height || byte < left * 4 || byte >= (left + width) * 4) {
            Check(actual[index] == initial[index], "GL painter changed letterbox/pitch guard");
          } else if (byte % 4 != 3) {
            const unsigned error = std::abs(int(actual[index]) - int(expected[row * Target::width * 4 + byte]));
            maximum = std::max(maximum, error);
            if (error != 0)
              throw std::runtime_error(
                  "native/headless GL pixel differs trial=" + std::to_string(trial) + " x=" + std::to_string(byte / 4) +
                  " y=" + std::to_string(row) + " error=" + std::to_string(error));
          } else
            Check(actual[index] == 255, "GPU preview alpha is not opaque");
        }
    }
  }
  Check(glXGetCurrentContext() == context.context, "teardown lost native GLX context");
  Gpu(cudaFree(allocation));
  Gpu(cudaStreamDestroy(stream));
  std::cout << "shared_gl_paths native_rgb_max_error=" << maximum << " scenes=30 paths=1860 viewport=" << left << ','
            << top << ',' << width << ',' << height << " context_restored=1 letterbox_guards=1\n";
}
} // namespace
int main() {
  try {
    Gpu(cudaSetDevice(0));
    Context context;
    std::cout << "native_gl_renderer=" << glGetString(GL_RENDERER) << '\n';
    Compare(context, 0, 0, Target::width, Target::height);
    Compare(context, 16, 12, 160, 104);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
