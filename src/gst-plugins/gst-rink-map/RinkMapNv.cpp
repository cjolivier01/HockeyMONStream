#include <gstnvdsbufferpool.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <atomic>
#include <mutex>
#include "hstream/src/gst-plugins/gst-amd-program/ProgramElement.h"
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cuda_egl_interop.h>
#endif

namespace hm::rink_map {
namespace {
namespace a = draw_display::analytics;
using a::ImageView;
using a::PixelFormat;
using Buffer = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>;
struct Mapping {
  GstBuffer* buffer;
  GstMapInfo info{};
  // Maps the NvBufSurface descriptor only. Pixels never enter host memory.
  explicit Mapping(GstBuffer* b) : buffer(b) {
    if (!gst_buffer_map(b, &info, GST_MAP_READ))
      buffer = nullptr;
  }
  ~Mapping() {
    if (buffer)
      gst_buffer_unmap(buffer, &info);
  }
  NvBufSurface* get() const {
    return buffer && info.size >= sizeof(NvBufSurface) ? reinterpret_cast<NvBufSurface*>(info.data) : nullptr;
  }
};
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
  const bool packed = format == PixelFormat::kRgba8 || format == PixelFormat::kRgb10A2;
  const unsigned planes = packed ? 1U : format == PixelFormat::kP010 ? 2U : 3U;
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
    if (packed)
      return image->data != nullptr;
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
  if (packed)
    return true;
  image->chroma = base + params.planeParams.offset[1];
  image->chroma_pitch = params.planeParams.pitch[1];
  if (planes == 3) {
    image->chroma_v = base + params.planeParams.offset[2];
    image->chroma_v_pitch = params.planeParams.pitch[2];
  }
  return true;
}

class Overlay final : public program::gst::Component {
 public:
  explicit Overlay(const Configuration& config) : config_(config), map_(config.map, config.settings.options) {}
  ~Overlay() override {
    if (stream_) {
      cudaSetDevice(config_.device);
      cudaStreamSynchronize(stream_);
      compositor_.reset();
      cudaStreamDestroy(stream_);
    }
    ClearPool();
  }
  void Interrupt() noexcept override {
    interrupted_.store(true);
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (pool_)
      gst_buffer_pool_set_flushing(pool_, TRUE);
  }
  absl::Status Reset() override {
    map_.Reset();
    last_reason_.clear();
    std::lock_guard<std::mutex> lock(pool_mutex_);
    interrupted_.store(false);
    if (pool_)
      gst_buffer_pool_set_flushing(pool_, FALSE);
    return absl::OkStatus();
  }
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    if (!config_.settings.enabled)
      return input;
    if (interrupted_.load())
      return absl::CancelledError("Rink map is flushing");
    Mapping source(input);
    auto* surface = source.get();
    if (!surface || !surface->surfaceList || surface->numFilled != 1 || surface->batchSize != 1 ||
        surface->gpuId != unsigned(config_.device))
      return absl::InvalidArgumentError("Rink map requires one GPU-resident stitched/Program frame");
    auto* batch = gst_buffer_get_nvds_batch_meta(input);
    const auto* frame =
        batch && batch->frame_meta_list ? static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data) : nullptr;
    unsigned width = 0, height = 0;
    if (const auto* transform = preview_overlay::find_playcropper_transform_meta(frame)) {
      width = transform->input_width;
      height = transform->input_height;
    } else {
      width = frame ? frame->source_frame_width : 0;
      height = frame ? frame->source_frame_height : 0;
    }
    const auto* portable = FindFrame(input, nullptr);
    auto data = portable ? *portable : draw_display::ReadRinkMapFrame(frame, width, height);
    const auto& p = surface->surfaceList[0];
    commands_.Clear();
    if (!map_.Append(data, p.width, p.height, &commands_)) {
      const char* reason = map_.unavailableReason();
      if (reason && last_reason_ != reason)
        g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=%s\n", reason);
      last_reason_ = reason ? reason : "";
      return input;
    }
    last_reason_.clear();
    const char* format = nullptr;
    PixelFormat pixel;
    switch (p.colorFormat) {
      case NVBUF_COLOR_FORMAT_YUV420:
      case NVBUF_COLOR_FORMAT_YUV420_709:
        format = "I420";
        pixel = PixelFormat::kI420;
        break;
      case NVBUF_COLOR_FORMAT_NV12_10LE:
      case NVBUF_COLOR_FORMAT_NV12_10LE_709:
        format = "P010_10LE";
        pixel = PixelFormat::kP010;
        break;
      case NVBUF_COLOR_FORMAT_RGBA:
        format = "RGBA";
        pixel = PixelFormat::kRgba8;
        break;
      case NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709:
        format = "RGB10A2_LE";
        pixel = PixelFormat::kRgb10A2;
        break;
      default:
        return absl::InvalidArgumentError("Unsupported rink-map GPU color format");
    }
    if (cudaSetDevice(config_.device) != cudaSuccess ||
        (!stream_ && cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) != cudaSuccess))
      return absl::InternalError("Cannot initialize rink-map CUDA stream");
    if (!pool_ || width_ != p.width || height_ != p.height || format_ != p.colorFormat) {
      ClearPool();
      std::lock_guard<std::mutex> lock(pool_mutex_);
      pool_ = gst_nvds_buffer_pool_new();
      auto* caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          format,
          "width",
          G_TYPE_INT,
          int(p.width),
          "height",
          G_TYPE_INT,
          int(p.height),
          nullptr);
      gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
      auto* settings = gst_buffer_pool_get_config(pool_);
      gst_buffer_pool_config_set_params(settings, caps, sizeof(NvBufSurface), 0, config_.settings.pool_frames);
      gst_structure_set(
          settings,
          "memtype",
          G_TYPE_UINT,
          unsigned(surface->memType),
          "gpu-id",
          G_TYPE_UINT,
          unsigned(config_.device),
          "batch-size",
          G_TYPE_UINT,
          1U,
          nullptr);
      gst_caps_unref(caps);
      if (!gst_buffer_pool_set_config(pool_, settings) || !gst_buffer_pool_set_active(pool_, TRUE))
        return absl::ResourceExhaustedError("Cannot allocate rink-map output pool");
      if (interrupted_.load())
        gst_buffer_pool_set_flushing(pool_, TRUE);
      width_ = p.width;
      height_ = p.height;
      format_ = p.colorFormat;
    }
    GstBuffer* raw = nullptr;
    // Ordinary backpressure retains a bounded number of frames. FLUSH_START
    // and PAUSED->READY interrupt this wait before joining the streaming task.
    const auto acquired = gst_buffer_pool_acquire_buffer(pool_, &raw, nullptr);
    if (acquired == GST_FLOW_FLUSHING)
      return absl::CancelledError("Rink-map pool is flushing");
    if (acquired != GST_FLOW_OK)
      return absl::ResourceExhaustedError("Cannot acquire rink-map output surface");
    Buffer output(raw, gst_buffer_unref);
    Mapping destination(raw);
    auto* target = destination.get();
    if (!target || NvBufSurfaceCopy(surface, target) != 0)
      return absl::InternalError("Cannot copy rink-map GPU output");
    target->numFilled = 1;
    if (!gst_buffer_copy_into(raw, input, GST_BUFFER_COPY_METADATA, 0, -1))
      return absl::InternalError("Cannot copy rink-map output metadata");
    ImageView image{};
#if defined(__aarch64__)
    EglPlanes egl(target, 0);
    if (!BuildImageView(target, 0, pixel, &image, &egl))
#else
    if (!BuildImageView(target, 0, pixel, &image))
#endif
      return absl::InvalidArgumentError("Invalid rink-map GPU plane layout");
    if (!compositor_)
      compositor_ = std::make_unique<a::Compositor>();
    const auto result = compositor_->Render(image, commands_, stream_);
    // Encoder/VIC consumers use separate streams; complete before handing off.
    const auto completed = cudaStreamSynchronize(stream_);
    if (completed != cudaSuccess || result.status == a::RenderStatus::kCudaError ||
        result.status == a::RenderStatus::kInvalidArgument)
      return absl::InternalError(std::string("Rink-map GPU render failed: ") + a::ToString(result.status));
    if (result.status != a::RenderStatus::kOk)
      g_printerr("HSTREAM_OUTPUT_RINK_MAP status=suppressed reason=%s\n", a::ToString(result.status));
    return output.release();
  }

 private:
  void ClearPool() {
    GstBufferPool* retired = nullptr;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      std::swap(retired, pool_);
    }
    if (retired) {
      gst_buffer_pool_set_active(retired, FALSE);
      gst_object_unref(retired);
    }
  }
  Configuration config_;
  draw_display::RinkMiniMap map_;
  a::CommandList commands_;
  std::string last_reason_;
  std::mutex pool_mutex_;
  std::atomic<bool> interrupted_{false};
  GstBufferPool* pool_{};
  unsigned width_{}, height_{};
  NvBufSurfaceColorFormat format_{};
  cudaStream_t stream_{};
  std::unique_ptr<a::Compositor> compositor_;
};
} // namespace
absl::StatusOr<GstElement*> Create(const Configuration& config) {
  return program::gst::CreateElement("hmrinkmap", nullptr, std::make_unique<Overlay>(config));
}
} // namespace hm::rink_map
