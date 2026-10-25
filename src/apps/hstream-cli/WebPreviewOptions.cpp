#include "WebPreviewOptions.h"

#include <json-glib/json-glib.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <functional>
#include <stdexcept>

namespace hm::preview {
namespace {
bool Token(const std::string& value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
        c == '.' || c == ':';
  });
}
bool Channel(const std::string& value) {
  if (value == "program" || value == "stitched" || value == "none")
    return true;
  if (value.size() < 7 || value.size() > 9 || value.compare(0, 6, "source") != 0 ||
      (value.size() > 7 && value[6] == '0'))
    return false;
  unsigned index;
  const auto parsed = std::from_chars(value.data() + 6, value.data() + value.size(), index);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && index < 128;
}
bool Integer(JsonObject* object, const char* name, gint64* value) {
  auto* node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
    return false;
  *value = json_node_get_int(node);
  return true;
}
bool String(JsonObject* object, const char* name, std::string* value) {
  auto* node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    return false;
  *value = json_node_get_string(node);
  return true;
}
bool Decimal(const std::string& value, uint64_t* output) {
  if (value.empty() || value.size() > 20 || value[0] == '0')
    return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
void Add(JsonBuilder* builder, const char* name, const std::string& value) {
  json_builder_set_member_name(builder, name);
  json_builder_add_string_value(builder, value.c_str());
}
void Add(JsonBuilder* builder, const char* name, gint64 value) {
  json_builder_set_member_name(builder, name);
  json_builder_add_int_value(builder, value);
}
std::string Serialize(const std::function<void(JsonBuilder*)>& fill) {
  auto* builder = json_builder_new();
  json_builder_begin_object(builder);
  fill(builder);
  json_builder_end_object(builder);
  auto* root = json_builder_get_root(builder);
  auto* generator = json_generator_new();
  json_generator_set_root(generator, root);
  auto* value = json_generator_to_data(generator, nullptr);
  std::string result(value);
  g_free(value);
  g_object_unref(generator);
  json_node_free(root);
  g_object_unref(builder);
  return result;
}
void Identity(
    JsonBuilder* builder,
    const RouteOwner::Session& session,
    uint64_t operation,
    const std::string& channel) {
  Add(builder, "version", gint64{1});
  Add(builder, "run", session.run);
  Add(builder, "stage", session.stage);
  Add(builder, "topology", std::to_string(session.topology));
  Add(builder, "operation", std::to_string(operation));
  Add(builder, "channel", channel);
}
} // namespace

absl::StatusOr<PreviewReferenceGeometry> ReadPreviewReferenceGeometry(const YAML::Node& resolved) {
  try {
    const auto node = resolved["preview_reference"];
    if (!node || !node.IsMap())
      return absl::InvalidArgumentError("Missing resolved preview_reference configuration");
    PreviewReferenceGeometry geometry{};
    const auto read = [&](const char* key, unsigned maximum, unsigned* output) {
      if (!node[key] || !node[key].IsScalar())
        throw std::runtime_error(std::string("Missing preview_reference.") + key);
      const auto value = node[key].as<uint64_t>();
      if (value < 2 || value > maximum || value % 2)
        throw std::runtime_error(std::string("Out-of-range preview_reference.") + key);
      *output = static_cast<unsigned>(value);
    };
    read("program_width", 3840, &geometry.program_width);
    read("program_height", 2160, &geometry.program_height);
    read("source_width", 3840, &geometry.source_width);
    read("source_height", 2160, &geometry.source_height);
    return geometry;
  } catch (const std::exception& exception) {
    return absl::InvalidArgumentError(std::string("Invalid preview reference geometry: ") + exception.what());
  }
}

absl::StatusOr<WebPreviewOptions> ReadWebPreviewOptions(const YAML::Node& resolved) {
  try {
    const auto node = resolved["web_preview"];
    if (!node || !node.IsMap())
      return absl::InvalidArgumentError("Missing resolved web_preview configuration");
    WebPreviewOptions options{};
    const auto reference = ReadPreviewReferenceGeometry(resolved);
    if (!reference.ok())
      return reference.status();
    options.program_reference_width = reference->program_width;
    options.program_reference_height = reference->program_height;
    options.source_reference_width = reference->source_width;
    options.source_reference_height = reference->source_height;
    const auto read = [&](const char* key, unsigned low, unsigned high, unsigned* output) {
      if (!node[key] || !node[key].IsScalar())
        throw std::runtime_error(std::string("Missing web_preview.") + key);
      const auto value = node[key].as<uint64_t>();
      if (value < low || value > high)
        throw std::runtime_error(std::string("Out-of-range web_preview.") + key);
      *output = static_cast<unsigned>(value);
    };
#define READ(key, low, high) read(#key, low, high, &options.key)
    READ(width, 2, 3840);
    READ(height, 2, 2160);
    READ(rate_numerator, 1, 60000);
    READ(rate_denominator, 1, 1001);
    READ(surface_pool_frames, 1, 8);
    // The hardware encoder retains input surfaces while filling its own
    // pipeline. Fewer conversion buffers can stall before the first output.
    READ(conversion_pool_frames, 8, 8);
    READ(bitrate, 1, 100000000);
    READ(idr_interval, 1, 600);
    READ(h264_profile, 0, 4);
    READ(encoder_tuning, 2, 3);
    READ(rtp_payload, 96, 127);
    READ(pending_frames, 1, 512);
    READ(pending_events, 1, 4096);
    READ(presentation_history, 1, 4096);
    READ(poll_interval_ms, 1, 1000);
    READ(events_per_batch, 1, 256);
    READ(queue_frames, 1, 8);
    READ(browser_max_frames, 1, 4096);
    READ(browser_max_age_ms, 100, 30000);
#undef READ
    options.initial_channel = node["initial_channel"].as<std::string>();
    if (!Channel(options.initial_channel) || options.width % 2 || options.height % 2 ||
        options.rate_numerator > 60 * options.rate_denominator || options.h264_profile == 3 ||
        options.rtp_payload == 97 || options.events_per_batch > options.pending_events ||
        options.browser_max_frames > options.presentation_history ||
        options.browser_max_age_ms < options.poll_interval_ms ||
        uint64_t(options.width) * options.height * 4 * (options.surface_pool_frames + options.conversion_pool_frames) >
            256ULL * 1024 * 1024)
      return absl::InvalidArgumentError("Incompatible web_preview geometry, codec or retention limits");
    return options;
  } catch (const std::exception& exception) {
    return absl::InvalidArgumentError(std::string("Invalid web preview settings: ") + exception.what());
  }
}

bool ValidWebPreviewRun(const std::string& run) {
  if (run.size() != 36)
    return false;
  for (std::size_t i = 0; i < run.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (run[i] != '-')
        return false;
    } else if (!((run[i] >= '0' && run[i] <= '9') || (run[i] >= 'a' && run[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<RouteOwner::Request> ParseWebPreviewRoute(const std::string& json) {
  if (json.empty() || json.size() > 1024)
    return absl::InvalidArgumentError("Preview route JSON exceeds its limit");
  auto* parser = json_parser_new();
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  RouteOwner::Request request;
  gint64 version = 0, stage = 0;
  std::string topology, operation;
  const bool valid = object && json_object_get_size(object) == 7 && Integer(object, "version", &version) &&
      version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN && stage <= INT_MAX &&
      String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "operation", &operation) && Decimal(operation, &request.operation) &&
      String(object, "requestId", &request.request_id) && Token(request.request_id) &&
      String(object, "channel", &request.channel) && Channel(request.channel);
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid preview route identity or channel");
  request.version = 1;
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string WebPreviewRouteReply(const RouteOwner::Request& request, const RouteOwner::Result& result) {
  using Status = RouteOwner::Status;
  const char* status = "failed";
  switch (result.status) {
    case Status::kActivated:
      status = "activated";
      break;
    case Status::kDeactivated:
      status = "deactivated";
      break;
    case Status::kUnavailable:
      status = "unavailable";
      break;
    case Status::kStale:
    case Status::kWrongSession:
      status = "stale";
      break;
    default:
      break;
  }
  return Serialize([&](JsonBuilder* builder) {
    Identity(builder, request.session, request.operation, request.channel);
    Add(builder, "requestId", request.request_id);
    Add(builder, "status", std::string(status));
    Add(builder, "reason", result.reason);
  });
}

std::string WebPreviewCapability(const WebPreviewOptions& options, const RouteOwner& owner) {
  return Serialize([&](JsonBuilder* builder) {
    Identity(
        builder,
        owner.session(),
        owner.operation_generation(),
        owner.selected_channel().empty() ? "none" : owner.selected_channel());
    json_builder_set_member_name(builder, "channels");
    json_builder_begin_array(builder);
    for (const auto& channel : owner.channels())
      json_builder_add_string_value(builder, channel.c_str());
    json_builder_end_array(builder);
    Add(builder, "width", options.width);
    Add(builder, "height", options.height);
    json_builder_set_member_name(builder, "seek");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_set_member_name(builder, "receiptLimits");
    json_builder_begin_object(builder);
    Add(builder, "maxFrames", options.browser_max_frames);
    Add(builder, "maxAgeMs", options.browser_max_age_ms);
    json_builder_end_object(builder);
  });
}
} // namespace hm::preview
