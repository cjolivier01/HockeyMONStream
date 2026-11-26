#include "hstream/src/libs/scoreboard/CameraImages.h"
#include "hstream/src/libs/gpu/ConvertImage.h"

#include <mutex>
#include <vector>

#ifdef HSTREAM_USE_HIP
#include "hstream/src/libs/detection/VaDetectorImage.h"
#else
#include "hstream/src/libs/common/Surface.h"
#endif

namespace hm::scoreboard {
namespace {
absl::Status GpuStatus(cudaError_t result) {
  return result == cudaSuccess ? absl::OkStatus() : absl::InternalError(cudaGetErrorString(result));
}
} // namespace
struct CameraImages::Impl {
  struct Slot {
    gpu::Image image{};
    bool busy = false;
  };
  unsigned device;
  cudaStream_t stream = nullptr;
  std::mutex mutex;
  std::vector<Slot> slots;
  explicit Impl(unsigned value) : device(value) {}
  ~Impl() {
    // Leases retain this state. No buffer can be freed while still borrowed.
    int previous = -1;
    cudaGetDevice(&previous);
    cudaSetDevice(device);
    if (stream)
      cudaStreamSynchronize(stream);
    for (auto& slot : slots)
      if (slot.image.plane0)
        cudaFree(const_cast<void*>(slot.image.plane0));
    if (stream)
      cudaStreamDestroy(stream);
    if (previous >= 0)
      cudaSetDevice(previous);
  }
};
CameraImages::CameraImages(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraImages::~CameraImages() = default;
absl::StatusOr<std::unique_ptr<CameraImages>> CameraImages::Create(
    unsigned device,
    int width,
    int height,
    unsigned capacity) {
  if (width <= 0 || height <= 0 || !capacity || static_cast<size_t>(width) > SIZE_MAX / 8)
    return absl::InvalidArgumentError("Camera image pool requires explicit positive geometry and capacity");
  auto status = GpuStatus(cudaSetDevice(device));
  if (!status.ok())
    return status;
  auto impl = std::make_shared<Impl>(device);
  status = GpuStatus(cudaStreamCreateWithFlags(&impl->stream, cudaStreamNonBlocking));
  if (!status.ok())
    return status;
  impl->slots.resize(capacity);
  for (auto& slot : impl->slots) {
    void* pixels = nullptr;
    size_t pitch = 0;
    status = GpuStatus(cudaMallocPitch(&pixels, &pitch, static_cast<size_t>(width) * 8, height));
    if (!status.ok())
      return status;
    slot.image = {pixels, nullptr, pitch, 0, width, height, gpu::PixelFormat::kRgba16Float, 0, 0, 0, 0, 0, 0, 0};
  }
  return std::unique_ptr<CameraImages>(new CameraImages(std::move(impl)));
}
absl::StatusOr<std::shared_ptr<const gpu::Image>> CameraImages::Copy(
    const CameraDecodedFrame& frame,
    const CameraRegion& region) {
  if (!frame.sample || region.width != impl_->slots[0].image.width || region.height != impl_->slots[0].image.height)
    return absl::InvalidArgumentError("Camera image does not match its compact pool");
  auto status = GpuStatus(cudaSetDevice(impl_->device));
  if (!status.ok())
    return status;
  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    while (index < impl_->slots.size() && impl_->slots[index].busy)
      ++index;
    if (index == impl_->slots.size())
      return absl::ResourceExhaustedError("All scoreboard GPU images are retained");
    impl_->slots[index].busy = true;
  }
  std::shared_ptr<const gpu::Image> lease(&impl_->slots[index].image, [owner = impl_, index](const gpu::Image*) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    owner->slots[index].busy = false;
  });
  auto* buffer = gst_sample_get_buffer(frame.sample.get());
  if (!buffer)
    return absl::InvalidArgumentError("Camera sample has no buffer");
  gpu::Image input{};
#ifdef HSTREAM_USE_HIP
  if (frame.cropped)
    return absl::InvalidArgumentError("VA camera must supply uncropped source coordinates");
  auto imported = gpu::VaHipFrame::Import(buffer, impl_->stream, gpu::VaHipFrame::Access::kRead);
  if (!imported.ok())
    return imported.status();
  auto description = detection::DescribeVaDetectorImage(**imported, frame.info);
  if (!description.ok())
    return description.status();
  auto crop = CropCameraImage(*description, region);
  if (!crop.ok())
    return crop.status();
  input = *crop;
#else
  if (!frame.cropped || frame.info.width != region.width || frame.info.height != region.height)
    return absl::InvalidArgumentError("NVIDIA camera must supply the negotiated cropped image");
  // NVMM mapping exposes the surface descriptor, not host video pixels.
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return absl::UnavailableError("Cannot access camera NVMM descriptor");
  const auto unmap = [&map, buffer](void*) { gst_buffer_unmap(buffer, &map); };
  std::unique_ptr<void, decltype(unmap)> mapped(buffer, unmap);
  if (map.size < sizeof(NvBufSurface))
    return absl::DataLossError("Camera NVMM descriptor is truncated");
  auto* batch = reinterpret_cast<NvBufSurface*>(map.data);
  if (batch->numFilled != 1 || batch->batchSize < 1 || !batch->surfaceList || batch->gpuId != impl_->device)
    return absl::DataLossError("Camera NVMM surface has unexpected batch or device");
#ifdef __aarch64__
  surface::EglSurfaceMapper mapper(batch, 0, true);
  status = GpuStatus(mapper.status());
  if (!status.ok())
    return status;
  auto surface = mapper.get_surface();
#else
  surface::Surface surface(&batch->surfaceList[0]);
#endif
  if (surface.width() != static_cast<unsigned>(region.width) ||
      surface.height() != static_cast<unsigned>(region.height) || surface.get()->colorFormat != NVBUF_COLOR_FORMAT_RGBA)
    return absl::DataLossError("Camera surface differs from negotiated RGBA crop");
  input = {
      surface.dataptr(),
      nullptr,
      surface.pitch(),
      0,
      region.width,
      region.height,
      gpu::PixelFormat::kRgba8,
      0,
      0,
      0,
      0,
      0,
      0,
      0};
#endif
  status = GpuStatus(gpu::ConvertToRgba16Float(input, const_cast<void*>(lease->plane0), lease->pitch0, impl_->stream));
  // Fence even a failed submission before releasing imports or reusing a slot.
  const auto completion = GpuStatus(cudaStreamSynchronize(impl_->stream));
  if (!status.ok())
    return status;
  if (!completion.ok())
    return completion;
#ifdef HSTREAM_USE_HIP
  status = (*imported)->Complete();
  if (!status.ok())
    return status;
#endif
  return lease;
}
} // namespace hm::scoreboard
