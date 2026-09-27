#include "hstream/src/apps/apps-common/ArchiveWatermark.h"

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlayAtlas.h"
#include "hstream/src/libs/draw_display/Watermark.h"

#include <cuda_runtime.h>
#include <gst/video/video.h>
#include <nvbufsurface.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cuda_egl_interop.h>
#endif

namespace hm::archive_watermark {
namespace {

using hm::draw_display::analytics::ImageView;
using hm::draw_display::analytics::PixelFormat;

// RenderSystemI420 reports through a borrowed string, so keep one with static
// storage rather than returning a pointer into a temporary.
const std::string& FontUnavailableReason() {
  static const std::string reason =
      std::string(hm::draw_display::analytics::ToString(hm::draw_display::analytics::RenderStatus::kFontUnavailable)) +
      hm::draw_display::analytics::Remedy(hm::draw_display::analytics::RenderStatus::kFontUnavailable);
  return reason;
}

struct State {
  State(GstElement* owner, int device, std::string font_path)
      : owner(owner), device(device), font_path(std::move(font_path)) {}
  ~State() {
    if (stream) {
      cudaSetDevice(device);
      cudaStreamSynchronize(stream);
      compositor.reset();
      cudaStreamDestroy(stream);
    }
  }
  GstElement* owner;
  int device;
  std::string font_path;
  cudaStream_t stream{nullptr};
  std::unique_ptr<hm::draw_display::analytics::Compositor> compositor;
  hm::draw_display::analytics::CommandList commands;
  std::vector<uint8_t> cpu_atlas;
  // Latched the way each compositor latches its own font failure. Without it
  // every later buffer re-opens and re-reads the TTF before failing the same
  // way.
  bool cpu_font_failed{false};
  // One bus error is the signal; the rest would be a log flood at frame rate.
  bool error_reported{false};
};

float GlyphCoverage(const hm::draw_display::analytics::detail::Command& glyph, float x, float y, const uint8_t* atlas) {
  using namespace hm::draw_display::analytics::detail;
  if (x < glyph.x0 || x >= glyph.x1 || y < glyph.y0 || y >= glyph.y1)
    return 0;
  const float u = (x - glyph.x0) * kGlyphWidth / (glyph.x1 - glyph.x0) - 0.5F;
  const float v = (y - glyph.y0) * kGlyphHeight / (glyph.y1 - glyph.y0) - 0.5F;
  const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
  const float fx = u - ix, fy = v - iy;
  const int ox = (glyph.glyph % kAtlasColumns) * kGlyphWidth;
  const int oy = (glyph.glyph / kAtlasColumns) * kGlyphHeight;
  float coverage = 0;
  for (int row = 0; row < 2; ++row)
    for (int column = 0; column < 2; ++column) {
      const int sx = ix + column, sy = iy + row;
      if (sx >= 0 && sx < kGlyphWidth && sy >= 0 && sy < kGlyphHeight)
        coverage += atlas[(oy + sy) * kAtlasWidth + ox + sx] * (column ? fx : 1 - fx) * (row ? fy : 1 - fy);
    }
  return coverage / 255.0F;
}

// Returns nullptr once the mark is blended, else why it could not be.
const char* RenderSystemI420(State* state, GstBuffer* buffer, GstVideoInfo* info) {
  using namespace hm::draw_display::analytics::detail;
  if (!gst_buffer_is_writable(buffer) || GST_VIDEO_INFO_WIDTH(info) % 2 || GST_VIDEO_INFO_HEIGHT(info) % 2)
    return "buffer is not writable or has odd dimensions";
  if (state->cpu_font_failed)
    return FontUnavailableReason().c_str();
  if (state->cpu_atlas.empty()) {
    // Build into a local first. Keeping a resized but unbuilt atlas would let
    // the next buffer skip this branch and blend nothing, which is how an
    // unmarked frame would reach the encoder.
    std::vector<uint8_t> atlas(kAtlasBytes);
    if (!BuildAtlas(atlas.data(), state->font_path)) {
      state->cpu_font_failed = true;
      return FontUnavailableReason().c_str();
    }
    state->cpu_atlas = std::move(atlas);
  }
  GstVideoFrame frame{};
  if (!gst_video_frame_map(&frame, info, buffer, GST_MAP_WRITE))
    return "could not map the frame for writing";
  const int width = GST_VIDEO_INFO_WIDTH(info), height = GST_VIDEO_INFO_HEIGHT(info);
  state->commands.Clear();
  if (!hm::draw_display::AppendWatermark(&state->commands, width, height)) {
    gst_video_frame_unmap(&frame);
    return "watermark command capacity exhausted";
  }
  auto* y_plane = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  auto* u_plane = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
  auto* v_plane = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
  const int y_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  const int u_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
  const int v_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);
  // Software encoders already receive system-memory I420. Touch only this
  // small final-output rectangle; no GPU frame is downloaded.
  for (int y = std::max(0, height - 112) & ~1; y < height; y += 2)
    for (int x = std::max(0, width - 352) & ~1; x < width; x += 2) {
      float luma[4] = {
          static_cast<float>(y_plane[y * y_pitch + x]),
          static_cast<float>(y_plane[y * y_pitch + x + 1]),
          static_cast<float>(y_plane[(y + 1) * y_pitch + x]),
          static_cast<float>(y_plane[(y + 1) * y_pitch + x + 1])};
      float u = u_plane[(y / 2) * u_pitch + x / 2];
      float v = v_plane[(y / 2) * v_pitch + x / 2];
      bool touched = false;
      for (size_t index = 0; index < state->commands.size(); ++index) {
        const Command& glyph = state->commands.data()[index];
        if (glyph.kind != Kind::kGlyph || x + 2 <= glyph.x0 || x >= glyph.x1 || y + 2 <= glyph.y0 || y >= glyph.y1)
          continue;
        float alpha_sum = 0;
        for (int row = 0; row < 2; ++row)
          for (int column = 0; column < 2; ++column) {
            const float alpha =
                glyph.color.alpha * GlyphCoverage(glyph, x + column + 0.5F, y + row + 0.5F, state->cpu_atlas.data());
            luma[row * 2 + column] = luma[row * 2 + column] * (1 - alpha) + (16.0F + 219.0F * 0.2126F) * alpha;
            alpha_sum += alpha;
          }
        if (alpha_sum > 0) {
          const float alpha = alpha_sum * 0.25F;
          u = u * (1 - alpha) + (128.0F - 224.0F * 0.2126F / 1.8556F) * alpha;
          v = v * (1 - alpha) + (128.0F + 224.0F * (1.0F - 0.2126F) / 1.5748F) * alpha;
          touched = true;
        }
      }
      if (!touched)
        continue;
      for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 2; ++column)
          y_plane[(y + row) * y_pitch + x + column] =
              static_cast<uint8_t>(std::lround(std::clamp(luma[row * 2 + column], 0.0F, 255.0F)));
      u_plane[(y / 2) * u_pitch + x / 2] = static_cast<uint8_t>(std::lround(std::clamp(u, 0.0F, 255.0F)));
      v_plane[(y / 2) * v_pitch + x / 2] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0F, 255.0F)));
    }
  gst_video_frame_unmap(&frame);
  return nullptr;
}

#if defined(__aarch64__)
struct EglPlanes {
  NvBufSurface* surface;
  unsigned index;
  bool mapped{false};
  cudaGraphicsResource_t resource{nullptr};
  cudaEglFrame frame{};
  EglPlanes(NvBufSurface* surface, unsigned index) : surface(surface), index(index) {}
  ~EglPlanes() {
    if (resource)
      cudaGraphicsUnregisterResource(resource);
    if (mapped)
      NvBufSurfaceUnMapEglImage(surface, index);
  }
  bool Map() {
    if (NvBufSurfaceMapEglImage(surface, index) != 0)
      return false;
    mapped = true;
    if (cudaGraphicsEGLRegisterImage(&resource, surface->surfaceList[index].mappedAddr.eglImage, 0) != cudaSuccess ||
        cudaGraphicsResourceGetMappedEglFrame(&frame, resource, 0, 0) != cudaSuccess)
      return false;
    return frame.frameType == cudaEglFrameTypePitch;
  }
};
#endif

bool BuildImageView(
    NvBufSurface* surface,
    unsigned index,
    PixelFormat format,
    ImageView* image
#if defined(__aarch64__)
    ,
    EglPlanes* egl
#endif
) {
  const NvBufSurfaceParams& params = surface->surfaceList[index];
  const unsigned planes = format == PixelFormat::kP010 ? 2U : 3U;
  if (params.layout != NVBUF_LAYOUT_PITCH || params.planeParams.num_planes != planes || !params.width ||
      !params.height || params.width % 2 || params.height % 2)
    return false;
  *image = {};
  image->width = params.width;
  image->height = params.height;
  image->format = format;
#if defined(__aarch64__)
  if (surface->memType == NVBUF_MEM_SURFACE_ARRAY) {
    if (!egl->Map())
      return false;
    image->data = egl->frame.frame.pPitch[0].ptr;
    image->pitch = params.planeParams.pitch[0];
    image->chroma = egl->frame.frame.pPitch[1].ptr;
    // Jetson CUDA EGL reports the P010 UV pitch per 16-bit channel, whereas
    // NvBufSurface reports the byte stride of the interleaved UV plane.
    image->chroma_pitch = params.planeParams.pitch[1];
    if (planes == 3) {
      image->chroma_v = egl->frame.frame.pPitch[2].ptr;
      image->chroma_v_pitch = params.planeParams.pitch[2];
    }
    return image->data && image->chroma && (planes == 2 || image->chroma_v);
  }
#endif
  if (!params.dataPtr)
    return false;
  auto* base = static_cast<unsigned char*>(params.dataPtr);
  image->data = base + params.planeParams.offset[0];
  image->pitch = params.planeParams.pitch[0];
  image->chroma = base + params.planeParams.offset[1];
  image->chroma_pitch = params.planeParams.pitch[1];
  if (planes == 3) {
    image->chroma_v = base + params.planeParams.offset[2];
    image->chroma_v_pitch = params.planeParams.pitch[2];
  }
  return true;
}

GstPadProbeReturn Probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  auto* state = static_cast<State*>(user_data);
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer)
    return GST_PAD_PROBE_OK;
  std::string error;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstVideoInfo video_info{};
  PixelFormat format = PixelFormat::kI420;
  const bool nvmm = caps && gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:NVMM");
  if (!caps || !gst_video_info_from_caps(&video_info, caps)) {
    error = "could not inspect archive encoder input";
  } else if (GST_VIDEO_INFO_FORMAT(&video_info) == GST_VIDEO_FORMAT_P010_10LE) {
    format = PixelFormat::kP010;
  } else if (GST_VIDEO_INFO_FORMAT(&video_info) != GST_VIDEO_FORMAT_I420) {
    error = "unsupported archive encoder input format";
  }
  if (caps)
    gst_caps_unref(caps);
  if (error.empty() && !nvmm) {
    if (format != PixelFormat::kI420)
      error = "software archive watermark render failed: input is not I420";
    else if (const char* reason = RenderSystemI420(state, buffer, &video_info))
      error = std::string("software archive watermark render failed: ") + reason;
    else
      return GST_PAD_PROBE_OK;
  }
  if (error.empty() && cudaSetDevice(state->device) != cudaSuccess)
    error = "could not select archive watermark CUDA device";
  if (error.empty() && !state->stream &&
      cudaStreamCreateWithFlags(&state->stream, cudaStreamNonBlocking) != cudaSuccess)
    error = "could not create archive watermark CUDA stream";
  GstMapInfo map{};
  bool mapped = false;
  if (error.empty()) {
    // This maps only the small NvBufSurface descriptor. Video planes stay in
    // device memory and are never copied or CPU-mapped.
    mapped = gst_buffer_map(buffer, &map, GST_MAP_READ);
    if (!mapped || map.size < sizeof(NvBufSurface))
      error = "could not inspect archive NvBufSurface";
  }
  if (error.empty()) {
    auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
    if (!surface->surfaceList || !surface->numFilled || surface->numFilled > surface->batchSize ||
        surface->gpuId != static_cast<unsigned>(state->device)) {
      error = "invalid archive NvBufSurface batch";
    } else {
      if (!state->compositor)
        state->compositor = std::make_unique<hm::draw_display::analytics::Compositor>(
            hm::draw_display::analytics::Limits{}, state->font_path.empty() ? nullptr : state->font_path.c_str());
      for (unsigned index = 0; index < surface->numFilled; ++index) {
        ImageView image{};
#if defined(__aarch64__)
        EglPlanes egl(surface, index);
        if (!BuildImageView(surface, index, format, &image, &egl)) {
#else
        if (!BuildImageView(surface, index, format, &image)) {
#endif
          error = "invalid archive watermark GPU plane layout";
          break;
        }
        state->commands.Clear();
        if (!hm::draw_display::AppendWatermark(&state->commands, image.width, image.height)) {
          error = "archive watermark command capacity exhausted";
          break;
        }
        const auto result = state->compositor->Render(image, state->commands, state->stream);
        if (result.status != hm::draw_display::analytics::RenderStatus::kOk) {
          cudaStreamSynchronize(state->stream);
          error = std::string("archive watermark GPU render failed: status=") +
              hm::draw_display::analytics::ToString(result.status) + hm::draw_display::analytics::Remedy(result.status);
          if (result.status != hm::draw_display::analytics::RenderStatus::kFontUnavailable) {
            // Surface geometry says nothing about a missing font.
            error += std::string(" cuda=") + cudaGetErrorString(result.cuda_error) +
                " size=" + std::to_string(image.width) + "x" + std::to_string(image.height) +
                " pitches=" + std::to_string(image.pitch) + "," + std::to_string(image.chroma_pitch);
          }
          break;
        }
        // The encoder uses its own stream. Complete the sparse ROI write before
        // handing the buffer to it or unmapping a Jetson EGL surface.
        if (cudaStreamSynchronize(state->stream) != cudaSuccess) {
          error = "archive watermark GPU synchronization failed";
          break;
        }
      }
    }
  }
  if (mapped)
    gst_buffer_unmap(buffer, &map);
  if (error.empty())
    return GST_PAD_PROBE_OK;
  if (!state->error_reported) {
    state->error_reported = true;
    GST_ELEMENT_ERROR(state->owner, RESOURCE, FAILED, ("%s", error.c_str()), ("archive watermark"));
  }
  return GST_PAD_PROBE_DROP;
}

} // namespace

bool Install(GstElement* caps_filter, int gpu_id, std::string font_path) {
  GstPad* pad = gst_element_get_static_pad(caps_filter, "src");
  if (!pad)
    return false;
  auto* state = new State(caps_filter, gpu_id, std::move(font_path));
  const gulong id = gst_pad_add_probe(
      pad, GST_PAD_PROBE_TYPE_BUFFER, Probe, state, [](gpointer data) { delete static_cast<State*>(data); });
  gst_object_unref(pad);
  if (id)
    return true;
  delete state;
  return false;
}

} // namespace hm::archive_watermark
