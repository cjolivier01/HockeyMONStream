#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"

#include <gst/va/gstva.h>
#include <gst/video/video.h>
#include <va/va_drmcommon.h>

#include <unistd.h>

#include <string>

namespace hm::gpu {
namespace {
absl::Status HipStatus(hipError_t error, const char* operation) {
  return error == hipSuccess ? absl::OkStatus()
                             : absl::InternalError(std::string(operation) + ": " + hipGetErrorString(error));
}
absl::Status Invalid(const char* message) {
  return absl::InvalidArgumentError(message);
}

// HIP's device selection is thread-local; cleanup may run on another thread.
class DeviceScope {
 public:
  explicit DeviceScope(int device) {
    error_ = hipGetDevice(&previous_);
    if (error_ == hipSuccess && previous_ != device)
      error_ = hipSetDevice(device);
  }
  ~DeviceScope() {
    if (previous_ >= 0)
      (void)hipSetDevice(previous_);
  }
  hipError_t error() const {
    return error_;
  }

 private:
  int previous_{-1};
  hipError_t error_;
};
} // namespace

absl::StatusOr<std::unique_ptr<VaHipFrame>> VaHipFrame::Import(GstBuffer* buffer, hipStream_t stream, Access access) {
  if (!buffer || !stream)
    return Invalid("A VA buffer and an explicit HIP stream are required");
  if (access == Access::kWrite && (!gst_buffer_is_writable(buffer) || !gst_buffer_is_all_memory_writable(buffer))) {
    return Invalid("HIP writes require an exclusively owned VA buffer");
  }
  auto* display = gst_va_buffer_peek_display(buffer);
  const auto surface = gst_va_buffer_get_surface(buffer);
  const auto* meta = gst_buffer_get_video_meta(buffer);
  if (!display || surface == VA_INVALID_SURFACE || !meta || !meta->width || !meta->height) {
    return Invalid("Expected a VA surface with video layout metadata");
  }

  auto frame = std::unique_ptr<VaHipFrame>(new VaHipFrame());
  auto status = HipStatus(hipGetDevice(&frame->device_), "hipGetDevice");
  if (!status.ok())
    return status;
  frame->stream_ = stream;
  frame->buffer_ = gst_buffer_ref(buffer);
  auto va_display = gst_va_display_get_va_dpy(display);
  auto va_status = vaSyncSurface(va_display, surface);
  if (va_status != VA_STATUS_SUCCESS) {
    return absl::InternalError(std::string("vaSyncSurface: ") + vaErrorStr(va_status));
  }
  VADRMPRIMESurfaceDescriptor descriptor{};
  va_status = vaExportSurfaceHandle(
      va_display,
      surface,
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      (access == Access::kRead ? VA_EXPORT_SURFACE_READ_ONLY : VA_EXPORT_SURFACE_READ_WRITE) |
          VA_EXPORT_SURFACE_COMPOSED_LAYERS,
      &descriptor);
  if (va_status != VA_STATUS_SUCCESS) {
    return absl::InternalError(std::string("vaExportSurfaceHandle: ") + vaErrorStr(va_status));
  }
  // Adopt every returned descriptor before validation so all failure paths close them.
  for (unsigned i = 0; i < descriptor.num_objects && i < frame->objects_.size(); ++i) {
    frame->objects_[i].fd = descriptor.objects[i].fd;
  }
  if (!descriptor.num_objects || descriptor.num_objects > frame->objects_.size() || descriptor.num_layers != 1 ||
      descriptor.width < meta->width || descriptor.height < meta->height) {
    return Invalid("Unsupported VA DMA-BUF object/layer layout");
  }
  frame->width_ = meta->width;
  frame->height_ = meta->height;
  unsigned bytes_per_pixel = 0;
  switch (descriptor.fourcc) {
    case VA_FOURCC_RGBA:
      frame->format_ = Format::kRgba8;
      bytes_per_pixel = 4;
      frame->plane_count_ = 1;
      break;
    case VA_FOURCC_BGRA:
      frame->format_ = Format::kBgra8;
      bytes_per_pixel = 4;
      frame->plane_count_ = 1;
      break;
    case VA_FOURCC_NV12:
      frame->format_ = Format::kNv12;
      bytes_per_pixel = 1;
      frame->plane_count_ = 2;
      break;
    case VA_FOURCC_P010:
      frame->format_ = Format::kP010;
      bytes_per_pixel = 2;
      frame->plane_count_ = 2;
      break;
    default:
      return Invalid("Unsupported VA pixel format for HIP import");
  }
  const auto& layer = descriptor.layers[0];
  if (layer.num_planes != frame->plane_count_)
    return Invalid("Unexpected VA plane count");
  if (frame->plane_count_ == 2 && ((frame->width_ | frame->height_) & 1)) {
    return Invalid("Subsampled VA images require even dimensions");
  }
  for (unsigned i = 0; i < descriptor.num_objects; ++i) {
    const auto& object = descriptor.objects[i];
    if (object.drm_format_modifier != 0 || !object.size || object.fd < 0) {
      return Invalid("HIP requires linear DMA-BUF storage; insert a linear VA postprocessing output");
    }
  }
  // Check extents before importing any object or exposing pointers.
  for (unsigned i = 0; i < frame->plane_count_; ++i) {
    if (layer.object_index[i] >= descriptor.num_objects)
      return Invalid("Invalid VA plane object index");
    auto& plane = frame->planes_[i];
    plane.row_bytes = static_cast<size_t>(frame->width_) * bytes_per_pixel;
    plane.height = i ? frame->height_ / 2 : frame->height_;
    plane.pitch = layer.pitch[i];
    const size_t object_size = descriptor.objects[layer.object_index[i]].size;
    if (plane.pitch < plane.row_bytes || layer.offset[i] > object_size ||
        plane.row_bytes > object_size - layer.offset[i] ||
        plane.height - 1 > (object_size - layer.offset[i] - plane.row_bytes) / plane.pitch) {
      return Invalid("VA plane extent exceeds its DMA-BUF object");
    }
  }
  for (unsigned i = 0; i < descriptor.num_objects; ++i) {
    auto& object = frame->objects_[i];
    hipExternalMemoryHandleDesc handle{};
    handle.type = hipExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = object.fd;
    handle.size = descriptor.objects[i].size;
    status = HipStatus(hipImportExternalMemory(&object.memory, &handle), "hipImportExternalMemory");
    if (!status.ok())
      return status;
    hipExternalMemoryBufferDesc mapping{};
    mapping.size = handle.size;
    status = HipStatus(
        hipExternalMemoryGetMappedBuffer(&object.data, object.memory, &mapping), "hipExternalMemoryGetMappedBuffer");
    if (!status.ok())
      return status;
  }
  for (unsigned i = 0; i < frame->plane_count_; ++i) {
    frame->planes_[i].data = static_cast<uint8_t*>(frame->objects_[layer.object_index[i]].data) + layer.offset[i];
  }
  return frame;
}

absl::Status VaHipFrame::Complete() {
  if (complete_)
    return absl::OkStatus();
  DeviceScope device(device_);
  auto status = HipStatus(device.error(), "hipSetDevice");
  if (status.ok())
    status = HipStatus(hipStreamSynchronize(stream_), "hipStreamSynchronize");
  if (status.ok())
    complete_ = true;
  return status;
}

VaHipFrame::~VaHipFrame() {
  if (device_ < 0)
    return;
  DeviceScope device(device_);
  if (!complete_ && stream_)
    (void)hipStreamSynchronize(stream_);
  for (auto& object : objects_) {
    if (object.data)
      (void)hipFree(object.data);
    if (object.memory)
      (void)hipDestroyExternalMemory(object.memory);
    // ROCm's external-memory import borrows the fd (unlike CUDA's opaque-fd import).
    if (object.fd >= 0)
      close(object.fd);
  }
  if (buffer_)
    gst_buffer_unref(buffer_);
}

} // namespace hm::gpu
