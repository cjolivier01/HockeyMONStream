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
bool Boolean(JsonObject* object, const char* name, bool* value) {
  auto* node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    return false;
  *value = json_node_get_boolean(node);
  return true;
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
void OverlayState(JsonBuilder* builder, const pipeline_internal::PreviewOverlayRuntimeState& state) {
  json_builder_begin_object(builder);
  Add(builder, "generation", std::to_string(state.generation()));
  const auto selection = state.selection();
  for (const auto& entry :
       {std::pair{"players", selection.players}, {"play", selection.play}, {"rink", selection.rink}}) {
    json_builder_set_member_name(builder, entry.first);
    json_builder_add_boolean_value(builder, entry.second);
  }
  json_builder_end_object(builder);
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
    READ(audio_bitrate, 4000, 650000);
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
    READ(authored_connect_timeout_ms, 1000, 300000);
    READ(authored_disconnect_timeout_ms, 1000, 300000);
#undef READ
    options.initial_channel = node["initial_channel"].as<std::string>();
    options.stun_server = node["stun_server"].as<std::string>();
    if (options.stun_server.size() > 2048 || options.stun_server.find('\0') != std::string::npos)
      return absl::InvalidArgumentError("Invalid web_preview.stun_server");
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

absl::StatusOr<WebPreviewSeekRequest> ParseWebPreviewSeek(const std::string& json) {
  if (json.size() > 4096)
    return absl::InvalidArgumentError("Preview seek JSON exceeds its limit");
  auto* parser = json_parser_new();
  unsigned member_count = 0;
  g_signal_connect(
      parser,
      "object-member",
      G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
      &member_count);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebPreviewSeekRequest request{};
  gint64 version = 0, stage = 0;
  std::string topology, identifier, position, delta;
  const bool valid = object && member_count == 6 && json_object_get_size(object) == 6 &&
      Integer(object, "version", &version) && version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN &&
      stage <= INT_MAX && String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &identifier) && Decimal(identifier, &request.request_id) &&
      request.request_id <= G_MAXINT64;
  bool valid_position = false;
  if (valid && String(object, "positionNs", &position)) {
    valid_position =
        ((position == "0" && (request.position_ns = 0, true)) || Decimal(position, &request.position_ns)) &&
        request.position_ns <= G_MAXINT64;
  } else if (valid && String(object, "deltaNs", &delta) && !delta.empty()) {
    int64_t value = 0;
    const auto parsed = std::from_chars(delta.data(), delta.data() + delta.size(), value);
    valid_position = parsed.ec == std::errc{} && parsed.ptr == delta.data() + delta.size() && value != 0 &&
        std::to_string(value) == delta;
    if (valid_position)
      request.delta_ns = value;
  }
  g_object_unref(parser);
  if (!valid || !valid_position)
    return absl::InvalidArgumentError("Invalid preview seek identity or position");
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string WebPreviewSeekReply(
    const WebPreviewSeekRequest& request,
    const std::string& status,
    const std::string& reason,
    uint64_t resulting_topology,
    std::optional<uint64_t> position_ns,
    std::optional<uint64_t> source_pts_ns) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", std::to_string(request.request_id));
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    Add(builder, "resultTopology", std::to_string(resulting_topology));
    for (const auto& item : {std::pair{"positionNs", position_ns}, {"sourcePtsNs", source_pts_ns}}) {
      json_builder_set_member_name(builder, item.first);
      if (item.second)
        json_builder_add_string_value(builder, std::to_string(*item.second).c_str());
      else
        json_builder_add_null_value(builder);
    }
  });
}

absl::StatusOr<WebCameraControlsRequest> ParseWebCameraControls(
    const std::string& json,
    const hm::tracking::CameraControlValues& defaults,
    bool query) {
  if (json.empty() || json.size() > 4096)
    return absl::InvalidArgumentError("Camera controls JSON exceeds its limit");
  const auto specs = hm::tracking::PlaytrackerControlSpecs(defaults);
  if (!specs.ok())
    return specs.status();
  auto* parser = json_parser_new();
  unsigned members = 0;
  g_signal_connect(
      parser,
      "object-member",
      G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
      &members);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebCameraControlsRequest request{};
  request.query = query;
  gint64 version = 0, stage = 0;
  uint64_t request_id = 0;
  std::string topology;
  bool valid = object && json_object_get_size(object) == (query ? 5U : 7U) && Integer(object, "version", &version) &&
      version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN && stage <= INT_MAX &&
      String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &request.request_id) && Decimal(request.request_id, &request_id) &&
      request_id <= G_MAXINT64;
  unsigned count = 0;
  if (valid && !query) {
    auto* controls_node = json_object_get_member(object, "controls");
    auto* controls =
        controls_node && JSON_NODE_HOLDS_OBJECT(controls_node) ? json_node_get_object(controls_node) : nullptr;
    valid = controls && Boolean(object, "forceAllTargets", &request.force_all_targets) &&
        json_object_get_size(controls) > 0 && json_object_get_size(controls) <= specs->size();
    if (valid) {
      count = json_object_get_size(controls);
      for (const auto& spec : *specs) {
        auto* node = json_object_get_member(controls, spec.id);
        if (!node)
          continue;
        const bool integer = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64;
        const bool real = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_DOUBLE;
        if (!integer && !(real && std::string(spec.id) == "Oversized_Player_Percent")) {
          valid = false;
          break;
        }
        const double value = integer ? static_cast<double>(json_node_get_int(node)) : json_node_get_double(node);
        if (!hm::tracking::ValidPlaytrackerControlValue(spec, value)) {
          valid = false;
          break;
        }
        request.controls.emplace(spec.id, value);
      }
      valid = valid && request.controls.size() == count;
    }
  }
  valid = valid && members == (query ? 5U : 7U + count);
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid camera control identity or values");
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string WebCameraControlsReply(
    const WebCameraControlsRequest& request,
    const std::string& status,
    const std::string& reason,
    const std::optional<hm::tracking::CameraControlValues>& controls) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", request.request_id);
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    json_builder_set_member_name(builder, "controls");
    if (!controls) {
      json_builder_add_null_value(builder);
      return;
    }
    json_builder_begin_object(builder);
    for (const auto& [id, value] : *controls) {
      json_builder_set_member_name(builder, id.c_str());
      if (id == "Oversized_Player_Percent")
        json_builder_add_double_value(builder, value);
      else
        json_builder_add_int_value(builder, static_cast<gint64>(value));
    }
    json_builder_end_object(builder);
  });
}

absl::StatusOr<WebRotationControlsRequest> ParseWebRotationControls(
    const std::string& json,
    const hm::program::RotationControlValues& range_reference,
    bool query) {
  if (json.empty() || json.size() > 4096 ||
      !hm::program::ValidRotationControls(range_reference, true, &range_reference))
    return absl::InvalidArgumentError("Rotation controls require bounded JSON and complete native ranges");
  auto* parser = json_parser_new();
  unsigned members = 0;
  g_signal_connect(parser, "object-member", G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer value) {
    ++*static_cast<unsigned*>(value);
  }), &members);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebRotationControlsRequest request{};
  request.query = query;
  gint64 version = 0, stage = 0;
  uint64_t request_id = 0;
  std::string topology;
  bool valid = object && json_object_get_size(object) == (query ? 5U : 6U) &&
      Integer(object, "version", &version) && version == 1 &&
      Integer(object, "stage", &stage) && stage >= INT_MIN && stage <= INT_MAX &&
      String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &request.request_id) && Decimal(request.request_id, &request_id) &&
      request_id <= G_MAXINT64;
  if (valid && !query) {
    auto* node = json_object_get_member(object, "controls");
    auto* controls = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : nullptr;
    valid = controls && json_object_get_size(controls) > 0 &&
        json_object_get_size(controls) <= hm::program::kRotationControls.size();
    if (valid) {
      for (const auto& spec : hm::program::kRotationControls) {
        gint64 value = 0;
        if (!json_object_has_member(controls, spec.id))
          continue;
        if (!Integer(controls, spec.id, &value) || value < INT_MIN || value > INT_MAX) {
          valid = false;
          break;
        }
        request.controls[spec.id] = static_cast<int>(value);
      }
      valid = valid && request.controls.size() == json_object_get_size(controls) &&
          hm::program::ValidRotationControls(request.controls, false, &range_reference);
    }
  }
  valid = valid && members == (query ? 5U : 6U + request.controls.size());
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid native rotation control request");
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string WebRotationControlsReply(
    const WebRotationControlsRequest& request,
    const std::string& status,
    const std::string& reason,
    const std::optional<hm::program::RotationControlValues>& controls,
    std::optional<uint64_t> operation,
    bool fixed_edge_suppressed) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", request.request_id);
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    json_builder_set_member_name(builder, "fixedEdgeSuppressed");
    json_builder_add_boolean_value(builder, fixed_edge_suppressed);
    json_builder_set_member_name(builder, "controls");
    if (controls) {
      json_builder_begin_object(builder);
      for (const auto& [id, value] : *controls)
        Add(builder, id.c_str(), static_cast<gint64>(value));
      json_builder_end_object(builder);
    } else {
      json_builder_add_null_value(builder);
    }
    json_builder_set_member_name(builder, "operation");
    if (operation)
      json_builder_add_string_value(builder, std::to_string(*operation).c_str());
    else
      json_builder_add_null_value(builder);
  });
}

std::string WebScoreboardSelectionPhase(const RouteOwner::Session& session, bool active) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", session.run);
    Add(builder, "stage", session.stage);
    Add(builder, "topology", std::to_string(session.topology));
    json_builder_set_member_name(builder, "active");
    json_builder_add_boolean_value(builder, active);
  });
}

absl::StatusOr<WebScoreboardRequest> ParseWebScoreboard(const std::string& json) {
  if (json.empty() || json.size() > 1024)
    return absl::InvalidArgumentError("Scoreboard JSON exceeds its limit");
  auto* parser = json_parser_new();
  unsigned members = 0;
  g_signal_connect(
      parser,
      "object-member",
      G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
      &members);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebScoreboardRequest request{};
  gint64 version = 0, stage = 0;
  uint64_t request_id = 0;
  std::string topology;
  const bool identity = object && json_object_get_size(object) == members && Integer(object, "version", &version) &&
      version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN && stage <= INT_MAX &&
      String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &request.request_id) && Decimal(request.request_id, &request_id) &&
      request_id <= G_MAXINT64 && String(object, "action", &request.action);
  const bool valid = identity &&
      (((request.action == "query" || request.action == "reselect") && members == 6) ||
       (request.action == "position" && members == 7 && String(object, "position", &request.position) &&
        (request.position == "upper-left" || request.position == "upper-center" || request.position == "upper-right")));
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid scoreboard identity or action");
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string WebScoreboardReply(
    const WebScoreboardRequest& request,
    const std::string& status,
    const std::string& reason,
    const std::optional<std::string>& position) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", request.request_id);
    Add(builder, "action", request.action);
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    json_builder_set_member_name(builder, "supported");
    json_builder_add_boolean_value(builder, position.has_value());
    json_builder_set_member_name(builder, "position");
    if (position)
      json_builder_add_string_value(builder, position->c_str());
    else
      json_builder_add_null_value(builder);
  });
}

absl::StatusOr<WebPlayerDrawingRequest> ParseWebPlayerDrawing(const std::string& json, bool query) {
  if (json.empty() || json.size() > 1024)
    return absl::InvalidArgumentError("Player drawing JSON exceeds its limit");
  auto* parser = json_parser_new();
  unsigned members = 0;
  g_signal_connect(
      parser,
      "object-member",
      G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
      &members);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebPlayerDrawingRequest request{};
  request.query = query;
  gint64 version = 0, stage = 0, layers = 0;
  uint64_t request_id = 0;
  std::string topology;
  const bool valid = object && members == (query ? 5U : 6U) && json_object_get_size(object) == members &&
      Integer(object, "version", &version) && version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN &&
      stage <= INT_MAX && String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &request.request_id) && Decimal(request.request_id, &request_id) &&
      request_id <= G_MAXINT64 && (query || (Integer(object, "layers", &layers) && layers >= 0 && layers <= 31));
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid player drawing identity or layers");
  request.session.stage = static_cast<int>(stage);
  request.layers = static_cast<unsigned>(layers);
  return request;
}

std::string WebPlayerDrawingReply(
    const WebPlayerDrawingRequest& request,
    const std::string& status,
    const std::string& reason,
    unsigned layers) {
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", request.request_id);
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    Add(builder, "layers", static_cast<gint64>(layers));
  });
}

absl::StatusOr<WebPreviewOverlayRequest> ParseWebPreviewOverlays(const std::string& json, bool query) {
  if (json.empty() || json.size() > 1024)
    return absl::InvalidArgumentError("Preview overlay JSON exceeds its limit");
  auto* parser = json_parser_new();
  unsigned member_count = 0;
  g_signal_connect(
      parser,
      "object-member",
      G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
      &member_count);
  const bool loaded = json_parser_load_from_data(parser, json.data(), json.size(), nullptr);
  auto* root = loaded ? json_parser_get_root(parser) : nullptr;
  auto* object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
  WebPreviewOverlayRequest request;
  request.query = query;
  gint64 version = 0, stage = 0;
  std::string topology, generation;
  const bool valid = object && member_count == (query ? 5U : 9U) && json_object_get_size(object) == member_count &&
      Integer(object, "version", &version) && version == 1 && Integer(object, "stage", &stage) && stage >= INT_MIN &&
      stage <= INT_MAX && String(object, "run", &request.session.run) && ValidWebPreviewRun(request.session.run) &&
      String(object, "topology", &topology) && Decimal(topology, &request.session.topology) &&
      String(object, "requestId", &request.request_id) && Token(request.request_id) &&
      (query ||
       (String(object, "generation", &generation) && Decimal(generation, &request.command.generation) &&
        Boolean(object, "players", &request.command.selection.players) &&
        Boolean(object, "play", &request.command.selection.play) &&
        Boolean(object, "rink", &request.command.selection.rink)));
  g_object_unref(parser);
  if (!valid)
    return absl::InvalidArgumentError("Invalid preview overlay identity or selection");
  request.session.stage = static_cast<int>(stage);
  return request;
}

std::string ApplyWebPreviewOverlays(
    const WebPreviewOverlayRequest& request,
    const RouteOwner::Session& session,
    pipeline_internal::PreviewOverlayRuntimeState& state,
    const std::string& mutation_blocked_reason,
    const std::function<bool(const pipeline_internal::PreviewOverlaySelection&)>& apply) {
  std::string status, reason;
  if (session.run.empty() || !session.topology || !apply) {
    status = "unavailable";
    reason = "selectable-preview-unavailable";
  } else if (!(request.session == session)) {
    status = "stale";
    reason = "stale-session";
  } else if (request.query) {
    status = "state";
  } else if (!state.is_fresh(request.command)) {
    status = "stale";
    reason = "stale-generation";
  } else if (!mutation_blocked_reason.empty()) {
    status = "failed";
    reason = mutation_blocked_reason;
  } else if (!apply(request.command.selection)) {
    status = "failed";
    reason = "apply-failed";
  } else {
    state.commit(request.command);
    status = "applied";
  }
  return Serialize([&](JsonBuilder* builder) {
    Add(builder, "version", gint64{1});
    Add(builder, "run", request.session.run);
    Add(builder, "stage", request.session.stage);
    Add(builder, "topology", std::to_string(request.session.topology));
    Add(builder, "requestId", request.request_id);
    if (!request.query)
      Add(builder, "generation", std::to_string(request.command.generation));
    Add(builder, "status", status);
    Add(builder, "reason", reason);
    json_builder_set_member_name(builder, "state");
    OverlayState(builder, state);
  });
}

void WebPreviewOverlayArtifactMailbox::Publish(WebPreviewOverlayArtifact artifact) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_ && last_->session == artifact.session && last_->operation == artifact.operation &&
      last_->generation == artifact.generation && last_->channel == artifact.channel &&
      last_->status == artifact.status && last_->reason == artifact.reason)
    return;
  last_ = artifact;
  pending_ = std::move(artifact);
}

void WebPreviewOverlayArtifactMailbox::Replay(const RouteOwner::Session& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_ && last_->session == session)
    pending_ = last_;
}

std::optional<WebPreviewOverlayArtifact> WebPreviewOverlayArtifactMailbox::Take(
    const RouteOwner::Session& session,
    uint64_t operation,
    const std::string& channel,
    const pipeline_internal::PreviewOverlayCommand& overlays) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<WebPreviewOverlayArtifact> artifact;
  artifact.swap(pending_);
  if (!artifact || !overlays.selection.rink || !(artifact->session == session) ||
      artifact->generation != overlays.generation || artifact->operation != operation || artifact->channel != channel)
    return std::nullopt;
  return artifact;
}

std::string WebPreviewOverlayArtifactEvent(const WebPreviewOverlayArtifact& artifact) {
  return Serialize([&](JsonBuilder* builder) {
    Identity(builder, artifact.session, artifact.operation, artifact.channel);
    Add(builder, "generation", std::to_string(artifact.generation));
    Add(builder, "artifact", std::string("rink-mask"));
    Add(builder, "status", artifact.status);
    Add(builder, "reason", artifact.reason);
  });
}

std::string WebPreviewCapability(
    const WebPreviewOptions& options,
    const RouteOwner& owner,
    const pipeline_internal::PreviewOverlayRuntimeState& overlays,
    bool seek) {
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
    json_builder_set_member_name(builder, "overlays");
    OverlayState(builder, overlays);
    Add(builder, "width", options.width);
    Add(builder, "height", options.height);
    json_builder_set_member_name(builder, "seek");
    json_builder_add_boolean_value(builder, seek);
    json_builder_set_member_name(builder, "receiptLimits");
    json_builder_begin_object(builder);
    Add(builder, "maxFrames", options.browser_max_frames);
    Add(builder, "maxAgeMs", options.browser_max_age_ms);
    json_builder_end_object(builder);
  });
}
} // namespace hm::preview
