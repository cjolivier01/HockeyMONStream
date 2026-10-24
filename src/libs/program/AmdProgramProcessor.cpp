#include "hstream/src/libs/program/AmdProgramProcessor.h"
#include "hstream/src/libs/program/AmdVideoStages.h"

namespace hm::program {
struct AmdProgramProcessor::Impl {
  std::unique_ptr<AmdStitcher> stitcher;
  std::unique_ptr<AmdDetector> detector;
  std::unique_ptr<tracking::ProgramTracking> tracking;
  std::unique_ptr<AmdProgramRenderer> renderer;
  std::shared_ptr<const AmdPanoramaFrame> frame;
  hm::BBox crop;
  bool retired{false};
};
AmdProgramProcessor::AmdProgramProcessor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AmdProgramProcessor::~AmdProgramProcessor() = default;
absl::StatusOr<std::unique_ptr<AmdProgramProcessor>> AmdProgramProcessor::Create(
    const AmdProgramProcessorConfig& config) {
  // The synchronous compatibility facade retains exactly one completed frame.
  auto stitcher = AmdStitcher::Create(config, 1);
  if (!stitcher.ok())
    return stitcher.status();
  auto tracking = tracking::ProgramTracking::Create(
      (*stitcher)->rink_mask(), config.camera, config.tracker, config.raise_center_ratio, config.lower_bottom_ratio);
  if (!tracking.ok())
    return tracking.status();
  auto detector = AmdDetector::Create(config.detector);
  if (!detector.ok())
    return detector.status();
  auto renderer = AmdProgramRenderer::Create(config.detector.device);
  if (!renderer.ok())
    return renderer.status();
  auto impl = std::make_unique<Impl>();
  impl->stitcher = std::move(*stitcher);
  impl->tracking = std::move(*tracking);
  impl->detector = std::move(*detector);
  impl->renderer = std::move(*renderer);
  return std::unique_ptr<AmdProgramProcessor>(new AmdProgramProcessor(std::move(impl)));
}
absl::StatusOr<AmdProgramFrame> AmdProgramProcessor::Process(uint64_t sequence, GstSample* left, GstSample* right) {
  auto& impl = *impl_;
  if (impl.retired)
    return absl::FailedPreconditionError("AMD Program retired after processing failure");
  impl.retired = true;
  impl.frame.reset();
  auto frame = impl.stitcher->Process(left, right);
  if (!frame.ok())
    return frame.status();
  auto detected = impl.detector->Process(**frame);
  if (!detected.ok())
    return detected.status();
  auto observed = impl.tracking->Process(sequence, width(), height(), *detected);
  if (!observed.ok())
    return observed.status();
  impl.frame = std::move(*frame);
  impl.crop = observed->crop.value_or(hm::BBox(0, 0, width(), height()));
  impl.retired = false;
  return AmdProgramFrame{detected->size(), std::move(*observed), impl.crop};
}
absl::Status AmdProgramProcessor::Render(
    GstBuffer* output,
    const GstVideoInfo& info,
    const ProgramRenderSettings& settings,
    ProgramPreviewBoxes* preview) {
  auto& impl = *impl_;
  if (impl.retired || !impl.frame)
    return absl::FailedPreconditionError("AMD Program has no completed frame to render");
  const auto status = impl.renderer->Render(*impl.frame, impl.crop, output, info, settings, preview);
  impl.retired = !impl.renderer->healthy();
  return status;
}
absl::Status AmdProgramProcessor::Reset() {
  auto& impl = *impl_;
  impl.frame.reset();
  if (impl.retired)
    return absl::FailedPreconditionError("Reconstruct AMD Program after processing failure");
  const auto status = impl.tracking->Reset();
  impl.retired = !status.ok();
  return status;
}
int AmdProgramProcessor::width() const {
  return impl_->stitcher->width();
}
int AmdProgramProcessor::height() const {
  return impl_->stitcher->height();
}
const std::string& AmdProgramProcessor::generation() const {
  return impl_->stitcher->generation();
}
} // namespace hm::program
