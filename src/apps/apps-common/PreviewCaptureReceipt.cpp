#include "PreviewCaptureReceipt.h"

#include <gstnvdsmeta.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace hm::preview {
namespace {
void text(JsonBuilder* b, const char* key, const std::string& value) {
  json_builder_set_member_name(b, key);
  json_builder_add_string_value(b, value.c_str());
}
void decimal(JsonBuilder* b, const char* key, uint64_t value) {
  text(b, key, std::to_string(value));
}
void clock(JsonBuilder* b, const char* key, GstClockTime value) {
  if (GST_CLOCK_TIME_IS_VALID(value))
    decimal(b, key, value);
  else {
    json_builder_set_member_name(b, key);
    json_builder_add_null_value(b);
  }
}
void number(JsonBuilder* b, const char* key, double value) {
  json_builder_set_member_name(b, key);
  if (std::isfinite(value))
    json_builder_add_double_value(b, value);
  else
    json_builder_add_null_value(b);
}
void flag(JsonBuilder* b, const char* key, bool value) {
  json_builder_set_member_name(b, key);
  json_builder_add_boolean_value(b, value);
}
bool physical(const DecodedFrameSequence& value) {
  const char* uri = value.source_uri ? g_quark_to_string(value.source_uri) : nullptr;
  return uri && std::strlen(uri) <= 4096 && g_str_has_prefix(uri, "file://") &&
      GST_CLOCK_TIME_IS_VALID(value.source_pts);
}
void camera(JsonBuilder* b, const DecodedFrameSequence& value, unsigned width, unsigned height) {
  json_builder_begin_object(b);
  text(b, "uri", value.source_uri ? g_quark_to_string(value.source_uri) : "");
  clock(b, "ptsNs", value.source_pts);
  decimal(b, "sequence", value.sequence);
  decimal(b, "sourceId", value.source_id);
  decimal(b, "width", width);
  decimal(b, "height", height);
  json_builder_end_object(b);
}
JsonNode* member(JsonObject* object, const char* key, GType type) {
  auto* node = json_object_get_member(object, key);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != type)
    throw std::invalid_argument(std::string("Invalid capture plan field: ") + key);
  return node;
}
std::string string(JsonObject* object, const char* key, size_t maximum) {
  const char* value = json_node_get_string(member(object, key, G_TYPE_STRING));
  if (!value || !*value || std::strlen(value) > maximum)
    throw std::invalid_argument(std::string("Unbounded capture plan field: ") + key);
  return value;
}
uint64_t pts(JsonObject* object, const char* key) {
  const auto value = string(object, key, 20);
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !GST_CLOCK_TIME_IS_VALID(result))
    throw std::invalid_argument("Capture PTS must be an exact unsigned decimal string");
  return result;
}
} // namespace

CaptureMetadata ReadCaptureMetadata(GstBuffer* buffer) {
  CaptureMetadata result;
  auto* batch = buffer ? gst_buffer_get_nvds_batch_meta(buffer) : nullptr;
  if (!batch || !batch->frame_meta_list || batch->frame_meta_list->next)
    return result;
  const auto* frame = static_cast<const NvDsFrameMeta*>(batch->frame_meta_list->data);
  result.pair = stitching_frame_pair(frame);
  if (const auto* crop = preview_overlay::find_playcropper_transform_meta(frame))
    result.crop = *crop;
  return result;
}

std::string CaptureReceipt(
    const CaptureContext& c,
    const CaptureMetadata& m,
    const std::string& request,
    unsigned occurrence) {
  auto* b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, 1);
  text(b, "interface", c.interface);
  text(b, "run", c.run);
  json_builder_set_member_name(b, "stage");
  json_builder_add_int_value(b, c.stage);
  decimal(b, "topology", c.topology);
  decimal(b, "operation", c.operation);
  decimal(b, "sequence", c.sequence);
  text(b, "channel", c.channel);
  clock(b, "sourcePts", c.source_pts);
  clock(b, "presentationPts", c.presentation_pts);
  decimal(b, "rendererGeneration", c.renderer_generation);
  text(b, "path", c.path);
  text(b, "request", request);
  decimal(b, "occurrence", occurrence);
  flag(b, "physicalIdentityValid", m.pair && physical(m.pair->left) && physical(m.pair->right));
  json_builder_set_member_name(b, "physicalSources");
  json_builder_begin_array(b);
  if (m.pair) {
    camera(b, m.pair->left, m.pair->left_width, m.pair->left_height);
    camera(b, m.pair->right, m.pair->right_width, m.pair->right_height);
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "crop");
  if (m.crop) {
    const auto& t = *m.crop;
    json_builder_begin_object(b);
    number(b, "inputWidth", t.input_width);
    number(b, "inputHeight", t.input_height);
    number(b, "metadataWidth", t.metadata_width);
    number(b, "metadataHeight", t.metadata_height);
    number(b, "sourceLeft", t.source_left);
    number(b, "sourceTop", t.source_top);
    number(b, "anchorX", t.anchor_x);
    number(b, "anchorY", t.anchor_y);
    number(b, "left", t.crop_left);
    number(b, "top", t.crop_top);
    number(b, "width", t.crop_width);
    number(b, "height", t.crop_height);
    number(b, "outputWidth", t.output_width);
    number(b, "outputHeight", t.output_height);
    number(b, "angleDegrees", t.angle_degrees);
    number(b, "fixedEdgeRotationLeft", t.fixed_edge_rotation_left);
    number(b, "fixedEdgeRotationRight", t.fixed_edge_rotation_right);
    flag(b, "rotationControlsValid", t.rotation_controls_valid);
    flag(b, "objectMetaTransformed", t.object_meta_transformed);
    decimal(b, "bakedPlayerLayers", t.baked_player_layers);
    json_builder_end_object(b);
  } else
    json_builder_add_null_value(b);
  json_builder_end_object(b);
  auto* node = json_builder_get_root(b);
  auto* generator = json_generator_new();
  json_generator_set_root(generator, node);
  gchar* data = json_generator_to_data(generator, nullptr);
  std::string result(data);
  g_free(data);
  g_object_unref(generator);
  json_node_free(node);
  g_object_unref(b);
  return result;
}

CapturePlan::CapturePlan(const char* json) {
  if (!json || !*json)
    return;
  if (std::strlen(json) > 32768)
    throw std::invalid_argument("Capture plan exceeds 32KiB");
  std::unique_ptr<JsonParser, void (*)(JsonParser*)> parser(
      json_parser_new(), [](JsonParser* p) { g_object_unref(p); });
  if (!json_parser_load_from_data(parser.get(), json, -1, nullptr))
    throw std::invalid_argument("Invalid capture plan JSON");
  auto* root = json_parser_get_root(parser.get());
  if (!JSON_NODE_HOLDS_OBJECT(root))
    throw std::invalid_argument("Capture plan must be an object");
  auto* object = json_node_get_object(root);
  if (json_object_get_size(object) != 2 || json_node_get_int(member(object, "version", G_TYPE_INT64)) != 1)
    throw std::invalid_argument("Unsupported capture plan version/fields");
  auto* list_node = json_object_get_member(object, "requests");
  if (!list_node || !JSON_NODE_HOLDS_ARRAY(list_node))
    throw std::invalid_argument("Capture requests must be an array");
  auto* list = json_node_get_array(list_node);
  if (!json_array_get_length(list) || json_array_get_length(list) > 8)
    throw std::invalid_argument("Capture plan requires 1..8 requests");
  for (guint i = 0; i < json_array_get_length(list); ++i) {
    auto* entry_node = json_array_get_element(list, i);
    if (!JSON_NODE_HOLDS_OBJECT(entry_node))
      throw std::invalid_argument("Capture request must be an object");
    auto* entry = json_node_get_object(entry_node);
    if (json_object_get_size(entry) != 7)
      throw std::invalid_argument("Unexpected capture request fields");
    Request request;
    request.id = string(entry, "id", 64);
    request.channel = string(entry, "channel", 16);
    if ((request.channel != "program" && request.channel != "stitched") ||
        !std::all_of(
            request.id.begin(),
            request.id.end(),
            [](unsigned char c) { return g_ascii_isalnum(c) || c == '-' || c == '_'; }) ||
        std::any_of(requests_.begin(), requests_.end(), [&](const auto& r) { return r.id == request.id; }))
      throw std::invalid_argument("Invalid capture channel/id");
    const auto left = string(entry, "leftUri", 4096), right = string(entry, "rightUri", 4096);
    if (!g_str_has_prefix(left.c_str(), "file://") || !g_str_has_prefix(right.c_str(), "file://"))
      throw std::invalid_argument("Capture plan requires physical file URIs");
    request.left_uri = g_quark_from_string(left.c_str());
    request.right_uri = g_quark_from_string(right.c_str());
    request.left_pts = pts(entry, "leftPtsNs");
    request.right_pts = pts(entry, "rightPtsNs");
    const auto count = json_node_get_int(member(entry, "count", G_TYPE_INT64));
    if (count < 1 || count > 3)
      throw std::invalid_argument("Capture count must be 1..3");
    request.count = static_cast<unsigned>(count);
    remaining_.fetch_add(request.count, std::memory_order_relaxed);
    requests_.push_back(std::move(request));
  }
}

std::optional<std::string> CapturePlan::Capture(const CaptureContext& c, const CaptureMetadata& m) {
  if (!pending() || !m.pair || !physical(m.pair->left) || !physical(m.pair->right) ||
      (c.channel == "program" && !m.crop))
    return {};
  std::lock_guard<std::mutex> lock(mutex_);
  const auto key =
      c.run + ":" + std::to_string(c.topology) + ":" + std::to_string(c.operation) + ":" + std::to_string(c.sequence);
  for (auto& r : requests_) {
    if (r.captured >= r.count || r.channel != c.channel ||
        std::find(r.identities.begin(), r.identities.end(), key) != r.identities.end() ||
        r.left_uri != m.pair->left.source_uri || r.right_uri != m.pair->right.source_uri ||
        r.left_pts != m.pair->left.source_pts || r.right_pts != m.pair->right.source_pts)
      continue;
    auto result = CaptureReceipt(c, m, r.id, r.captured + 1);
    r.identities.push_back(key);
    ++r.captured;
    remaining_.fetch_sub(1, std::memory_order_relaxed);
    return result;
  }
  return {};
}
CapturePlan& ProcessCapturePlan() {
  static CapturePlan plan(g_getenv("HSTREAM_PREVIEW_CAPTURE_PLAN"));
  return plan;
}
} // namespace hm::preview
