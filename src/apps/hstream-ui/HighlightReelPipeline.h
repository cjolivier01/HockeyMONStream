#pragma once
#include <memory>
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"

namespace hm::ui {
// One decoder at a time, one continuous output encoder/mux, and bounded NVMM
// pools. Preview and export share source timing and all authored GPU rendering.
class HighlightReelPipeline {
 public:
  struct Request {
    QString archive_path, asset_root, output_path;
    QVector<HighlightInterval> items;
    qint64 archive_offset_ms{0};
    ArchiveMediaInfo media;
    quint64 window_id{0}; // nonzero for preview; output_path for export
    bool loop{false};
    qint64 inspect_game_ms{-1}; // hold one authored frame for positioning
  };
  struct Status {
    bool finished{false}, cancelled{false};
    QString error;
    qint64 position_ms{0}, total_ms{0};
    int item{-1};
  };
  HighlightReelPipeline();
  ~HighlightReelPipeline();
  bool Start(Request request, QString* error);
  void Cancel(); // nonblocking; Poll reports completion after cleanup
  Status Poll() const;
  static bool PreviewAvailable();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::ui
