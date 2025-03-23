#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "deepstream/sources/includes/nvll_osd_struct.h"
#include "hstream/src/libs/common/Status.h"
#include "jetson-utils/cuda/cudaDraw.h"

namespace hm {
namespace draw_display {

namespace {

const float4 no_color = make_float4(0, 0, 0, 0);

template <typename T>
inline __device__ __host__ T sqr(T x) {
  return x * x;
}

inline float dist2(float x1, float y1, float x2, float y2) {
  return sqr(x1 - x2) + sqr(y1 - y2);
}
inline float dist(float x1, float y1, float x2, float y2) {
  return sqrtf(dist2(x1, y1, x2, y2));
}

// Helper to get the background color from a meta struct.
template <typename T>
inline float4 bg_color(const T& meta) {
  return meta.has_bg_color
      ? make_float4(meta.bg_color.red, meta.bg_color.green, meta.bg_color.blue, meta.bg_color.alpha)
      : no_color;
}

inline float4 scale_and_clamp_color(const float4& clr) {
  return {
      std::clamp(clr.x * 255, 0.0f, 255.0f),
      std::clamp(clr.y * 255, 0.0f, 255.0f),
      std::clamp(clr.z * 255, 0.0f, 255.0f),
      std::clamp(clr.w * 255, 0.0f, 255.0f)};
}

inline uchar4 scale_and_clamp_color(const NvOSD_ColorParams& clr) {
  return {
      (uint8_t)std::clamp(clr.red * 255, 0.0, 255.0),
      (uint8_t)std::clamp(clr.green * 255, 0.0, 255.0),
      (uint8_t)std::clamp(clr.blue * 255, 0.0, 255.0),
      (uint8_t)std::clamp(clr.alpha * 255, 0.0, 255.0)};
}

#if 0
const float3 html_colors[32] = {
    {0.000f, 0.000f, 0.000f}, // Black
    {1.000f, 1.000f, 1.000f}, // White
    {1.000f, 0.000f, 0.000f}, // Red
    {0.000f, 1.000f, 0.000f}, // Lime
    {0.000f, 0.000f, 1.000f}, // Blue
    {1.000f, 1.000f, 0.000f}, // Yellow
    {0.000f, 1.000f, 1.000f}, // Cyan / Aqua
    {1.000f, 0.000f, 1.000f}, // Magenta / Fuchsia
    {0.753f, 0.753f, 0.753f}, // Silver (approx 192/255)
    {0.502f, 0.502f, 0.502f}, // Gray (approx 128/255)
    {0.502f, 0.000f, 0.000f}, // Maroon (approx 128/255, 0, 0)
    {0.502f, 0.502f, 0.000f}, // Olive (approx 128,128,0)
    {0.000f, 0.502f, 0.000f}, // Green (approx 0,128,0)
    {0.502f, 0.000f, 0.502f}, // Purple (approx 128,0,128)
    {0.000f, 0.502f, 0.502f}, // Teal (approx 0,128,128)
    {0.000f, 0.000f, 0.502f}, // Navy (approx 0,0,128)
    {1.000f, 0.647f, 0.000f}, // Orange (255,165,0)
    {1.000f, 0.843f, 0.000f}, // Gold (255,215,0)
    {1.000f, 0.753f, 0.796f}, // Pink (255,192,203)
    {0.647f, 0.165f, 0.165f}, // Brown (165,42,42)
    {0.863f, 0.078f, 0.235f}, // Crimson (220,20,60)
    {0.294f, 0.000f, 0.510f}, // Indigo (75,0,130)
    {0.933f, 0.510f, 0.933f}, // Violet (238,130,238)
    {0.941f, 0.902f, 0.549f}, // Khaki (240,230,140)
    {0.855f, 0.439f, 0.839f}, // Orchid (218,112,214)
    {0.251f, 0.878f, 0.816f}, // Turquoise (64,224,208)
    {0.980f, 0.502f, 0.447f}, // Salmon (250,128,114)
    {1.000f, 0.498f, 0.314f}, // Coral (255,127,80)
    {0.416f, 0.353f, 0.804f}, // SlateBlue (106,90,205)
    {0.000f, 1.000f, 0.498f}, // SpringGreen (0,255,127)
    {0.196f, 0.804f, 0.196f}, // LimeGreen (50,205,50)
    {0.098f, 0.098f, 0.439f} // MidnightBlue (25,25,112)
};
const float3& get_object_color(size_t object_id) {
  return html_colors[object_id % sizeof(html_colors) / sizeof(html_colors[0])];
}
#else
// Convert HSV to RGB. Hue is in [0, 1), Saturation and Value in [0, 1].
// Returns a float4 with alpha set to 1.0.
float4 hsv2rgb(float h, float s, float v) {
  float r, g, b;
  int i = static_cast<int>(h * 6);
  float f = h * 6 - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  switch (i % 6) {
    case 0:
      r = v;
      g = t;
      b = p;
      break;
    case 1:
      r = q;
      g = v;
      b = p;
      break;
    case 2:
      r = p;
      g = v;
      b = t;
      break;
    case 3:
      r = p;
      g = q;
      b = v;
      break;
    case 4:
      r = t;
      g = p;
      b = v;
      break;
    case 5:
      r = v;
      g = p;
      b = q;
      break;
    default:
      r = g = b = 0;
      break;
  }
  return {r, g, b, 1.0f};
}
constexpr size_t kNumColors = 32;
std::vector<float4> object_colors(kNumColors);

const float4& get_object_color(size_t object_id) {
  static bool initialized = false;
  if (!initialized) {
    // Choose parameters for high contrast on white:
    // High saturation ensures vivid colors, while a moderate value (brightness)
    // keeps the colors from being washed out on a white background.
    float saturation = 0.9f;
    float value = 0.7f;

    // Generate colors by evenly spacing the hue around the color wheel.
    for (size_t i = 0; i < kNumColors; ++i) {
      float hue = static_cast<float>(i) / kNumColors; // hue in [0,1)
      object_colors[i] = hsv2rgb(hue, saturation, value);
    }
    initialized = true;
  }
  return object_colors[object_id % kNumColors];
}
#endif
} // namespace

// Overloaded helper for rectangles. This version takes an NvOSD_RectParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_RectParams& rect,
    float scale,
    cudaStream_t stream) {
  int left = scale * rect.left;
  int top = scale * rect.top;
  int right = scale * (rect.left + rect.width);
  int bottom = scale * (rect.top + rect.height);
  float4 fillColor = bg_color(rect);
  float4 borderColor =
      make_float4(rect.border_color.red, rect.border_color.green, rect.border_color.blue, rect.border_color.alpha);
  float lineWidth = rect.border_width;
  return ::cudaDrawRect(
      image,
      width,
      height,
      format,
      left,
      top,
      right,
      bottom,
      scale_and_clamp_color(fillColor),
      scale_and_clamp_color(borderColor),
      std::max(scale * lineWidth, 1.0f),
      stream);
}

// Overloaded helper for circles. This version takes an NvOSD_CircleParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_CircleParams& circle,
    float scale,
    cudaStream_t stream) {
  int cx = scale * circle.xc;
  int cy = scale * circle.yc;
  float radius = scale * static_cast<float>(circle.radius);
  float4 color = make_float4(
      circle.circle_color.red, circle.circle_color.green, circle.circle_color.blue, circle.circle_color.alpha);
  float inner_radius = radius - circle.circle_width;
  if (inner_radius < 0) {
    inner_radius = 0;
  }
  return ::cudaDrawCircle(
      image, width, height, format, cx, cy, radius, inner_radius, scale_and_clamp_color(color), stream);
}

// Overloaded helper for lines. This version takes an NvOSD_LineParams struct.
cudaError_t cudaDraw(
    void* image,
    size_t width,
    size_t height,
    imageFormat format,
    const NvOSD_LineParams& line,
    float scale,
    cudaStream_t stream) {
  int x1 = scale * line.x1;
  int y1 = scale * line.y1;
  int x2 = scale * line.x2;
  int y2 = scale * line.y2;
  if (dist(x1, y1, x2, y2) < 2.0) {
    return cudaError_t::cudaSuccess;
  }
  float4 color = make_float4(line.line_color.red, line.line_color.green, line.line_color.blue, line.line_color.alpha);
  float lineWidth = line.line_width;
  cudaError_t cuerr = ::cudaDrawLine(
      image,
      width,
      height,
      format,
      x1,
      y1,
      x2,
      y2,
      scale_and_clamp_color(color),
      std::max(scale * lineWidth, 1.0f),
      stream);
  return cuerr;
}

absl::Status draw_display_meta(
    surface::Surface surface,
    const NvDsDisplayMeta* display_meta,
    std::shared_ptr<FontCache> font_cache,
    float scale,
    cudaStream_t stream) {
  // For now, we assume no extra pitch.
  assert(surface.pitch_width() == surface.width());

  const imageFormat format = surface.get_image_format();
  const int ww = surface.width();
  const int hh = surface.height();

  // Draw each rectangle.
  for (size_t i = 0; i < display_meta->num_rects; ++i) {
    const NvOSD_RectParams& rect = display_meta->rect_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, rect, scale, stream));
  }
  // Draw each circle.
  for (size_t i = 0; i < display_meta->num_circles; ++i) {
    const NvOSD_CircleParams& circle = display_meta->circle_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, circle, scale, stream));
  }
  // Draw each line.
  for (size_t i = 0; i < display_meta->num_lines; ++i) {
    const NvOSD_LineParams& line = display_meta->line_params[i];
    XCUDA_RETURN_IF_ERROR(cudaDraw(surface.dataptr(), ww, hh, format, line, scale, stream));
  }
  for (size_t i = 0; i < display_meta->num_labels; ++i) {
    const NvOSD_TextParams& text = display_meta->text_params[i];
    if (!text.display_text || !*text.display_text) {
      continue;
    }
    std::shared_ptr<Font> font;
    HM_ASSIGN_OR_RETURN(font, font_cache->get_or_create_font(text.font_params.font_name, text.font_params.font_size));
    std::pair<int, int> newpos;
    uchar4 text_color = scale_and_clamp_color(text.font_params.font_color);
    uchar4 bg_color = text.set_bg_clr ? scale_and_clamp_color(text.text_bg_clr) : uchar4{0,0,0,0};
    HM_ASSIGN_OR_RETURN(
        newpos,
        font->draw(
            text.display_text,
            surface.dataptr(),
            surface.get_image_format(),
            surface.width(),
            surface.height(),
            surface.pitch(),
            text.x_offset,
            text.y_offset,
            text_color,
            bg_color,
            stream));
    (void)newpos;
  }
  return absl::OkStatus();
}

absl::Status draw_object_meta(
    surface::Surface surface,
    const NvDsObjectMeta* object_meta,
    std::shared_ptr<FontCache> font_cache,
    float scale,
    cudaStream_t stream) {
  if (object_meta->tracker_confidence < 0.1) {
    std::cout << "Low confidence tracking" << std::endl;
  }
  const NvOSD_RectParams* rect_params = &object_meta->rect_params;
  NvOSD_RectParams rparams;
  rparams = *rect_params;
  rparams.border_width = 1;
  const auto& clr = get_object_color(object_meta->object_id);
  rparams.border_color.red = clr.x;
  rparams.border_color.green = clr.y;
  rparams.border_color.blue = clr.z;

  // It can keep its alpha
  rect_params = &rparams;

  XCUDA_RETURN_IF_ERROR(cudaDraw(
      surface.dataptr(), surface.width(), surface.height(), surface.get_image_format(), *rect_params, scale, stream));

  if (*object_meta->obj_label) {
    const NvOSD_TextParams& text = object_meta->text_params;
    float adapted_font_size = adaptFontSize(surface.width()) / 10.0 * scale;
    std::shared_ptr<Font> font;
    HM_ASSIGN_OR_RETURN(
        font,
        font_cache->get_or_create_font(text.font_params.font_name, text.font_params.font_size * adapted_font_size));
    std::pair<int, int> newpos;
    uchar4 text_color = scale_and_clamp_color(text.font_params.font_color);
    uchar4 bg_color = text.set_bg_clr ? scale_and_clamp_color(text.text_bg_clr) : uchar4{0,0,0,0};
    HM_ASSIGN_OR_RETURN(
        newpos,
        font->draw(
            text.display_text,
            surface.dataptr(),
            surface.get_image_format(),
            surface.width(),
            surface.height(),
            surface.pitch(),
            scale * text.x_offset,
            scale * text.y_offset,
            text_color,
            bg_color,
            stream));
    (void)newpos;
  }

  return absl::OkStatus();
}

} // namespace draw_display
} // namespace hm
