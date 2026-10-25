#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"

namespace hm::rink_map {
absl::StatusOr<Settings> ResolveSettings(const YAML::Node& config) {
  try {
    const auto node = config["output"]["rink-map"];
    const auto enabled = node["enable"].as<bool>();
    const auto size = node["size"].as<std::string>();
    const auto position = node["position"].as<std::string>();
    const auto frames = node["pool-frames"].as<unsigned>();
    if ((size != "icon" && size != "compact" && size != "standard" && size != "large") ||
        (position != "upper-left" && position != "upper-right" && position != "lower-left") || frames < 2 ||
        frames > 32)
      return absl::InvalidArgumentError("Invalid output.rink-map size, position or pool-frames (2..32)");
    return Settings{enabled, draw_display::RinkMiniMapPreferences(size, position), frames};
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(std::string("Missing or invalid output.rink-map settings: ") + e.what());
  }
}
namespace {
struct Payload {
  draw_display::RinkMapFrame frame;
  GstVideoInfo info;
};
struct Meta {
  GstMeta base;
  std::shared_ptr<const Payload>* payload;
};
GType Type() {
  static const GType type = [] {
    static const gchar* tags[] = {nullptr};
    return gst_meta_api_type_register("HmRinkMapFrameAPI", tags);
  }();
  return type;
}
const GstMetaInfo* Info() {
  static const GstMetaInfo* info = gst_meta_register(
      Type(),
      "HmRinkMapFrame",
      sizeof(Meta),
      [](GstMeta* m, gpointer, GstBuffer*) -> gboolean {
        try {
          reinterpret_cast<Meta*>(m)->payload = new std::shared_ptr<const Payload>;
          return TRUE;
        } catch (...) {
          return FALSE;
        }
      },
      [](GstMeta* m, GstBuffer*) { delete reinterpret_cast<Meta*>(m)->payload; },
      [](GstBuffer* dest, GstMeta* m, GstBuffer*, GQuark type, gpointer data) -> gboolean {
        if (!GST_META_TRANSFORM_IS_COPY(type) || static_cast<GstMetaTransformCopy*>(data)->region)
          return FALSE;
        auto* copy = reinterpret_cast<Meta*>(gst_buffer_add_meta(dest, Info(), nullptr));
        if (!copy)
          return FALSE;
        *copy->payload = *reinterpret_cast<Meta*>(m)->payload;
        return TRUE;
      });
  return info;
}
} // namespace
bool AttachFrame(GstBuffer* buffer, const draw_display::RinkMapFrame& frame, const GstVideoInfo& info) {
  if (!buffer || !gst_buffer_is_writable(buffer) || gst_buffer_get_meta(buffer, Type()))
    return false;
  try {
    auto value = std::make_shared<const Payload>(Payload{frame, info});
    auto* meta = reinterpret_cast<Meta*>(gst_buffer_add_meta(buffer, Info(), nullptr));
    if (!meta)
      return false;
    *meta->payload = std::move(value);
    return true;
  } catch (...) {
    return false;
  }
}
const draw_display::RinkMapFrame* FindFrame(GstBuffer* buffer, const GstVideoInfo** info) {
  const auto* meta = reinterpret_cast<const Meta*>(gst_buffer_get_meta(buffer, Type()));
  if (!meta || !meta->payload || !*meta->payload)
    return nullptr;
  if (info)
    *info = &(*meta->payload)->info;
  return &(*meta->payload)->frame;
}
} // namespace hm::rink_map
