#include "hstream/src/libs/draw_display/AnalyticsOverlayGl.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

std::atomic<uint64_t> allocations{0};
void* operator new(size_t size) {
  ++allocations;
  if (void* p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, size_t) noexcept {
  std::free(p);
}
namespace a = hm::draw_display::analytics;
namespace {
void Check(bool okay, const char* message) {
  if (!okay)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t error) {
  if (error != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(error));
}
void GlCheck() {
  const auto error = glGetError();
  if (error)
    throw std::runtime_error("GL error " + std::to_string(error));
}
struct Context {
  Display* display{};
  GLXContext context{};
  Window window{};
  Colormap colormap{};
  Context() {
    display = XOpenDisplay(nullptr);
    Check(display != nullptr, "XOpenDisplay failed");
    int attributes[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
    XVisualInfo* visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    Check(visual != nullptr, "GLX visual unavailable");
    colormap = XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    XSetWindowAttributes options{};
    options.colormap = colormap;
    window = XCreateWindow(
        display,
        RootWindow(display, visual->screen),
        0,
        0,
        128,
        96,
        0,
        visual->depth,
        InputOutput,
        visual->visual,
        CWColormap,
        &options);
    context = glXCreateContext(display, visual, nullptr, True);
    XFree(visual);
    Check(context && glXMakeCurrent(display, window, context), "GLX context failed");
  }
  ~Context() {
    if (context) {
      glXMakeCurrent(display, None, nullptr);
      glXDestroyContext(display, context);
    }
    if (window)
      XDestroyWindow(display, window);
    if (colormap)
      XFreeColormap(display, colormap);
    if (display)
      XCloseDisplay(display);
  }
  void Current() {
    Check(glXMakeCurrent(display, window, context), "restore GL context failed");
  }
};
struct Target {
  unsigned width, height;
  GLuint texture{}, framebuffer{};
  bool packed;
  Target(unsigned w, unsigned h, bool packed = false) : width(w), height(h), packed(packed) {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, packed ? GL_RGB10_A2 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "test target incomplete");
    glViewport(0, 0, w, h);
    GlCheck();
  }
  ~Target() {
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
  }
  void Clear() {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(.1F, .2F, .3F, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  void Upload(const std::vector<uint8_t>& top_down, size_t pitch) {
    std::vector<uint8_t> flipped(top_down.size());
    for (unsigned y = 0; y < height; ++y)
      std::memcpy(flipped.data() + y * pitch, top_down.data() + (height - y - 1) * pitch, pitch);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width,
        height,
        GL_RGBA,
        packed ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE,
        flipped.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GlCheck();
  }
  std::vector<uint8_t> Read() {
    const size_t pitch = (width + 23) * 4;
    std::vector<uint8_t> raw(pitch * height + 512, 0xa5), result(width * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, width + 23);
    glReadPixels(
        0, 0, width, height, GL_RGBA, packed ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE, raw.data() + 256);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    GlCheck();
    for (unsigned y = 0; y < height; ++y) {
      for (size_t i = width * 4; i < pitch; ++i)
        Check(raw[256 + y * pitch + i] == 0xa5, "GL pitched read guard changed");
      std::memcpy(result.data() + y * width * 4, raw.data() + 256 + (height - y - 1) * pitch, width * 4);
    }
    for (unsigned i = 0; i < 256; ++i)
      Check(raw[i] == 0xa5 && raw[raw.size() - 1 - i] == 0xa5, "GL allocation guard changed");
    return result;
  }
};
void Empty() {
  const auto before = allocations.load();
  {
    a::GlCompositor renderer;
    a::CommandList list;
    for (int i = 0; i < 20; ++i)
      Check(renderer.Render(NAN, -1, list).status == a::GlRenderStatus::kOk, "empty GL validation touched context");
    const auto& c = renderer.counters();
    Check(
        c.draws == 0 && c.buffer_allocations == 0 && c.atlas_uploads == 0 && c.vertex_uploads == 0,
        "empty GL resources");
  }
  Check(allocations.load() == before, "empty GL CPU allocation");
  std::cout << "gl_empty no_context=1 cpu_allocations=0 resources=0\n";
}
std::vector<uint8_t> CudaReference(
    const a::CommandList& commands,
    unsigned width,
    unsigned height,
    bool packed,
    const std::vector<uint8_t>& original,
    size_t pitch) {
  cudaStream_t stream{};
  void* device{};
  Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  Cuda(cudaMalloc(&device, original.size()));
  std::vector<uint8_t> result(original.size());
  {
    a::Compositor compositor;
    Cuda(cudaMemcpyAsync(device, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
    const auto rendered = compositor.Render(
        {device, pitch, width, height, packed ? a::PixelFormat::kRgb10A2 : a::PixelFormat::kRgba8}, commands, stream);
    Check(rendered.launched, "CUDA reference suppression");
    Cuda(cudaMemcpyAsync(result.data(), device, result.size(), cudaMemcpyDeviceToHost, stream));
    Cuda(cudaStreamSynchronize(stream));
  }
  Cuda(cudaFree(device));
  Cuda(cudaStreamDestroy(stream));
  return result;
}
void Pixels(bool packed) {
  constexpr unsigned width = 137, height = 103;
  Target target(width, height, packed);
  const size_t pitch = (width + 19) * 4;
  std::vector<uint8_t> original(pitch * height, 0xa5);
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const uint32_t pixel = packed ? ((x * 13 + 17) % 1024) | (((y * 7 + 11) % 1024) << 10) | (317U << 20) | (3U << 30)
                                    : ((x * 13 + 17) % 256) | (((y * 7 + 11) % 256) << 8) | (83U << 16) | (255U << 24);
      std::memcpy(original.data() + y * pitch + x * 4, &pixel, 4);
    }
  a::CommandList commands;
  commands.AddFill(-20, -20, 20.2F, 15.3F, {.4F, .7F, .3F, .35F});
  commands.AddLine(-100, -100, 100, 100, 3.5F, {1, 0, 0, .65F});
  commands.AddLine(10, 40, 66, 0, 1.2F, {0, 1, 0, .45F});
  commands.AddDisc(31.3F, 28.7F, 14.5F, {0, 0, 1, .6F});
  commands.AddRectangle(20, 15, 80, 45, 3, {.8F, .2F, .7F, .5F});
  commands.AddLine(10.5F, 8.5F, 10.5F, 8.5F, 5, {1, 1, 0, 1});
  commands.AddText(-8, 51, 29, "87 skating", {.9F, .8F, .1F, .7F});
  commands.AddText(55, 3, 19, "27", {.2F, .7F, .9F, 1});
  commands.AddDisc(136, 102, 7, {1, 0, 1, .9F});
  const auto expected = CudaReference(commands, width, height, packed, original, pitch);
  a::GlCompositor renderer;
  target.Upload(original, pitch);
  const auto result = renderer.Render(width, height, commands);
  Check(result.drawn && result.status == a::GlRenderStatus::kOk, "GL pixel render failed");
  const auto retained = commands;
  commands.Clear(); // GL submission owns its upload; caller commands may change immediately.
  const auto actual = target.Read();
  int maximum = 0;
  unsigned changed = 0;
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      uint32_t wanted, pixel, before;
      std::memcpy(&wanted, expected.data() + y * pitch + x * 4, 4);
      std::memcpy(&pixel, actual.data() + (y * width + x) * 4, 4);
      std::memcpy(&before, original.data() + y * pitch + x * 4, 4);
      changed += pixel != before;
      for (int channel = 0; channel < 4; ++channel) {
        const int shift = packed ? channel * 10 : channel * 8;
        const unsigned mask = packed ? (channel == 3 ? 3 : 1023) : 255;
        const int error = std::abs(int((wanted >> shift) & mask) - int((pixel >> shift) & mask));
        maximum = std::max(maximum, error);
        if (error > 3)
          throw std::runtime_error(
              "CUDA/GL mismatch at " + std::to_string(x) + "," + std::to_string(y) + " error=" + std::to_string(error));
      }
      for (size_t p = width * 4; p < pitch; ++p)
        Check(expected[y * pitch + p] == 0xa5, "CUDA reference pitch guard changed");
    }
  Check(changed > 300, "GL pixel test drew too little");
  target.Upload(original, pitch);
  Check(renderer.Render(width, height, retained).drawn, "GL second render failed");
  Check(target.Read() == actual, "GL repeat is nondeterministic");
  Check(
      renderer.counters().atlas_uploads == 1 && renderer.counters().buffer_allocations == 4, "GL resources recreated");
  std::cout << "gl_pixels format=" << (packed ? "rgb10a2" : "rgba8") << " native_cuda_channel_max_error=" << maximum
            << " changed=" << changed << " cached_atlas=1 pitched_read_guards=1 deterministic=1\n";
}
void People(a::CommandList* commands, unsigned width, unsigned height) {
  constexpr float joints[17][2] = {
      {.5F, .07F},
      {.45F, .06F},
      {.55F, .06F},
      {.4F, .08F},
      {.6F, .08F},
      {.3F, .25F},
      {.7F, .25F},
      {.15F, .4F},
      {.85F, .4F},
      {.05F, .55F},
      {.95F, .55F},
      {.35F, .55F},
      {.65F, .55F},
      {.3F, .75F},
      {.7F, .75F},
      {.25F, .97F},
      {.75F, .97F}};
  constexpr int edges[][2] = {
      {0, 1},
      {0, 2},
      {1, 3},
      {2, 4},
      {5, 6},
      {5, 7},
      {7, 9},
      {6, 8},
      {8, 10},
      {5, 11},
      {6, 12},
      {11, 12},
      {11, 13},
      {13, 15},
      {12, 14},
      {14, 16},
      {0, 5},
      {0, 6},
      {3, 5}};
  const float scale = width / 1920.0F;
  for (int person = 0; person < 32; ++person) {
    const float left = (person % 8) * width / 8.0F + 30 * scale;
    const float top = (person / 8) * height / 4.0F + 10 * scale;
    const float w = 110 * scale, h = 190 * scale;
    const a::Color color{
        (person % 3) == 0 ? .9F : .2F, (person % 3) == 1 ? .8F : .2F, (person % 3) == 2 ? .9F : .2F, 1};
    commands->AddRectangle(left, top, left + w, top + h, 2 * scale, color);
    for (const auto& edge : edges)
      commands->AddLine(
          left + joints[edge[0]][0] * w,
          top + joints[edge[0]][1] * h,
          left + joints[edge[1]][0] * w,
          top + joints[edge[1]][1] * h,
          2.5F * scale,
          color);
    for (const auto& joint : joints)
      commands->AddDisc(left + joint[0] * w, top + joint[1] * h, 3 * scale, color);
    commands->AddText(left, top + h, 20 * scale, "87 skating", color);
  }
  Check(commands->rejected() == 0, "32-player fixture overflowed commands");
}
float Percentile(std::vector<float> values, float q) {
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(q * (values.size() - 1))];
}
void StateAndCapacity(Context& owner) {
  Target target(128, 96);
  target.Clear();
  const auto before = target.Read();
  a::CommandList excessive;
  for (unsigned i = 0; i < a::kMaximumCommands; ++i)
    excessive.AddFill(0, 0, 128, 96, {1, 0, 0, 1});
  a::GlCompositor bounded;
  Check(bounded.Render(128, 96, excessive).status == a::GlRenderStatus::kCapacity, "GL fragment bound ignored");
  Check(
      target.Read() == before && bounded.counters().buffer_allocations == 0, "GL capacity changed image or allocated");
  a::CommandList commands;
  commands.AddDisc(33, 34, 4, {1, 0, 0, .5F});
  a::GlCompositor renderer;
  GLuint buffer, element, texture, unpack;
  glGenBuffers(1, &buffer);
  glGenBuffers(1, &element);
  glGenBuffers(1, &unpack);
  glGenTextures(1, &texture);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glBufferData(GL_ARRAY_BUFFER, 1024, nullptr, GL_STATIC_DRAW);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_TRUE, 24, reinterpret_cast<void*>(16));
  glEnableVertexAttribArray(2);
  glVertexAttrib4f(1, .2F, .3F, .4F, .5F);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 13);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glActiveTexture(GL_TEXTURE3);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glEnable(GL_CULL_FACE);
  glEnable(GL_ALPHA_TEST);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
  glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ZERO, GL_SRC_ALPHA);
  glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
  glColor4f(.7F, .6F, .5F, .4F);
  GlCheck();
  commands.AddText(8, 40, 24, "27", {1, 1, 1, 1}); // Exercises atlas upload with unusual unpack state.
  Check(renderer.Render(128, 96, commands).drawn, "stateful GL render failed");
  GlCheck();
  GLint value;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
  Check(value == GL_TEXTURE3, "active texture not restored");
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
  Check(GLuint(value) == texture, "texture binding not restored");
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &value);
  Check(GLuint(value) == buffer, "vertex buffer not restored");
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &value);
  Check(GLuint(value) == element, "element buffer not restored");
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &value);
  Check(GLuint(value) == unpack, "unpack buffer not restored");
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
  Check(value == 13, "unpack row length not restored");
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
  Check(value == 8, "unpack alignment not restored");
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &value);
  Check(value == 2, "unpack rows not restored");
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &value);
  Check(value == 3, "unpack pixels not restored");
  glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_SIZE, &value);
  Check(value == 3, "attribute size not restored");
  glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &value);
  Check(value == 24, "attribute stride not restored");
  glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &value);
  Check(value == GL_TRUE, "attribute enable not restored");
  void* pointer{};
  glGetVertexAttribPointerv(2, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
  Check(pointer == reinterpret_cast<void*>(16), "attribute pointer not restored");
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &value);
  Check(value == GL_FUNC_SUBTRACT, "blend equation not restored");
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &value);
  Check(value == GL_ZERO, "blend factors not restored");
  GLboolean mask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  Check(!mask[0] && mask[1] && !mask[2] && mask[3], "color mask not restored");
  GLfloat color[4];
  glGetFloatv(GL_CURRENT_COLOR, color);
  Check(std::abs(color[0] - .7F) < 1e-6F && std::abs(color[3] - .4F) < 1e-6F, "current color not restored");
  Check(
      glIsEnabled(GL_DEPTH_TEST) && glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_ALPHA_TEST) &&
          glIsEnabled(GL_CULL_FACE),
      "GL enables not restored");
  {
    Context other;
    Check(
        renderer.Render(128, 96, commands).status == a::GlRenderStatus::kInvalidArgument, "cross-context use accepted");
  }
  owner.Current();
  glDisableVertexAttribArray(2);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glDeleteBuffers(1, &buffer);
  glDeleteBuffers(1, &element);
  glDeleteBuffers(1, &unpack);
  glDeleteTextures(1, &texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glColor4f(1, 1, 1, 1);
  glBlendEquation(GL_FUNC_ADD);
  glActiveTexture(GL_TEXTURE0);
  GlCheck();
  std::cout << "gl_state restores_arrays_textures_blend_unpack=1 cross_context_rejected=1 capacity_untouched=1\n";
}
void ScaledViewport() {
  Target target(128, 96);
  target.Clear();
  glViewport(16, 12, 96, 72);
  a::CommandList commands;
  commands.AddFill(0, 0, 64, 48, {1, 0, 0, 1});
  commands.AddDisc(32, 24, 5, {0, 1, 0, 1});
  a::GlCompositor renderer;
  Check(renderer.Render(64, 48, commands).drawn, "scaled draw failed");
  const auto actual = target.Read();
  const auto at = [&](unsigned x, unsigned y, unsigned c) { return actual[(y * 128 + x) * 4 + c]; };
  Check(at(16, 12, 0) == 255 && at(111, 83, 0) == 255 && at(64, 48, 1) == 255, "scaled viewport coordinate mismatch");
  Check(
      at(15, 12, 0) != 255 && at(112, 83, 0) != 255 && at(16, 11, 0) != 255 && at(111, 84, 0) != 255,
      "letterbox was overwritten");
  std::cout << "gl_viewport output_coordinates_scaled=1 letterbox_preserved=1\n";
}
void FailureSuppression() {
  Target target(128, 96);
  target.Clear();
  const auto before = target.Read();
  a::CommandList commands;
  commands.AddText(1, 1, 24, "27", {1, 1, 1, 1});
  a::GlCompositor missing_font("/not-present/hstream-test-font.ttf");
  Check(
      missing_font.Render(128, 96, commands).status == a::GlRenderStatus::kFontUnavailable,
      "missing GL font did not suppress");
  const auto allocated = missing_font.counters().buffer_allocations;
  Check(
      missing_font.Render(128, 96, commands).status == a::GlRenderStatus::kFontUnavailable &&
          missing_font.counters().buffer_allocations == allocated,
      "missing GL font did not cache failure");
  Check(target.Read() == before, "missing GL font changed video");
  a::GlCompositor failed;
  glBindTexture(GL_TRIANGLES, 0); // Deliberate incoming GL error, test only.
  Check(failed.Render(128, 96, commands).status == a::GlRenderStatus::kGlError, "GL error was ignored");
  Check(
      failed.Render(128, 96, commands).status == a::GlRenderStatus::kGlError && failed.counters().draws == 0,
      "GL error did not retire renderer");
  Check(target.Read() == before, "GL error changed video");
  a::GlCompositor outside;
  a::CommandList clipped;
  clipped.AddDisc(-100, -100, 1, {1, 0, 0, 1});
  Check(
      outside.Render(128, 96, clipped).status == a::GlRenderStatus::kOk && outside.counters().buffer_allocations == 0,
      "fully clipped GL work allocated resources");
  std::cout << "gl_failure missing_font_cached=1 gl_error_retires=1 video_untouched=1 clipped_resources=0\n";
}
void Benchmark(unsigned width, unsigned height) {
  Target target(width, height);
  target.Clear();
  a::GlCompositor renderer;
  a::CommandList commands;
  People(&commands, width, height);
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  for (int i = 0; i < 8 || std::chrono::steady_clock::now() < until; ++i) {
    Check(renderer.Render(width, height, commands).drawn, "GL warmup failed");
    glFinish();
  }
  const auto before = renderer.counters();
  std::vector<float> gpu, cpu, build, wall;
  gpu.reserve(200);
  cpu.reserve(200);
  build.reserve(200);
  wall.reserve(200);
  GLuint query;
  glGenQueries(1, &query);
  a::GlRenderResult result;
  for (int i = 0; i < 200; ++i) {
    const auto allocated = allocations.load();
    const auto start = std::chrono::steady_clock::now();
    commands.Clear();
    People(&commands, width, height);
    const auto built = std::chrono::steady_clock::now();
    glBeginQuery(GL_TIME_ELAPSED, query);
    result = renderer.Render(width, height, commands);
    glEndQuery(GL_TIME_ELAPSED);
    const auto submitted = std::chrono::steady_clock::now();
    Check(allocations.load() == allocated, "GL warmed CPU allocation");
    Check(result.drawn && result.status == a::GlRenderStatus::kOk, "GL benchmark suppressed");
    GLuint64 nanoseconds = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
    const auto completed = std::chrono::steady_clock::now();
    gpu.push_back(nanoseconds / 1000.0F);
    cpu.push_back(std::chrono::duration<float, std::micro>(submitted - built).count());
    build.push_back(std::chrono::duration<float, std::micro>(built - start).count());
    wall.push_back(std::chrono::duration<float, std::micro>(completed - start).count());
  }
  const auto& after = renderer.counters();
  Check(
      after.buffer_allocations == before.buffer_allocations && after.atlas_uploads == before.atlas_uploads,
      "GL warmed resources allocated");
  Check(
      after.draws - before.draws == 200 && after.vertex_uploads - before.vertex_uploads == 200,
      "GL expected one upload/draw per frame");
  glDeleteQueries(1, &query);
  GlCheck();
  std::cout << "gl_benchmark width=" << width << " height=" << height << " players=32 commands=" << commands.size()
            << " upload_bytes=" << result.upload_bytes << " gpu_upload_draw_us_p50=" << Percentile(gpu, .5F)
            << " gpu_upload_draw_us_p95=" << Percentile(gpu, .95F) << " cpu_build_us_p50=" << Percentile(build, .5F)
            << " cpu_submit_us_p50=" << Percentile(cpu, .5F) << " wall_us_p50=" << Percentile(wall, .5F)
            << " wall_us_p95=" << Percentile(wall, .95F) << " buffer_bytes=" << after.buffer_bytes
            << " atlas_bytes=" << after.atlas_bytes
            << " warmed_allocations=0 warmed_cpu_allocations=0 draws_per_frame=1\n";
}
} // namespace
int main(int argc, char** argv) {
  try {
    Empty();
    Context context;
    std::cout << "gl_renderer=" << glGetString(GL_RENDERER) << " version=" << glGetString(GL_VERSION) << '\n';
    Cuda(cudaSetDevice(0));
    Pixels(false);
    Pixels(true);
    StateAndCapacity(context);
    ScaledViewport();
    FailureSuppression();
    if (argc == 2 && std::string(argv[1]) == "--benchmark") {
      Benchmark(1920, 1080);
      Benchmark(3840, 2160);
    }
    std::cout << "GL analytics checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
