#include "src/apps/hstream-ui/CameraExperimentPreviewOutput.h"

#include "hstream/src/apps/apps-common/HmGpuPreview.h"

#include <QtGui/QImage>

namespace {
class NativeOutput final : public CameraExperimentPreviewOutput {
 public:
  GstElement* CreateSink(const Settings& settings, std::string* error) override {
    if (!settings.window_id || !hm::gpu_preview::renderer_available() || !hm::gpu_preview::register_elements()) {
      if (error)
        *error = "Camera experiment video preview requires NVIDIA graphics and an X11 display.";
      return nullptr;
    }
    sink_ = gst_element_factory_make("hmgpupreviewsink", "experiment-renderer");
    if (!sink_)
      return nullptr;
    g_object_set(
        sink_,
        "window-id",
        settings.window_id,
        "channel",
        "program",
        "sync",
        settings.realtime ? TRUE : FALSE,
        "max-lateness",
        gint64{-1},
        "async",
        settings.asynchronous ? TRUE : FALSE,
        "qos",
        FALSE,
        "enable-last-sample",
        TRUE,
        "show-player-tracking",
        settings.players,
        "show-play-tracking",
        settings.play,
        "show-rink-mask",
        settings.rink,
        nullptr);
    hm::gpu_preview::configure_recorded_rink_mask(sink_, settings.rink_mask);
    hm::gpu_preview::set_source_geometry(sink_, 1280, 720);
    return sink_;
  }
  void SetGeneration(std::uint64_t generation) override {
    generation_ = generation;
    hm::gpu_preview::set_renderer_generation(sink_, generation);
  }
  bool Presented(std::uint64_t* generation, GstClockTime* source_pts) override {
    return sink_ && hm::gpu_preview::presented_frame(sink_, generation, source_pts);
  }
  std::uint64_t PresentationOperation() const override {
    return generation_;
  }
  void SetOverlays(bool players, bool play, bool rink) override {
    if (sink_)
      g_object_set(sink_, "show-player-tracking", players, "show-play-tracking", play, "show-rink-mask", rink, nullptr);
  }
  void Redraw() override {
    if (sink_)
      hm::gpu_preview::redraw_last_sample(sink_);
  }
  bool Capture(const std::string& path, std::string* error) override {
    std::vector<std::uint8_t> rgba;
    unsigned width = 0, height = 0;
    if (!sink_ || !hm::gpu_preview::capture_presented_frame(sink_, &rgba, &width, &height, error))
      return false;
    // Explicit operator/test screenshot only; playback never reads pixels back.
    QImage image(rgba.data(), width, height, width * 4, QImage::Format_RGBA8888);
    if (image.save(QString::fromStdString(path)))
      return true;
    if (error)
      *error = "Could not save the preview screenshot.";
    return false;
  }
  void ReleaseSink() override {
    if (sink_)
      hm::gpu_preview::quiesce(sink_, generation_);
    sink_ = nullptr;
  }

 private:
  GstElement* sink_{nullptr};
  std::uint64_t generation_{0};
};
} // namespace

std::shared_ptr<CameraExperimentPreviewOutput> MakeNativeCameraExperimentPreviewOutput() {
  return std::make_shared<NativeOutput>();
}
