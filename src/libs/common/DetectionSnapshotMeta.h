#pragma once

#include <nvdsmeta.h>

#include <vector>

namespace hm::detection_snapshot {

struct Detection {
  float left{0.0F};
  float top{0.0F};
  float width{0.0F};
  float height{0.0F};
  float score{0.0F};
  int class_id{0};
};

struct Snapshot {
  std::vector<Detection> detections;
};

NvDsMetaType meta_type();

// Captures primary detector output while it is still upstream of rink-mask
// filtering and tracking. The snapshot contains metadata only and never maps
// or reads the video surface.
bool add_meta(NvDsBatchMeta* batch_meta, gint primary_component_id) noexcept;

// Records that a batch passed through a pipeline with no primary detector.
// Every frame receives an explicit empty snapshot so downstream telemetry can
// distinguish that configuration from lost snapshot metadata.
bool add_empty_meta(NvDsBatchMeta* batch_meta) noexcept;

const Snapshot* find_meta(const NvDsBatchMeta* batch_meta, const NvDsFrameMeta* frame_meta) noexcept;

} // namespace hm::detection_snapshot
