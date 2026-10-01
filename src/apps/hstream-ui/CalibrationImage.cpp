#include "src/apps/hstream-ui/CalibrationImage.h"

#include <png.h>
#include <tiffio.h>
#include <QtCore/QFile>
#include <QtGui/QImageReader>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
constexpr png_uint_32 kMaximumSourceDimension = 65535;
constexpr uint64_t kMaximumSourcePixels = 256ULL * 1024ULL * 1024ULL;

struct PngDecodeContext {
  FILE* file{nullptr};
  png_structp png{nullptr};
  png_infop info{nullptr};
  unsigned char* source_row{nullptr};
  unsigned char* preview_pixels{nullptr};
};

struct PngPreview {
  unsigned char* pixels{nullptr};
  int width{0};
  int height{0};
  int stride{0};
};

void release_png_context(PngDecodeContext* context) {
  if (!context)
    return;
  std::free(context->source_row);
  std::free(context->preview_pixels);
  if (context->png)
    png_destroy_read_struct(&context->png, context->info ? &context->info : nullptr, nullptr);
  if (context->file)
    std::fclose(context->file);
  std::free(context);
}

PngPreview read_bounded_png_preview(const QByteArray& path, const QSize& requested_size) {
  PngPreview result;
  auto* context = static_cast<PngDecodeContext*>(std::calloc(1, sizeof(PngDecodeContext)));
  if (!context)
    return result;
  context->file = std::fopen(path.constData(), "rb");
  if (!context->file) {
    release_png_context(context);
    return result;
  }
  context->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!context->png) {
    release_png_context(context);
    return result;
  }
  context->info = png_create_info_struct(context->png);
  if (!context->info) {
    release_png_context(context);
    return result;
  }
  if (setjmp(png_jmpbuf(context->png))) {
    release_png_context(context);
    return result;
  }

  png_set_user_limits(context->png, kMaximumSourceDimension, kMaximumSourceDimension);
  png_init_io(context->png, context->file);
  png_read_info(context->png, context->info);
  png_uint_32 source_width = 0;
  png_uint_32 source_height = 0;
  int bit_depth = 0;
  int color_type = 0;
  int interlace_type = 0;
  png_get_IHDR(
      context->png,
      context->info,
      &source_width,
      &source_height,
      &bit_depth,
      &color_type,
      &interlace_type,
      nullptr,
      nullptr);
  const uint64_t source_pixels = static_cast<uint64_t>(source_width) * source_height;
  if (source_width == 0 || source_height == 0 || source_pixels > kMaximumSourcePixels || requested_size.width() <= 0 ||
      requested_size.height() <= 0 || static_cast<png_uint_32>(requested_size.width()) > source_width ||
      static_cast<png_uint_32>(requested_size.height()) > source_height || interlace_type != PNG_INTERLACE_NONE) {
    release_png_context(context);
    return result;
  }

  if (bit_depth == 16)
    png_set_strip_16(context->png);
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(context->png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(context->png);
  const bool has_transparency = png_get_valid(context->png, context->info, PNG_INFO_tRNS);
  if (has_transparency)
    png_set_tRNS_to_alpha(context->png);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(context->png);
  if (!(color_type & PNG_COLOR_MASK_ALPHA) && !has_transparency)
    png_set_add_alpha(context->png, 0xff, PNG_FILLER_AFTER);
  png_read_update_info(context->png, context->info);
  if (png_get_bit_depth(context->png, context->info) != 8 || png_get_channels(context->png, context->info) != 4) {
    release_png_context(context);
    return result;
  }

  const png_size_t source_stride = png_get_rowbytes(context->png, context->info);
  if (source_stride != static_cast<png_size_t>(source_width) * 4) {
    release_png_context(context);
    return result;
  }
  const size_t preview_stride = static_cast<size_t>(requested_size.width()) * 4;
  const size_t preview_bytes = preview_stride * static_cast<size_t>(requested_size.height());
  context->source_row = static_cast<unsigned char*>(std::malloc(source_stride));
  context->preview_pixels = static_cast<unsigned char*>(std::malloc(preview_bytes));
  if (!context->source_row || !context->preview_pixels) {
    release_png_context(context);
    return result;
  }

  int preview_y = 0;
  for (png_uint_32 source_y = 0; source_y < source_height; ++source_y) {
    png_read_row(context->png, context->source_row, nullptr);
    if (preview_y >= requested_size.height())
      continue;
    const png_uint_32 selected_source_y = static_cast<png_uint_32>(
        (static_cast<uint64_t>(2 * preview_y + 1) * source_height) /
        (2 * static_cast<uint64_t>(requested_size.height())));
    if (source_y != selected_source_y)
      continue;
    unsigned char* preview_row = context->preview_pixels + static_cast<size_t>(preview_y) * preview_stride;
    for (int preview_x = 0; preview_x < requested_size.width(); ++preview_x) {
      const png_uint_32 source_x = static_cast<png_uint_32>(
          (static_cast<uint64_t>(2 * preview_x + 1) * source_width) /
          (2 * static_cast<uint64_t>(requested_size.width())));
      std::copy_n(context->source_row + static_cast<size_t>(source_x) * 4, 4, preview_row + preview_x * 4);
    }
    ++preview_y;
  }
  png_read_end(context->png, context->info);
  if (preview_y != requested_size.height()) {
    release_png_context(context);
    return result;
  }

  result = {context->preview_pixels, requested_size.width(), requested_size.height(), static_cast<int>(preview_stride)};
  context->preview_pixels = nullptr;
  release_png_context(context);
  return result;
}

void free_png_preview(void* pixels) {
  std::free(pixels);
}

} // namespace

QImage readCalibrationPng(const QString& path, const QSize& size) {
  PngPreview preview = read_bounded_png_preview(QFile::encodeName(path), size);
  return QImage(
      preview.pixels,
      preview.width,
      preview.height,
      preview.stride,
      QImage::Format_RGBA8888,
      free_png_preview,
      preview.pixels);
}

QImage readCalibrationTiff(const QString& path, int maximum_dimension) {
  std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(QFile::encodeName(path).constData(), "r"), TIFFClose);
  if (!file)
    return {};
  if (maximum_dimension <= 0 || maximum_dimension > 8192)
    return {};
  uint32_t width = 0, height = 0;
  uint16_t bits = 0, samples = 0, planar = 0, photometric = 0, orientation = 0, sample_format = 0;
  TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ORIENTATION, &orientation);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  // Enblend writes stripped, contiguous RGB(A). Decode one row at a time so a
  // native panorama never needs a full-size CPU bitmap or a Qt TIFF plugin.
  if (!width || !height || width > kMaximumSourceDimension || height > kMaximumSourceDimension ||
      uint64_t(width) * height > kMaximumSourcePixels || TIFFIsTiled(file.get()) || (bits != 8 && bits != 16) ||
      (samples != 3 && samples != 4) || planar != PLANARCONFIG_CONTIG || photometric != PHOTOMETRIC_RGB ||
      orientation != ORIENTATION_TOPLEFT || sample_format != SAMPLEFORMAT_UINT)
    return {};
  uint32_t rows_per_strip = 0;
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
  const uint64_t stride = uint64_t(width) * samples * (bits / 8);
  if (TIFFScanlineSize64(file.get()) != stride || stride * std::min(rows_per_strip, height) > 64ULL * 1024 * 1024)
    return {};
  QSize size(width, height);
  if (std::max(width, height) > static_cast<uint32_t>(maximum_dimension))
    size.scale(maximum_dimension, maximum_dimension, Qt::KeepAspectRatio);
  QImage image(size, QImage::Format_RGB32);
  if (image.isNull())
    return {};
  uint16_t extra_count = 0;
  uint16_t* extras = nullptr;
  const bool unassociated_alpha = samples == 4 &&
      TIFFGetField(file.get(), TIFFTAG_EXTRASAMPLES, &extra_count, &extras) && extra_count == 1 &&
      extras[0] == EXTRASAMPLE_UNASSALPHA;
  std::vector<uint16_t> row((stride + 1) / 2);
  int output_y = 0;
  for (uint32_t y = 0; y < height; ++y) {
    if (TIFFReadScanline(file.get(), row.data(), y) < 0)
      return {};
    if (output_y >= size.height() || y != uint64_t(2 * output_y + 1) * height / (2 * size.height()))
      continue;
    auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(output_y++));
    for (int x = 0; x < size.width(); ++x) {
      const size_t offset = uint64_t(2 * x + 1) * width / (2 * size.width()) * samples;
      const auto component = [&](size_t channel) {
        return bits == 16 ? row[offset + channel] >> 8
                          : reinterpret_cast<const unsigned char*>(row.data())[offset + channel];
      };
      // Composite unassociated TIFF alpha over the black preview background;
      // associated-alpha RGB already has this multiplication applied.
      const int alpha = unassociated_alpha ? component(3) : 255;
      pixels[x] = qRgb(component(0) * alpha / 255, component(1) * alpha / 255, component(2) * alpha / 255);
    }
  }
  return image;
}
