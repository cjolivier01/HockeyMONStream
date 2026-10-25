#include "hstream/src/gst-plugins/gst-amd-program/ProgramElement.h"
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/program/VaProgramOutput.h"

namespace hm::rink_map {
namespace {
namespace a = draw_display::analytics;
class Overlay final : public program::gst::Component {
 public:
  explicit Overlay(const Configuration& config) : config_(config), map_(config.map, config.settings.options) {}
  ~Overlay() override {
    if (stream_) {
      hipSetDevice(config_.device);
      hipStreamSynchronize(stream_);
      compositor_.reset();
      hipStreamDestroy(stream_);
    }
  }
  absl::Status Reset() override {
    map_.Reset();
    last_reason_.clear();
    return absl::OkStatus();
  }
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    if (!config_.settings.enabled)
      return input;
    const GstVideoInfo* info = nullptr;
    const auto* frame = FindFrame(input, &info);
    if (!frame)
      return input; // This output deliberately requests clean video.
    commands_.Clear();
    const char* reason = "Missing rink-map frame metadata";
    if (frame && map_.Append(*frame, info->width, info->height, &commands_))
      reason = nullptr;
    else if (frame)
      reason = map_.unavailableReason();
    if (reason) {
      if (last_reason_ != reason)
        g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=%s\n", reason);
      last_reason_ = reason;
      return input;
    }
    last_reason_.clear();
    if (hipSetDevice(config_.device) != hipSuccess ||
        (!stream_ && hipStreamCreateWithFlags(&stream_, hipStreamNonBlocking) != hipSuccess))
      return absl::InternalError("Cannot initialize rink-map HIP stream");
    auto surface = gpu::VaHipFrame::Import(input, stream_, gpu::VaHipFrame::Access::kWrite);
    if (!surface.ok())
      return surface.status();
    auto yuv = playcropper::DescribeVaProgramOutput(**surface, *info);
    if (!yuv.ok())
      return yuv.status();
    if (yuv->format != gpu::PixelFormat::kP010)
      return absl::InvalidArgumentError("Rink map requires P010 VA output");
    a::ImageView image{yuv->y, yuv->y_pitch, unsigned(yuv->width), unsigned(yuv->height), a::PixelFormat::kP010};
    image.chroma = yuv->uv;
    image.chroma_pitch = yuv->uv_pitch;
    image.explicit_yuv_encoding = true;
    image.yuv_encoding = yuv->color;
    if (!compositor_)
      compositor_ = std::make_unique<a::Compositor>();
    const auto result = compositor_->Render(image, commands_, stream_);
    const auto complete = (*surface)->Complete();
    if (!complete.ok())
      return complete;
    if (result.status == a::RenderStatus::kCudaError || result.status == a::RenderStatus::kInvalidArgument)
      return absl::InternalError(std::string("Rink map GPU render failed: ") + a::ToString(result.status));
    if (result.status != a::RenderStatus::kOk)
      g_printerr("HSTREAM_OUTPUT_RINK_MAP status=suppressed reason=%s\n", a::ToString(result.status));
    return input;
  }

 private:
  Configuration config_;
  draw_display::RinkMiniMap map_;
  a::CommandList commands_;
  std::string last_reason_;
  hipStream_t stream_{};
  std::unique_ptr<a::Compositor> compositor_;
};
} // namespace
absl::StatusOr<GstElement*> Create(const Configuration& config) {
  return program::gst::CreateElement("hmrinkmap", nullptr, std::make_unique<Overlay>(config));
}
} // namespace hm::rink_map
