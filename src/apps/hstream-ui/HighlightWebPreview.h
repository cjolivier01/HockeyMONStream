#pragma once

#include "hstream/src/apps/apps-common/PreviewTransportMonitor.h"
#include "src/apps/hstream-cli/WebPreviewOptions.h"

#include <memory>
#include <string>

namespace hm::ui {

struct HighlightWebPreviewConfig {
  hm::preview::WebPreviewOptions options;
  std::string run;
  unsigned port{0};
  std::string stun_server;
};

// A presentation adapter for the native authored reel. The reel owns and paces
// video/audio; this target owns only bounded GPU conversion, encoding and peers.
// Stop signaling before the reel graph changes state, then destroy this object
// after the graph reaches NULL. No archive or CPU video path is created here.
class HighlightWebPreview {
 public:
  static std::unique_ptr<HighlightWebPreview> Create(
      const HighlightWebPreviewConfig& config,
      GstElement* graph,
      GstElement* video,
      GstElement* audio,
      std::string* error);
  ~HighlightWebPreview();
  void Stop();
  hm::preview::TransportCounters counters() const;
  bool CheckPresentation(std::string* error);

 private:
  struct Impl;
  HighlightWebPreview();
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::ui
