#include "hstream/src/libs/detection/HipDetector.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#include "hstream/src/libs/detection/DetectionPostprocess.h"

namespace hm::detection {
namespace {
void Hip(hipError_t status) {
  if (status != hipSuccess)
    throw std::runtime_error(hipGetErrorString(status));
}
void Cleanup(hipError_t status) noexcept {
  if (status != hipSuccess)
    std::fprintf(stderr, "AMD detector cleanup: %s\n", hipGetErrorString(status));
}
class DeviceScope {
 public:
  explicit DeviceScope(int device) {
    Hip(hipGetDevice(&previous_));
    Hip(hipSetDevice(device));
  }
  ~DeviceScope() {
    Cleanup(hipSetDevice(previous_));
  }

 private:
  int previous_;
};
} // namespace
struct HipDetector::Impl {
  DetectorConfig config;
  std::unique_ptr<gpu::MigraphxSession> session;
  hipStream_t stream{nullptr};
  bool retired{false};
  uint32_t rows{0};
  uint32_t* device_count{nullptr};
  uint32_t* host_count{nullptr};
  Proposal* device_proposals{nullptr};
  Proposal* host_proposals{nullptr};
  explicit Impl(DetectorConfig config) : config(std::move(config)) {}
  ~Impl() {
    try {
      DeviceScope scope(config.device);
      if (stream)
        Cleanup(hipStreamSynchronize(stream));
      session.reset();
      if (device_count)
        Cleanup(hipFree(device_count));
      if (host_count)
        Cleanup(hipHostFree(host_count));
      if (device_proposals)
        Cleanup(hipFree(device_proposals));
      if (host_proposals)
        Cleanup(hipHostFree(host_proposals));
    } catch (const std::exception& error) {
      std::fprintf(stderr, "AMD detector cleanup: %s\n", error.what());
    }
  }
};
HipDetector::HipDetector(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HipDetector::~HipDetector() = default;

absl::StatusOr<std::unique_ptr<HipDetector>> HipDetector::Create(const DetectorConfig& config) {
  if (config.width <= 0 || config.height <= 0 || config.classes <= 0 || config.maximum_detections <= 0 ||
      !std::isfinite(config.scale) || config.scale <= 0 || !std::isfinite(config.confidence_threshold) ||
      config.confidence_threshold < 0 || config.confidence_threshold > 1 || !std::isfinite(config.iou_threshold) ||
      config.iou_threshold < 0 || config.iou_threshold > 1)
    return absl::InvalidArgumentError("Invalid AMD detector configuration");
  auto session = gpu::MigraphxSession::Compile(
      config.onnx,
      {{config.input_name, {1, 3, static_cast<size_t>(config.height), static_cast<size_t>(config.width)}}},
      config.precision,
      config.device,
      config.maximum_io_bytes);
  if (!session.ok())
    return session.status();
  if ((*session)->output_count() != 1)
    return absl::InvalidArgumentError("Detector must have one [1,N,6] output");
  const auto& shape = (*session)->output(0).dimensions;
  if (shape.size() != 3 || shape[0] != 1 || shape[2] != 6 || !shape[1] ||
      shape[1] > std::numeric_limits<uint32_t>::max() - 255 ||
      shape[1] > config.maximum_io_bytes / (2 * sizeof(Proposal)))
    return absl::InvalidArgumentError("Detector output is not bounded [1,N,6]");
  try {
    DeviceScope scope(config.device);
    auto impl = std::make_unique<Impl>(config);
    impl->rows = shape[1];
    impl->session = std::move(*session);
    Hip(hipMalloc(reinterpret_cast<void**>(&impl->device_count), sizeof(uint32_t)));
    Hip(hipHostMalloc(reinterpret_cast<void**>(&impl->host_count), sizeof(uint32_t)));
    Hip(hipMalloc(reinterpret_cast<void**>(&impl->device_proposals), impl->rows * sizeof(Proposal)));
    Hip(hipHostMalloc(reinterpret_cast<void**>(&impl->host_proposals), impl->rows * sizeof(Proposal)));
    return std::unique_ptr<HipDetector>(new HipDetector(std::move(impl)));
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("AMD detector preparation: ") + error.what());
  }
}

absl::StatusOr<std::vector<Proposal>> HipDetector::Detect(const Image& image, hipStream_t stream) {
  auto& impl = *impl_;
  const auto& config = impl.config;
  if (impl.retired)
    return absl::FailedPreconditionError("AMD detector retired after GPU failure");
  if (!stream || stream == hipStreamLegacy || stream == hipStreamPerThread || (impl.stream && impl.stream != stream))
    return absl::InvalidArgumentError("AMD detector requires one explicit caller stream");
  const auto box = MakeLetterbox(
      image.width,
      image.height,
      config.width,
      config.height,
      config.maintain_aspect,
      config.symmetric_padding,
      config.scale);
  if (!box.ok())
    return box.status();
  try {
    DeviceScope scope(config.device);
    hipDevice_t stream_device;
    Hip(hipStreamGetDevice(stream, &stream_device));
    if (stream_device != config.device)
      return absl::InvalidArgumentError("Detector stream belongs to another device");
    impl.stream = stream;
    Hip(PreprocessDetector(image, *box, impl.session->input(0).data, stream));
    auto status = impl.session->Enqueue(stream);
    if (!status.ok()) {
      impl.retired = true;
      return status;
    }
    Hip(FilterDetectorProposals(
        impl.session->output(0).data,
        impl.rows,
        config.classes,
        config.confidence_threshold,
        config.width,
        config.height,
        impl.device_proposals,
        impl.device_count,
        stream));
    Hip(hipMemcpyAsync(impl.host_count, impl.device_count, sizeof(uint32_t), hipMemcpyDeviceToHost, stream));
    Hip(hipStreamSynchronize(stream));
    if (*impl.host_count > impl.rows)
      throw std::runtime_error("Invalid compact proposal count");
    if (*impl.host_count) {
      Hip(hipMemcpyAsync(
          impl.host_proposals,
          impl.device_proposals,
          *impl.host_count * sizeof(Proposal),
          hipMemcpyDeviceToHost,
          stream));
      Hip(hipStreamSynchronize(stream));
    }
    return SuppressProposals(
        {impl.host_proposals, impl.host_proposals + *impl.host_count},
        config.iou_threshold,
        config.maximum_detections,
        *box,
        image.width,
        image.height);
  } catch (const std::exception& error) {
    impl.retired = true;
    return absl::InternalError(std::string("AMD detector: ") + error.what());
  }
}
} // namespace hm::detection
