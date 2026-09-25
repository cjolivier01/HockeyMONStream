#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <yaml-cpp/yaml.h>

#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "hstream/src/gst-plugins/gst-player-analytics/PlayerAnalyticsProcessor.h"

namespace {
namespace pa = hm::player_analytics;

struct GstPlayerAnalytics {
  GstBaseTransform parent;
  gchar* configuration;
  gint gpu_id;
  pa::PlayerAnalyticsProcessor* processor;
  gboolean running;
  gboolean failed;
};
struct GstPlayerAnalyticsClass {
  GstBaseTransformClass parent;
};

G_DEFINE_TYPE(GstPlayerAnalytics, gst_player_analytics, GST_TYPE_BASE_TRANSFORM)

enum { kPropertyZero, kPropertyConfiguration, kPropertyGpuId };

GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:NVMM), format=(string){ RGBA, RGB10A2_LE }"));
GstStaticPadTemplate source_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:NVMM), format=(string){ RGBA, RGB10A2_LE }"));

void Report(GstPlayerAnalytics* self, const char* message) noexcept {
  GST_ELEMENT_ERROR(self, STREAM, FAILED, ("Player analytics failed: %s", message), (nullptr));
}

gboolean Start(GstBaseTransform* transform) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(transform);
  GST_OBJECT_LOCK(self);
  self->running = TRUE;
  self->failed = FALSE;
  if (self->configuration && std::strlen(self->configuration) > 65536) {
    self->running = FALSE;
    GST_OBJECT_UNLOCK(self);
    Report(self, "configuration exceeds 64 KiB");
    return FALSE;
  }
  gchar* copied = g_strdup(self->configuration ? self->configuration : "");
  const gint gpu_id = self->gpu_id;
  GST_OBJECT_UNLOCK(self);
  std::unique_ptr<gchar, decltype(&g_free)> serialized(copied, &g_free);
  absl::Status failure;
  try {
    auto config = pa::ParseConfig(!*copied ? YAML::Node() : YAML::Load(copied));
    if (!config.ok()) {
      failure = config.status();
    } else {
      auto processor = pa::PlayerAnalyticsProcessor::Create(*config, gpu_id);
      if (!processor.ok()) {
        failure = processor.status();
      } else {
        GST_OBJECT_LOCK(self);
        auto* previous = self->processor;
        self->processor = processor->release();
        GST_OBJECT_UNLOCK(self);
        delete previous;
        return TRUE;
      }
    }
  } catch (const std::exception& error) {
    failure = absl::InternalError(error.what());
  } catch (...) {
    failure = absl::InternalError("unknown startup failure");
  }
  GST_OBJECT_LOCK(self);
  self->running = FALSE;
  GST_OBJECT_UNLOCK(self);
  Report(self, failure.ToString().c_str());
  return FALSE;
}

gboolean Stop(GstBaseTransform* transform) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(transform);
  GST_OBJECT_LOCK(self);
  auto* processor = self->processor;
  self->processor = nullptr;
  self->running = FALSE;
  GST_OBJECT_UNLOCK(self);
  // GstBaseTransform stops its streaming task before invoking stop. Detach
  // under the object lock so an overlapping FLUSH_START cannot use a freed owner.
  if (processor) {
    const auto counters = processor->counters();
    g_print(
        "HSTREAM_PLAYER_ANALYTICS frames=%" G_GUINT64_FORMAT " pose-enqueues=%" G_GUINT64_FORMAT
        " pose-results=%" G_GUINT64_FORMAT " invalid-time=%" G_GUINT64_FORMAT " invalid-rois=%" G_GUINT64_FORMAT
        " capacity-excluded=%" G_GUINT64_FORMAT " duplicate-time=%" G_GUINT64_FORMAT
        " cancelled-batches=%" G_GUINT64_FORMAT " jersey-enqueues=%" G_GUINT64_FORMAT
        " jersey-results=%" G_GUINT64_FORMAT " action-enqueues=%" G_GUINT64_FORMAT " action-results=%" G_GUINT64_FORMAT
        " model-samples=%" G_GUINT64_FORMAT " maximum-frame-samples=%" G_GUINT64_FORMAT
        " budget-deferred=%" G_GUINT64_FORMAT " jersey-pose-skipped=%" G_GUINT64_FORMAT
        " jersey-visibility-skipped=%" G_GUINT64_FORMAT " action-history-unready=%" G_GUINT64_FORMAT
        " action-history-resets=%" G_GUINT64_FORMAT "\n",
        counters.frames,
        counters.pose_enqueues,
        counters.pose_results,
        counters.invalid_time,
        counters.invalid_rois,
        counters.capacity_excluded,
        counters.duplicate_time,
        counters.cancelled_batches,
        counters.jersey_enqueues,
        counters.jersey_results,
        counters.action_enqueues,
        counters.action_results,
        counters.model_samples,
        counters.maximum_frame_samples,
        counters.budget_deferred,
        counters.jersey_pose_skipped,
        counters.jersey_visibility_skipped,
        counters.action_history_unready,
        counters.action_history_resets);
    delete processor;
  }
  return TRUE;
}

GstFlowReturn Transform(GstBaseTransform* transform, GstBuffer* buffer) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(transform);
  if (self->failed)
    return GST_FLOW_ERROR;
  if (!self->processor)
    return GST_FLOW_OK;
  if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
    self->processor->Reset();
  // NVMM GstBuffer mapping exposes only the CPU NvBufSurface descriptor. Its
  // dataPtr/EGL image stays on the GPU; no video plane is CPU-mapped/read back.
  struct Mapping {
    GstBuffer* buffer;
    GstMapInfo info{};
    bool mapped{false};
    ~Mapping() {
      if (mapped)
        gst_buffer_unmap(buffer, &info);
    }
  } mapping{buffer};
  try {
    mapping.mapped = gst_buffer_map(buffer, &mapping.info, GST_MAP_READ);
    if (!mapping.mapped || mapping.info.size < sizeof(NvBufSurface)) {
      self->failed = TRUE;
      Report(self, "input lacks its NVMM surface descriptor");
      return GST_FLOW_ERROR;
    }
    auto status = self->processor->Process(
        reinterpret_cast<NvBufSurface*>(mapping.info.data), gst_buffer_get_nvds_batch_meta(buffer));
    if (!status.ok()) {
      if (absl::IsCancelled(status))
        return GST_FLOW_FLUSHING;
      self->failed = TRUE;
      Report(self, status.ToString().c_str());
      return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
  } catch (const std::exception& error) {
    self->failed = TRUE;
    Report(self, error.what());
  } catch (...) {
    self->failed = TRUE;
    Report(self, "unknown processing failure");
  }
  return GST_FLOW_ERROR;
}

gboolean SinkEvent(GstBaseTransform* transform, GstEvent* event) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(transform);
  // FLUSH_START is not serialized; it may overlap borrowed-surface processing.
  // FLUSH_STOP and the other events below execute after old work is retired.
  // Ordinary continuous SEGMENT events intentionally preserve cadence/history.
  GST_OBJECT_LOCK(self);
  if (self->processor && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START)
    self->processor->CancelPending();
  else if (
      self->processor &&
      (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP || GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START ||
       GST_EVENT_TYPE(event) == GST_EVENT_EOS))
    self->processor->Reset(GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP);
  GST_OBJECT_UNLOCK(self);
  return GST_BASE_TRANSFORM_CLASS(gst_player_analytics_parent_class)->sink_event(transform, event);
}

gboolean SetCaps(GstBaseTransform* transform, GstCaps* input, GstCaps*) noexcept {
  GstVideoInfo info{};
  const char* format =
      gst_caps_get_size(input) == 1 ? gst_structure_get_string(gst_caps_get_structure(input, 0), "format") : nullptr;
  if (!gst_caps_is_fixed(input) || gst_caps_get_size(input) != 1 || !format ||
      (std::strcmp(format, "RGBA") != 0 && std::strcmp(format, "RGB10A2_LE") != 0) ||
      !gst_caps_features_contains(gst_caps_get_features(input, 0), "memory:NVMM") ||
      !gst_video_info_from_caps(&info, input) || GST_VIDEO_INFO_WIDTH(&info) < 1 || GST_VIDEO_INFO_HEIGHT(&info) < 1 ||
      GST_VIDEO_INFO_WIDTH(&info) > 65536 || GST_VIDEO_INFO_HEIGHT(&info) > 65536) {
    Report(reinterpret_cast<GstPlayerAnalytics*>(transform), "requires bounded NVMM video dimensions");
    return FALSE;
  }
  return TRUE;
}

void SetProperty(GObject* object, guint id, const GValue* value, GParamSpec* spec) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(object);
  GST_OBJECT_LOCK(self);
  if (GST_STATE(self) > GST_STATE_READY || self->running) {
    GST_OBJECT_UNLOCK(self);
    GST_WARNING_OBJECT(self, "Player analytics configuration applies on the next run");
    return;
  }
  switch (id) {
    case kPropertyConfiguration:
      g_free(self->configuration);
      self->configuration = g_value_dup_string(value);
      break;
    case kPropertyGpuId:
      self->gpu_id = g_value_get_int(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
  }
  GST_OBJECT_UNLOCK(self);
}

void GetProperty(GObject* object, guint id, GValue* value, GParamSpec* spec) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(object);
  GST_OBJECT_LOCK(self);
  switch (id) {
    case kPropertyConfiguration:
      g_value_set_string(value, self->configuration);
      break;
    case kPropertyGpuId:
      g_value_set_int(value, self->gpu_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
  }
  GST_OBJECT_UNLOCK(self);
}

void Finalize(GObject* object) noexcept {
  auto* self = reinterpret_cast<GstPlayerAnalytics*>(object);
  delete self->processor;
  g_free(self->configuration);
  G_OBJECT_CLASS(gst_player_analytics_parent_class)->finalize(object);
}

void gst_player_analytics_class_init(GstPlayerAnalyticsClass* klass) {
  auto* object = G_OBJECT_CLASS(klass);
  object->set_property = SetProperty;
  object->get_property = GetProperty;
  object->finalize = Finalize;
  constexpr auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  g_object_class_install_property(
      object,
      kPropertyConfiguration,
      g_param_spec_string(
          "configuration", "Configuration", "Resolved player analytics YAML; applies before playback", "", flags));
  g_object_class_install_property(
      object,
      kPropertyGpuId,
      g_param_spec_int("gpu-id", "GPU", "CUDA device owning the incoming surfaces", 0, G_MAXINT, 0, flags));
  auto* element = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(
      element,
      "HStream player analytics",
      "Filter/Video",
      "Optional GPU player inference with compact immutable metadata",
      "HStream");
  gst_element_class_add_static_pad_template(element, &sink_template);
  gst_element_class_add_static_pad_template(element, &source_template);
  auto* transform = GST_BASE_TRANSFORM_CLASS(klass);
  transform->start = Start;
  transform->stop = Stop;
  transform->transform_ip = Transform;
  transform->sink_event = SinkEvent;
  transform->set_caps = SetCaps;
  transform->transform_ip_on_passthrough = TRUE;
}

void gst_player_analytics_init(GstPlayerAnalytics* self) {
  self->configuration = g_strdup("");
  self->gpu_id = 0;
  self->processor = nullptr;
  self->running = FALSE;
  self->failed = FALSE;
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), TRUE);
}

gboolean Register(GstPlugin* plugin) {
  return gst_element_register(plugin, "hmplayeranalytics", GST_RANK_NONE, gst_player_analytics_get_type());
}
} // namespace

#ifndef PACKAGE
#define PACKAGE "hstream"
#endif

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    hmplayeranalytics,
    "Optional HStream GPU player analytics",
    Register,
    "1.0",
    "Proprietary",
    "hstream",
    "https://github.com/cjolivier01/HockeyMONStream")
