#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>

#include "hstream/src/libs/stitching/RinkLandmarks.h"
#include "hstream/src/libs/stitching/RinkMap.h"

struct RinkMapSnapshot {
  QString game_directory;
  std::shared_ptr<QTemporaryDir> temporary;
  QByteArray revision;
  // Read under the calibration/config locks; also fences competing editors.
  std::optional<std::string> saved_map;
  std::string generation;
  std::array<QImage, 2> images;
  std::array<QSize, 2> original_sizes;
};

struct RinkMapWorkResult {
  hm::stitching::RinkMapDocument document;
  hm::stitching::RinkMapFit fit;
  std::array<std::vector<hm::stitching::RinkLandmarkDetection>, 2> detections;
  std::array<std::vector<std::vector<cv::Point2d>>, 2> projected_layout;
};

absl::StatusOr<RinkMapSnapshot> LoadRinkMapSnapshot(const QString& game_directory);
absl::Status SaveRinkMap(RinkMapSnapshot& snapshot, const hm::stitching::RinkMapDocument& document);
absl::StatusOr<RinkMapWorkResult> UpdateRinkMap(
    const RinkMapSnapshot& snapshot,
    hm::stitching::RinkMapDocument document,
    bool detect,
    const std::function<bool()>& cancelled);
