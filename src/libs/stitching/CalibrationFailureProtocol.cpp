#include "hstream/src/libs/stitching/CalibrationFailureProtocol.h"

#include <json-glib/json-glib.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

namespace hm::stitching::failure {
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::invalid_argument(message);
}
bool token(const std::string& value, bool empty = false) {
  return (empty || !value.empty()) && value.size() <= 128 &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
void keys(JsonObject* object, std::initializer_list<const char*> names) {
  require(object && json_object_get_size(object) == names.size(), "Unexpected diagnostic context fields");
  for (const auto name : names)
    require(json_object_has_member(object, name), "Missing diagnostic context field");
}
JsonNode* scalar(JsonObject* object, const char* key, GType type) {
  auto* node = json_object_get_member(object, key);
  require(
      node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == type, "Invalid diagnostic field type");
  return node;
}
std::string text(JsonObject* object, const char* key) {
  return json_node_get_string(scalar(object, key, G_TYPE_STRING));
}
int integer(JsonObject* object, const char* key, int minimum, int maximum) {
  const auto value = json_node_get_int(scalar(object, key, G_TYPE_INT64));
  require(value >= minimum && value <= maximum, "Diagnostic integer exceeds its bound");
  return int(value);
}
bool boolean(JsonObject* object, const char* key) {
  return json_node_get_boolean(scalar(object, key, G_TYPE_BOOLEAN));
}
double number(JsonObject* object, const char* key) {
  auto* node = json_object_get_member(object, key);
  require(
      node && JSON_NODE_HOLDS_VALUE(node) &&
          (json_node_get_value_type(node) == G_TYPE_DOUBLE || json_node_get_value_type(node) == G_TYPE_INT64),
      "Invalid diagnostic number");
  const double value = json_node_get_double(node);
  require(std::isfinite(value) && value > 0 && value <= 360, "Diagnostic field of view exceeds its bound");
  return value;
}
JsonObject* object(JsonObject* parent, const char* key) {
  auto* node = json_object_get_member(parent, key);
  require(node && JSON_NODE_HOLDS_OBJECT(node), "Expected diagnostic object");
  return json_node_get_object(node);
}
Camera camera(JsonObject* value) {
  keys(value, {"configuration", "horizontal_fov", "vertical_fov"});
  return {text(value, "configuration"), number(value, "horizontal_fov"), number(value, "vertical_fov")};
}
Context context(JsonObject* value) {
  keys(
      value,
      {"version",
       "selected",
       "defaults",
       "camera_differs_from_defaults",
       "calibration_required",
       "calibration_start_stage",
       "calibration_only"});
  require(integer(value, "version", 1, 1) == 1, "Unsupported diagnostic context");
  auto* selected = object(value, "selected");
  keys(
      selected,
      {"matcher", "mapping_backend", "projection", "frame_count", "control_point_limit", "max_output_width", "camera"});
  auto* defaults = object(value, "defaults");
  keys(defaults, {"control_point_limit", "camera"});
  Context result;
  result.matcher = text(selected, "matcher");
  result.mapping_backend = text(selected, "mapping_backend");
  result.projection = text(selected, "projection");
  result.frame_count = integer(selected, "frame_count", 1, 1000000);
  result.control_point_limit = integer(selected, "control_point_limit", 1, 1000000);
  result.max_output_width = integer(selected, "max_output_width", 0, INT_MAX);
  result.camera = camera(object(selected, "camera"));
  result.default_camera = camera(object(defaults, "camera"));
  result.default_control_point_limit = integer(defaults, "control_point_limit", 1, 1000000);
  result.camera_differs_from_defaults = boolean(value, "camera_differs_from_defaults");
  result.calibration_required = boolean(value, "calibration_required");
  result.calibration_only = boolean(value, "calibration_only");
  result.calibration_start_stage = text(value, "calibration_start_stage");
  require(ValidateContext(result).ok(), "Invalid resolved diagnostic context");
  return result;
}
JsonObject* encode_camera(const Camera& camera) {
  auto* value = json_object_new();
  json_object_set_string_member(value, "configuration", camera.configuration.c_str());
  json_object_set_double_member(value, "horizontal_fov", camera.horizontal_fov);
  json_object_set_double_member(value, "vertical_fov", camera.vertical_fov);
  return value;
}
JsonObject* encode_context(const Context& context) {
  auto* value = json_object_new();
  json_object_set_int_member(value, "version", 1);
  auto* selected = json_object_new();
  json_object_set_string_member(selected, "matcher", context.matcher.c_str());
  json_object_set_string_member(selected, "mapping_backend", context.mapping_backend.c_str());
  json_object_set_string_member(selected, "projection", context.projection.c_str());
  json_object_set_int_member(selected, "frame_count", context.frame_count);
  json_object_set_int_member(selected, "control_point_limit", context.control_point_limit);
  json_object_set_int_member(selected, "max_output_width", context.max_output_width);
  json_object_set_object_member(selected, "camera", encode_camera(context.camera));
  json_object_set_object_member(value, "selected", selected);
  auto* defaults = json_object_new();
  json_object_set_int_member(defaults, "control_point_limit", context.default_control_point_limit);
  json_object_set_object_member(defaults, "camera", encode_camera(context.default_camera));
  json_object_set_object_member(value, "defaults", defaults);
  json_object_set_boolean_member(value, "camera_differs_from_defaults", context.camera_differs_from_defaults);
  json_object_set_boolean_member(value, "calibration_required", context.calibration_required);
  json_object_set_boolean_member(value, "calibration_only", context.calibration_only);
  json_object_set_string_member(value, "calibration_start_stage", context.calibration_start_stage.c_str());
  return value;
}
JsonObject* envelope(const std::string& binding, const std::string& invocation, unsigned generation) {
  require(
      HexIdentity(binding, 64) && HexIdentity(invocation, 32) && generation > 0 && generation <= kMaximumGenerations,
      "Invalid diagnostic scope identity");
  auto* value = json_object_new();
  json_object_set_int_member(value, "version", 1);
  json_object_set_string_member(value, "binding", binding.c_str());
  json_object_set_string_member(value, "invocation", invocation.c_str());
  json_object_set_int_member(value, "generation", generation);
  return value;
}
std::string encode(JsonObject* object, const char* prefix) {
  auto* node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, object);
  auto* generator = json_generator_new();
  json_generator_set_root(generator, node);
  auto* data = json_generator_to_data(generator, nullptr);
  std::string result = std::string(prefix) + data;
  g_free(data);
  g_object_unref(generator);
  json_node_free(node);
  require(result.size() <= kMaximumRecordBytes, "Diagnostic record exceeds its bound");
  return result;
}
unsigned members(JsonNode* node) {
  if (JSON_NODE_HOLDS_OBJECT(node)) {
    auto* value = json_node_get_object(node);
    GList* names = json_object_get_members(value);
    unsigned count = json_object_get_size(value);
    for (auto* item = names; item; item = item->next)
      count += members(json_object_get_member(value, static_cast<const char*>(item->data)));
    g_list_free(names);
    return count;
  }
  if (JSON_NODE_HOLDS_ARRAY(node)) {
    auto* value = json_node_get_array(node);
    unsigned count = 0;
    for (unsigned i = 0; i < json_array_get_length(value); ++i)
      count += members(json_array_get_element(value, i));
    return count;
  }
  return 0;
}
} // namespace
bool HexIdentity(const std::string& value, std::size_t size) {
  return value.size() == size &&
      std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
absl::Status ValidateContext(const Context& value) {
  auto camera_ok = [](const Camera& camera) {
    return token(camera.configuration) && std::isfinite(camera.horizontal_fov) && camera.horizontal_fov > 0 &&
        camera.horizontal_fov <= 360 && std::isfinite(camera.vertical_fov) && camera.vertical_fov > 0 &&
        camera.vertical_fov <= 360;
  };
  if (!token(value.matcher) || !token(value.mapping_backend) || !token(value.projection) || !camera_ok(value.camera) ||
      !camera_ok(value.default_camera) || value.frame_count < 1 || value.frame_count > 1000000 ||
      value.control_point_limit < 1 || value.control_point_limit > 1000000 || value.default_control_point_limit < 1 ||
      value.default_control_point_limit > 1000000 || value.max_output_width < 0 ||
      !token(value.calibration_start_stage, true))
    return absl::InvalidArgumentError("Invalid resolved calibration failure context");
  return absl::OkStatus();
}
absl::StatusOr<std::string> ScopeRecord(
    const std::string& binding,
    const std::string& invocation,
    unsigned generation,
    int stage,
    unsigned instances) {
  try {
    auto* value = envelope(binding, invocation, generation);
    json_object_set_int_member(value, "native_stage", stage);
    json_object_set_int_member(value, "instances", instances);
    return encode(value, kScopePrefix);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
absl::StatusOr<std::string> ContextRecord(
    const std::string& binding,
    const std::string& invocation,
    unsigned generation,
    const Context& context) {
  const auto valid = ValidateContext(context);
  if (!valid.ok())
    return valid;
  try {
    auto* value = envelope(binding, invocation, generation);
    json_object_set_object_member(value, "context", encode_context(context));
    return encode(value, kContextPrefix);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}
absl::Status Session::Record(const std::string& raw) {
  if (!error_.empty()) {
    if (raw.size() <= kMaximumLineBytes && raw.find('\0') == std::string::npos)
      evidence_.Record(raw, generation_, false);
    return absl::InvalidArgumentError(error_);
  }
  std::string line = raw;
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  const bool scope = line.compare(0, sizeof(kScopePrefix) - 1, kScopePrefix) == 0;
  const bool context_record = line.compare(0, sizeof(kContextPrefix) - 1, kContextPrefix) == 0;
  try {
    require(
        line.size() <= kMaximumLineBytes && line.find('\0') == std::string::npos,
        "Diagnostic line exceeds its bound or contains NUL");
    if (!scope && !context_record) {
      evidence_.Record(line, generation_, contexts_.count(generation_) != 0);
      return absl::OkStatus();
    }
    require(HexIdentity(binding_, 64) && line.size() <= kMaximumRecordBytes, "Unbound or oversized diagnostic record");
    std::unique_ptr<JsonParser, decltype(&g_object_unref)> parser(json_parser_new(), g_object_unref);
    unsigned count = 0;
    g_signal_connect(
        parser.get(),
        "object-member",
        G_CALLBACK(+[](JsonParser*, JsonObject*, const gchar*, gpointer data) { ++*static_cast<unsigned*>(data); }),
        &count);
    const auto json = line.substr(scope ? sizeof(kScopePrefix) - 1 : sizeof(kContextPrefix) - 1);
    require(json.find("\\u0000") == std::string::npos, "NUL is forbidden in diagnostic protocol strings");
    require(json_parser_load_from_data(parser.get(), json.data(), json.size(), nullptr), "Malformed diagnostic JSON");
    auto* root = json_parser_get_root(parser.get());
    require(root && JSON_NODE_HOLDS_OBJECT(root) && count == members(root), "Duplicate diagnostic JSON fields");
    auto* value = json_node_get_object(root);
    if (scope)
      keys(value, {"version", "binding", "invocation", "generation", "native_stage", "instances"});
    else
      keys(value, {"version", "binding", "invocation", "generation", "context"});
    require(integer(value, "version", 1, 1) == 1 && text(value, "binding") == binding_, "Diagnostic binding changed");
    const auto invocation = text(value, "invocation");
    require(
        HexIdentity(invocation, 32) && (invocation_.empty() || invocation_ == invocation),
        "Diagnostic invocation changed");
    const auto generation = integer(value, "generation", 1, kMaximumGenerations);
    if (scope) {
      require(std::uint64_t(generation) == generation_ + 1, "Diagnostic scope was repeated or skipped");
      integer(value, "native_stage", INT_MIN, INT_MAX);
      require(integer(value, "instances", 1, 1) == 1, "Multiple native contexts are not attributable");
      invocation_ = invocation;
      generation_ = generation;
      evidence_.AdvanceGeneration(generation_);
    } else {
      require(
          generation_ == std::uint64_t(generation) && invocation_ == invocation && !contexts_.count(generation),
          "Unowned or repeated diagnostic context");
      contexts_.emplace(generation, context(object(value, "context")));
      records_.emplace(generation, line);
      evidence_.AdvanceGeneration(generation_, true);
    }
    return absl::OkStatus();
  } catch (const std::exception& error) {
    error_ = error.what();
    return absl::InvalidArgumentError(error_);
  }
}
Diagnosis Session::Analyze(const std::string& terminal, const Context* fallback, const std::string& stage) const {
  return evidence_.Analyze(terminal, error_.empty() ? contexts_ : std::map<std::uint64_t, Context>{}, fallback, stage);
}
} // namespace hm::stitching::failure
