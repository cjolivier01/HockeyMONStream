#include "hstream/src/libs/draw_display/AnalyticsOverlayGl.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlayAtlas.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace hm::draw_display::analytics {
namespace {
using detail::Command;
using detail::Kind;
struct Vertex {
  float position[2], endpoints[4], color[4], parameters[3];
};
constexpr size_t kVertexBytes = kMaximumCommands * 4 * sizeof(Vertex);
constexpr size_t kIndexCount = kMaximumCommands * 6;
static_assert(kMaximumCommands * 4 <= 65536);
// GLSL 1.20 literals below must stay paired with the one shared atlas layout.
static_assert(
    detail::kGlyphWidth == 32 && detail::kGlyphHeight == 48 && detail::kAtlasColumns == 16 &&
    detail::kAtlasWidth == 512 && detail::kAtlasHeight == 288);
static_assert(static_cast<int>(Kind::kGlyph) == 4);

struct Bounds {
  float left, top, right, bottom;
};
Bounds ClippedBounds(const Command& c, float width, float height) {
  Bounds b{c.x0, c.y0, c.x1, c.y1};
  if (c.kind == Kind::kLine) {
    b = {
        std::min(c.x0, c.x1) - c.radius - 1,
        std::min(c.y0, c.y1) - c.radius - 1,
        std::max(c.x0, c.x1) + c.radius + 1,
        std::max(c.y0, c.y1) + c.radius + 1};
  } else if (c.kind == Kind::kDisc) {
    b = {c.x0 - c.radius - 1, c.y0 - c.radius - 1, c.x0 + c.radius + 1, c.y0 + c.radius + 1};
  }
  return {std::max(0.0F, b.left), std::max(0.0F, b.top), std::min(width, b.right), std::min(height, b.bottom)};
}
bool Visible(Bounds b) {
  return b.left < b.right && b.top < b.bottom;
}

constexpr char kVertexShader[] = R"GLSL(#version 120
attribute vec2 position;
attribute vec4 endpoints;
attribute vec4 color;
attribute vec3 parameters;
uniform vec2 extent;
varying vec2 pixel;
varying vec4 ends;
varying vec4 rgba;
varying vec3 params;
void main() {
  pixel = position;
  ends = endpoints;
  rgba = color;
  params = parameters;
  gl_Position = vec4(position.x * 2.0 / extent.x - 1.0, 1.0 - position.y * 2.0 / extent.y, 0.0, 1.0);
}
)GLSL";
constexpr char kFragmentShader[] = R"GLSL(#version 120
uniform sampler2D atlas;
varying vec2 pixel;
varying vec4 ends;
varying vec4 rgba;
varying vec3 params;
float sample_cell(vec2 origin, vec2 xy) {
  if (xy.x < 0.0 || xy.x >= 32.0 || xy.y < 0.0 || xy.y >= 48.0) return 0.0;
  return texture2D(atlas, (origin + xy + vec2(0.5)) / vec2(512.0, 288.0)).r;
}
void main() {
  float coverage = 0.0;
  if (params.x < 0.5) {
    vec2 d = ends.zw - ends.xy;
    float length2 = dot(d,d);
    float u = length2 > 0.0 ? clamp(dot(pixel-ends.xy,d)/length2, 0.0, 1.0) : 0.0;
    coverage = clamp(params.y + 0.5 - length(pixel-ends.xy-u*d), 0.0, 1.0);
  } else if (params.x < 1.5) {
    coverage = clamp(params.y + 0.5 - length(pixel-ends.xy), 0.0, 1.0);
  } else {
    if (pixel.x < ends.x || pixel.y < ends.y || pixel.x >= ends.z || pixel.y >= ends.w) discard;
    if (params.x < 2.5) {
      coverage = (pixel.x < ends.x+params.y || pixel.x >= ends.z-params.y ||
                  pixel.y < ends.y+params.y || pixel.y >= ends.w-params.y) ? 1.0 : 0.0;
    } else if (params.x < 3.5) {
      coverage = 1.0;
    } else {
      float glyph = floor(params.z + 0.5);
      vec2 origin = vec2(mod(glyph,16.0)*32.0, floor(glyph/16.0)*48.0);
      vec2 uv = (pixel-ends.xy)*vec2(32.0,48.0)/(ends.zw-ends.xy) - vec2(0.5);
      vec2 base = floor(uv), f = fract(uv);
      coverage = mix(mix(sample_cell(origin,base), sample_cell(origin,base+vec2(1,0)), f.x),
                     mix(sample_cell(origin,base+vec2(0,1)), sample_cell(origin,base+vec2(1,1)), f.x), f.y);
    }
  }
  if (coverage <= 0.0) discard;
  gl_FragColor = vec4(rgba.rgb, rgba.a * coverage);
}
)GLSL";

GLuint Compile(GLenum kind, const char* source) {
  const GLuint shader = glCreateShader(kind);
  if (!shader)
    return 0;
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok)
    return shader;
  glDeleteShader(shader);
  return 0;
}

// Generic vertex attributes are saved explicitly: the preview currently uses
// fixed-function drawing, but callers may have configured their own arrays.
struct SavedState {
  struct Attribute {
    GLint enabled, size, type, normalized, stride, buffer;
    void* pointer;
    GLfloat current[4];
  };
  std::array<Attribute, 4> attributes;
  GLint program, array_buffer, element_buffer, active_texture, texture, unpack_buffer;
  GLint unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
  SavedState() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
    for (GLuint i = 0; i < attributes.size(); ++i) {
      auto& a = attributes[i];
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
      glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
      if (i)
        glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB, a.current);
    }
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_POLYGON_BIT | GL_CURRENT_BIT);
  }
  ~SavedState() {
    for (GLuint i = 0; i < attributes.size(); ++i) {
      const auto& a = attributes[i];
      glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
      glVertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, a.pointer);
      if (a.enabled)
        glEnableVertexAttribArray(i);
      else
        glDisableVertexAttribArray(i);
      if (i)
        glVertexAttrib4fv(i, a.current);
    }
    glPopAttrib();
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active_texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
  }
};
} // namespace

struct GlCompositor::State {
  GLXContext context{nullptr};
  Display* display{nullptr};
  GLuint program{0}, atlas{0}, index{0};
  std::array<GLuint, 3> buffers{};
  GLint extent{-1};
  size_t slot{0};
  bool poisoned{false}, font_failed{false};
  std::vector<Vertex> vertices;
  ~State() {
    if (!context || glXGetCurrentContext() != context || glXGetCurrentDisplay() != display)
      return;
    glDeleteBuffers(buffers.size(), buffers.data());
    if (index)
      glDeleteBuffers(1, &index);
    if (atlas)
      glDeleteTextures(1, &atlas);
    if (program)
      glDeleteProgram(program);
  }
};

GlCompositor::GlCompositor(const char* font_path) : font_path_(font_path ? font_path : "") {}
GlCompositor::~GlCompositor() = default;
GlRenderResult GlCompositor::Render(float width, float height, const CommandList& commands) {
  GlRenderResult result;
  ++counters_.renders;
  if (commands.empty()) {
    ++counters_.empty_renders;
    return result;
  }
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    result.status = GlRenderStatus::kInvalidArgument;
    return result;
  }
  bool glyphs = false;
  double area = 0;
  for (size_t i = 0; i < commands.size(); ++i) {
    const auto& c = commands.data()[i];
    const Bounds b = ClippedBounds(c, width, height);
    if (!Visible(b))
      continue;
    ++result.commands;
    glyphs |= c.kind == Kind::kGlyph;
    area += (b.right - b.left) * (b.bottom - b.top);
  }
  if (!result.commands)
    return result;
  const GLXContext context = glXGetCurrentContext();
  Display* display = glXGetCurrentDisplay();
  if (!context || !display || (state_ && (state_->context != context || state_->display != display))) {
    result.status = GlRenderStatus::kInvalidArgument;
    return result;
  }
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (viewport[2] <= 0 || viewport[3] <= 0)
    return result;
  // Conservative fragment budget includes the source-to-viewport scale.
  if (area * viewport[2] * viewport[3] / (double(width) * height) > kMaximumGlFragmentCandidates) {
    ++counters_.capacity_suppressions;
    result.status = GlRenderStatus::kCapacity;
    return result;
  }
  try {
    if (!state_) {
      state_ = std::make_unique<State>();
      state_->context = context;
      state_->display = display;
    }
    State& state = *state_;
    if (state.poisoned) {
      result.status = GlRenderStatus::kGlError;
      return result;
    }
    if (glyphs && state.font_failed) {
      result.status = GlRenderStatus::kFontUnavailable;
      return result;
    }
    state.vertices.clear();
    state.vertices.reserve(kMaximumCommands * 4);
    for (size_t i = 0; i < commands.size(); ++i) {
      const auto& c = commands.data()[i];
      const Bounds b = ClippedBounds(c, width, height);
      if (!Visible(b))
        continue;
      for (const auto& p :
           {std::array<float, 2>{b.left, b.top}, {b.left, b.bottom}, {b.right, b.bottom}, {b.right, b.top}}) {
        state.vertices.push_back(
            {{p[0], p[1]},
             {c.x0, c.y0, c.x1, c.y1},
             {c.color.red, c.color.green, c.color.blue, c.color.alpha},
             {static_cast<float>(c.kind), c.radius, static_cast<float>(c.glyph)}});
      }
    }
    const auto error = [&]() {
      result.gl_error = glGetError();
      if (result.gl_error == GL_NO_ERROR)
        return false;
      state.poisoned = true;
      result.status = GlRenderStatus::kGlError;
      return true;
    };
    // The caller must not carry a preexisting GL error into this operation.
    if (error())
      return result;
    {
      SavedState saved;
      if (!state.program) {
        std::vector<uint16_t> indices(kIndexCount);
        const GLuint vertex = Compile(GL_VERTEX_SHADER, kVertexShader),
                     fragment = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
        if (vertex && fragment) {
          state.program = glCreateProgram();
          glAttachShader(state.program, vertex);
          glAttachShader(state.program, fragment);
          glBindAttribLocation(state.program, 0, "position");
          glBindAttribLocation(state.program, 1, "endpoints");
          glBindAttribLocation(state.program, 2, "color");
          glBindAttribLocation(state.program, 3, "parameters");
          glLinkProgram(state.program);
        }
        if (vertex)
          glDeleteShader(vertex);
        if (fragment)
          glDeleteShader(fragment);
        GLint linked = 0;
        if (state.program)
          glGetProgramiv(state.program, GL_LINK_STATUS, &linked);
        if (!linked) {
          state.poisoned = true;
          result.status = GlRenderStatus::kGlError;
          return result;
        }
        state.extent = glGetUniformLocation(state.program, "extent");
        glUseProgram(state.program);
        glUniform1i(glGetUniformLocation(state.program, "atlas"), 0);
        for (size_t i = 0; i < kMaximumCommands; ++i) {
          const uint16_t base = i * 4;
          const uint16_t quad[6] = {
              base,
              static_cast<uint16_t>(base + 1),
              static_cast<uint16_t>(base + 2),
              base,
              static_cast<uint16_t>(base + 2),
              static_cast<uint16_t>(base + 3)};
          std::memcpy(indices.data() + i * 6, quad, sizeof(quad));
        }
        glGenBuffers(1, &state.index);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.index);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
        glGenBuffers(state.buffers.size(), state.buffers.data());
        for (const GLuint buffer : state.buffers) {
          glBindBuffer(GL_ARRAY_BUFFER, buffer);
          glBufferData(GL_ARRAY_BUFFER, kVertexBytes, nullptr, GL_STREAM_DRAW);
        }
        counters_.buffer_allocations += state.buffers.size() + 1;
        counters_.buffer_bytes += state.buffers.size() * kVertexBytes + indices.size() * sizeof(uint16_t);
        if (error())
          return result;
      }
      if (glyphs && !state.atlas) {
        std::vector<uint8_t> atlas(detail::kAtlasBytes);
        if (!detail::BuildAtlas(atlas.data(), font_path_)) {
          state.font_failed = true;
          result.status = GlRenderStatus::kFontUnavailable;
          return result;
        }
        glGenTextures(1, &state.atlas);
        glBindTexture(GL_TEXTURE_2D, state.atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_LUMINANCE8,
            detail::kAtlasWidth,
            detail::kAtlasHeight,
            0,
            GL_LUMINANCE,
            GL_UNSIGNED_BYTE,
            atlas.data());
        ++counters_.atlas_uploads;
        counters_.atlas_bytes += detail::kAtlasBytes;
        if (error())
          return result;
      }
      glUseProgram(state.program);
      glUniform2f(state.extent, width, height);
      glBindTexture(GL_TEXTURE_2D, state.atlas);
      glBindBuffer(GL_ARRAY_BUFFER, state.buffers[state.slot]);
      state.slot = (state.slot + 1) % state.buffers.size();
      result.upload_bytes = state.vertices.size() * sizeof(Vertex);
      glBufferSubData(GL_ARRAY_BUFFER, 0, result.upload_bytes, state.vertices.data());
      ++counters_.vertex_uploads;
      counters_.vertex_bytes += result.upload_bytes;
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.index);
      const GLint sizes[4] = {2, 4, 4, 3};
      const size_t offsets[4] = {
          offsetof(Vertex, position),
          offsetof(Vertex, endpoints),
          offsetof(Vertex, color),
          offsetof(Vertex, parameters)};
      for (GLuint i = 0; i < 4; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(
            i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsets[i]));
      }
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_STENCIL_TEST);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_CULL_FACE);
      glDisable(GL_ALPHA_TEST);
      glDisable(GL_DITHER);
      glDisable(GL_COLOR_LOGIC_OP);
      glDisable(GL_MULTISAMPLE);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glEnable(GL_BLEND);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      if (error())
        return result;
      glDrawElements(GL_TRIANGLES, result.commands * 6, GL_UNSIGNED_SHORT, nullptr);
      ++counters_.draws;
      result.drawn = true;
    }
    error();
  } catch (const std::bad_alloc&) {
    ++counters_.capacity_suppressions;
    result.status = GlRenderStatus::kCapacity;
  }
  return result;
}
} // namespace hm::draw_display::analytics
