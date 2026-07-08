#include "hstream/src/libs/pipeline_controller/GstPropertyService.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

#include <glib-object.h>
#include <gst/gst.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hm::pipeline {
namespace {

std::string safe_string(const char* value) {
  return value ? value : "";
}

std::string trim(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::string to_lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

int64_t parse_int64_exact(const std::string& raw_value);

bool parse_bool(const std::string& value) {
  const std::string lowered = to_lower(trim(value));
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
    return false;
  }
  return parse_int64_exact(lowered) != 0;
}

int64_t parse_int64_exact(const std::string& raw_value) {
  const std::string value = trim(raw_value);
  if (value.empty()) {
    throw std::invalid_argument("empty integer");
  }
  std::size_t end = 0;
  const long long parsed = std::stoll(value, &end, 0);
  if (end != value.size()) {
    throw std::invalid_argument("integer contains trailing characters");
  }
  return static_cast<int64_t>(parsed);
}

uint64_t parse_uint64_exact(const std::string& raw_value) {
  const std::string value = trim(raw_value);
  if (value.empty()) {
    throw std::invalid_argument("empty unsigned integer");
  }
  if (value[0] == '-') {
    throw std::invalid_argument("negative value is not valid for an unsigned property");
  }
  std::size_t end = 0;
  const unsigned long long parsed = std::stoull(value, &end, 0);
  if (end != value.size()) {
    throw std::invalid_argument("unsigned integer contains trailing characters");
  }
  return static_cast<uint64_t>(parsed);
}

double parse_double_exact(const std::string& raw_value) {
  const std::string value = trim(raw_value);
  if (value.empty()) {
    throw std::invalid_argument("empty floating-point value");
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end != value.c_str() + value.size()) {
    throw std::invalid_argument("floating-point value contains trailing characters");
  }
  if (errno == ERANGE) {
    throw std::out_of_range("floating-point value is out of range");
  }
  return parsed;
}

void require_signed_range(int64_t value, int64_t minimum, int64_t maximum) {
  if (value < minimum || value > maximum) {
    throw std::out_of_range("integer value outside property range");
  }
}

void require_unsigned_range(uint64_t value, uint64_t minimum, uint64_t maximum) {
  if (value < minimum || value > maximum) {
    throw std::out_of_range("unsigned integer value outside property range");
  }
}

void require_double_range(double value, double minimum, double maximum) {
  if (value < minimum || value > maximum) {
    throw std::out_of_range("floating-point value outside property range");
  }
}

int parse_enum_value(GType value_type, const std::string& raw_value) {
  GEnumClass* enum_class = G_ENUM_CLASS(g_type_class_ref(value_type));
  GEnumValue* enum_value = g_enum_get_value_by_nick(enum_class, raw_value.c_str());
  if (!enum_value) {
    enum_value = g_enum_get_value_by_name(enum_class, raw_value.c_str());
  }
  if (!enum_value) {
    g_type_class_unref(enum_class);
    const int64_t parsed = parse_int64_exact(raw_value);
    require_signed_range(parsed, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    return static_cast<int>(parsed);
  }
  const int result = enum_value->value;
  g_type_class_unref(enum_class);
  return result;
}

std::string element_name(GstElement* element) {
  if (!element) {
    return "";
  }
  gchar* name = gst_element_get_name(element);
  std::string result = safe_string(name);
  g_free(name);
  return result;
}

std::string element_path(GstElement* element, const std::string& parent_path) {
  const std::string name = element_name(element);
  if (parent_path.empty()) {
    return name;
  }
  return parent_path + "." + name;
}

std::string factory_name(GstElement* element) {
  if (!element) {
    return "";
  }
  GstElementFactory* factory = gst_element_get_factory(element);
  if (!factory) {
    return "";
  }
  return safe_string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
}

std::string serialize_property_value(GObject* object, GParamSpec* pspec) {
  if (!object || !pspec || !(pspec->flags & G_PARAM_READABLE)) {
    return "";
  }

  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));
  g_object_get_property(object, pspec->name, &value);
  if (G_VALUE_HOLDS_BOXED(&value) && g_value_get_boxed(&value) == nullptr) {
    g_value_unset(&value);
    return "";
  }
  gchar* serialized = gst_value_serialize(&value);
  std::string result = safe_string(serialized);
  g_free(serialized);
  g_value_unset(&value);
  return result;
}

std::string serialize_value(const GValue* value) {
  if (!value) {
    return "";
  }
  if (G_VALUE_HOLDS_BOXED(value) && g_value_get_boxed(value) == nullptr) {
    return "";
  }
  gchar* serialized = gst_value_serialize(value);
  std::string result = safe_string(serialized);
  g_free(serialized);
  return result;
}

std::string serialize_default_value(GParamSpec* pspec) {
  if (!pspec) {
    return "";
  }
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));
  g_param_value_set_default(pspec, &value);
  std::string result = serialize_value(&value);
  g_value_unset(&value);
  return result;
}

int parse_flags_value(GType value_type, const std::string& raw_value) {
  const std::string value = trim(raw_value);
  if (value.empty()) {
    return 0;
  }

  try {
    const int64_t parsed = parse_int64_exact(value);
    require_signed_range(parsed, 0, std::numeric_limits<int>::max());
    return static_cast<int>(parsed);
  } catch (const std::exception&) {
  }

  GFlagsClass* flags_class = G_FLAGS_CLASS(g_type_class_ref(value_type));
  int result = 0;
  std::string token;
  bool saw_token = false;
  for (const char c : value) {
    if (c == ',' || c == '|' || c == '+') {
      const std::string trimmed = trim(token);
      if (!trimmed.empty()) {
        GFlagsValue* flags_value = g_flags_get_value_by_nick(flags_class, trimmed.c_str());
        if (!flags_value) {
          flags_value = g_flags_get_value_by_name(flags_class, trimmed.c_str());
        }
        if (!flags_value) {
          g_type_class_unref(flags_class);
          throw std::invalid_argument("unknown flags token '" + trimmed + "'");
        }
        result |= flags_value->value;
        saw_token = true;
      }
      token.clear();
    } else {
      token.push_back(c);
    }
  }

  const std::string trimmed = trim(token);
  if (!trimmed.empty()) {
    GFlagsValue* flags_value = g_flags_get_value_by_nick(flags_class, trimmed.c_str());
    if (!flags_value) {
      flags_value = g_flags_get_value_by_name(flags_class, trimmed.c_str());
    }
    if (!flags_value) {
      g_type_class_unref(flags_class);
      throw std::invalid_argument("unknown flags token '" + trimmed + "'");
    }
    result |= flags_value->value;
    saw_token = true;
  }

  g_type_class_unref(flags_class);
  if (!saw_token) {
    throw std::invalid_argument("empty flags value");
  }
  return result;
}

std::vector<GstEnumValueInfo> enum_values(GType value_type) {
  std::vector<GstEnumValueInfo> result;
  if (G_TYPE_IS_ENUM(value_type)) {
    GEnumClass* enum_class = G_ENUM_CLASS(g_type_class_ref(value_type));
    for (guint i = 0; i < enum_class->n_values; ++i) {
      const GEnumValue& value = enum_class->values[i];
      result.push_back({
          .name = safe_string(value.value_name),
          .nick = safe_string(value.value_nick),
          .value = value.value,
      });
    }
    g_type_class_unref(enum_class);
  } else if (G_TYPE_IS_FLAGS(value_type)) {
    GFlagsClass* flags_class = G_FLAGS_CLASS(g_type_class_ref(value_type));
    for (guint i = 0; i < flags_class->n_values; ++i) {
      const GFlagsValue& value = flags_class->values[i];
      result.push_back({
          .name = safe_string(value.value_name),
          .nick = safe_string(value.value_nick),
          .value = static_cast<int>(value.value),
      });
    }
    g_type_class_unref(flags_class);
  }
  return result;
}

void populate_range_metadata(GParamSpec* pspec, std::string* minimum_value, std::string* maximum_value) {
  if (!pspec || !minimum_value || !maximum_value) {
    return;
  }

  const GType value_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
  if (value_type == G_TYPE_INT) {
    auto* spec = G_PARAM_SPEC_INT(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_UINT) {
    auto* spec = G_PARAM_SPEC_UINT(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_LONG) {
    auto* spec = G_PARAM_SPEC_LONG(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_ULONG) {
    auto* spec = G_PARAM_SPEC_ULONG(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_INT64) {
    auto* spec = G_PARAM_SPEC_INT64(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_UINT64) {
    auto* spec = G_PARAM_SPEC_UINT64(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_FLOAT) {
    auto* spec = G_PARAM_SPEC_FLOAT(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  } else if (value_type == G_TYPE_DOUBLE) {
    auto* spec = G_PARAM_SPEC_DOUBLE(pspec);
    *minimum_value = std::to_string(spec->minimum);
    *maximum_value = std::to_string(spec->maximum);
  }
}

absl::Status set_typed_value(GObject* object, GParamSpec* pspec, const std::string& raw_value) {
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));

  try {
    const GType value_type = G_VALUE_TYPE(&value);
    if (value_type == G_TYPE_BOOLEAN) {
      g_value_set_boolean(&value, parse_bool(raw_value));
    } else if (value_type == G_TYPE_INT) {
      const int64_t parsed = parse_int64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_INT(pspec);
      require_signed_range(parsed, spec->minimum, spec->maximum);
      g_value_set_int(&value, static_cast<gint>(parsed));
    } else if (value_type == G_TYPE_UINT) {
      const uint64_t parsed = parse_uint64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_UINT(pspec);
      require_unsigned_range(parsed, spec->minimum, spec->maximum);
      g_value_set_uint(&value, static_cast<guint>(parsed));
    } else if (value_type == G_TYPE_LONG) {
      const int64_t parsed = parse_int64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_LONG(pspec);
      require_signed_range(parsed, spec->minimum, spec->maximum);
      g_value_set_long(&value, static_cast<glong>(parsed));
    } else if (value_type == G_TYPE_ULONG) {
      const uint64_t parsed = parse_uint64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_ULONG(pspec);
      require_unsigned_range(parsed, spec->minimum, spec->maximum);
      g_value_set_ulong(&value, static_cast<gulong>(parsed));
    } else if (value_type == G_TYPE_INT64) {
      const int64_t parsed = parse_int64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_INT64(pspec);
      require_signed_range(parsed, spec->minimum, spec->maximum);
      g_value_set_int64(&value, static_cast<gint64>(parsed));
    } else if (value_type == G_TYPE_UINT64) {
      const uint64_t parsed = parse_uint64_exact(raw_value);
      auto* spec = G_PARAM_SPEC_UINT64(pspec);
      require_unsigned_range(parsed, spec->minimum, spec->maximum);
      g_value_set_uint64(&value, static_cast<guint64>(parsed));
    } else if (value_type == G_TYPE_FLOAT) {
      const double parsed = parse_double_exact(raw_value);
      auto* spec = G_PARAM_SPEC_FLOAT(pspec);
      require_double_range(parsed, spec->minimum, spec->maximum);
      g_value_set_float(&value, static_cast<gfloat>(parsed));
    } else if (value_type == G_TYPE_DOUBLE) {
      const double parsed = parse_double_exact(raw_value);
      auto* spec = G_PARAM_SPEC_DOUBLE(pspec);
      require_double_range(parsed, spec->minimum, spec->maximum);
      g_value_set_double(&value, parsed);
    } else if (value_type == G_TYPE_STRING) {
      g_value_set_string(&value, raw_value.c_str());
    } else if (G_TYPE_IS_ENUM(value_type)) {
      g_value_set_enum(&value, parse_enum_value(value_type, raw_value));
    } else if (G_TYPE_IS_FLAGS(value_type)) {
      g_value_set_flags(&value, parse_flags_value(value_type, raw_value));
    } else if (!gst_value_deserialize(&value, raw_value.c_str())) {
      g_value_unset(&value);
      return absl::InvalidArgumentError(absl::StrCat("Unsupported property type for '", pspec->name, "'"));
    }
  } catch (const std::exception& ex) {
    g_value_unset(&value);
    return absl::InvalidArgumentError(absl::StrCat(
        "Could not parse property '",
        pspec->name,
        "' value '",
        redactSensitiveValueForDisplay(pspec->name, raw_value),
        "': ",
        ex.what()));
  }

  if (g_param_value_validate(pspec, &value)) {
    g_value_unset(&value);
    return absl::InvalidArgumentError(absl::StrCat(
        "Property '",
        pspec->name,
        "' value '",
        redactSensitiveValueForDisplay(pspec->name, raw_value),
        "' is outside allowed constraints"));
  }

  g_object_set_property(object, pspec->name, &value);
  g_value_unset(&value);
  return absl::OkStatus();
}

void collect_element_tree(GstElement* element, const std::string& parent_path, std::vector<GstElementInfo>* result) {
  if (!element || !result) {
    return;
  }

  const std::string path = element_path(element, parent_path);
  result->push_back({
      .path = path,
      .name = element_name(element),
      .type_name = G_OBJECT_TYPE_NAME(element),
      .factory_name = factory_name(element),
  });

  if (!GST_IS_BIN(element)) {
    return;
  }

  GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(element));
  if (!iterator) {
    return;
  }

  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(iterator, &item)) {
      case GST_ITERATOR_OK: {
        GstElement* child = GST_ELEMENT(g_value_get_object(&item));
        if (child) {
          collect_element_tree(child, path, result);
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(iterator);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }

  g_value_unset(&item);
  gst_iterator_free(iterator);
}

absl::StatusOr<GstState> effective_element_state(GstElement* element) {
  GstState current_state = GST_STATE_NULL;
  GstState pending_state = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn state_result = gst_element_get_state(element, &current_state, &pending_state, 0);
  if (state_result == GST_STATE_CHANGE_FAILURE) {
    return absl::FailedPreconditionError(absl::StrCat("Cannot query state for element ", G_OBJECT_TYPE_NAME(element)));
  }
  if (pending_state != GST_STATE_VOID_PENDING && pending_state > current_state) {
    return pending_state;
  }
  return current_state;
}

absl::Status set_element_property_from_string_in_state(
    GstElement* element,
    const std::string& property_name,
    const std::string& value,
    GstState current_state) {
  if (!element) {
    return absl::InvalidArgumentError("Cannot set property on a null GstElement");
  }
  GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property_name.c_str());
  if (!pspec) {
    return absl::NotFoundError(
        absl::StrCat("Property '", property_name, "' does not exist on ", G_OBJECT_TYPE_NAME(element)));
  }
  if (!propertyMutableInCurrentState(pspec, current_state)) {
    return absl::FailedPreconditionError(
        absl::StrCat("Property '", property_name, "' cannot be changed in the current pipeline state"));
  }
  return set_typed_value(G_OBJECT(element), pspec, value);
}

} // namespace

RuntimeControlApplyMode propertyApplyMode(GParamSpec* pspec) {
  if (!pspec || !(pspec->flags & G_PARAM_WRITABLE) || (pspec->flags & G_PARAM_CONSTRUCT_ONLY)) {
    return RuntimeControlApplyMode::Restart;
  }

  if (pspec->flags & GST_PARAM_MUTABLE_PLAYING) {
    return RuntimeControlApplyMode::Live;
  }
  if (pspec->flags & GST_PARAM_MUTABLE_PAUSED) {
    return RuntimeControlApplyMode::Paused;
  }
  if (pspec->flags & GST_PARAM_MUTABLE_READY) {
    return RuntimeControlApplyMode::Ready;
  }
  return RuntimeControlApplyMode::Restart;
}

bool propertyMutableInCurrentState(GParamSpec* pspec, GstState state) {
  if (!pspec || !(pspec->flags & G_PARAM_WRITABLE) || (pspec->flags & G_PARAM_CONSTRUCT_ONLY)) {
    return false;
  }

  switch (propertyApplyMode(pspec)) {
    case RuntimeControlApplyMode::Live:
      return true;
    case RuntimeControlApplyMode::Paused:
      return state <= GST_STATE_PAUSED;
    case RuntimeControlApplyMode::Ready:
      return state <= GST_STATE_READY;
    case RuntimeControlApplyMode::Restart:
      return state <= GST_STATE_NULL;
  }
  return false;
}

RuntimeControlKind propertyControlKind(GType value_type) {
  if (value_type == G_TYPE_BOOLEAN) {
    return RuntimeControlKind::Toggle;
  }
  if (value_type == G_TYPE_INT || value_type == G_TYPE_UINT || value_type == G_TYPE_LONG ||
      value_type == G_TYPE_ULONG || value_type == G_TYPE_INT64 || value_type == G_TYPE_UINT64) {
    return RuntimeControlKind::Integer;
  }
  if (value_type == G_TYPE_FLOAT || value_type == G_TYPE_DOUBLE) {
    return RuntimeControlKind::Float;
  }
  if (G_TYPE_IS_ENUM(value_type) || G_TYPE_IS_FLAGS(value_type)) {
    return RuntimeControlKind::Enum;
  }
  return RuntimeControlKind::Text;
}

bool isSensitivePropertyName(const std::string& property_name) {
  const std::string name = absl::AsciiStrToLower(property_name);
  return name.find("password") != std::string::npos || name.find("passwd") != std::string::npos ||
      name.find("pwd") != std::string::npos || name.find("secret") != std::string::npos ||
      name.find("token") != std::string::npos || name.find("credential") != std::string::npos ||
      name.find("stream-key") != std::string::npos || name.find("stream_key") != std::string::npos || name == "key" ||
      name == "uri" || name.find("-uri") != std::string::npos || name.find("_uri") != std::string::npos ||
      name.find("location") != std::string::npos || name.find("url") != std::string::npos;
}

std::string redactSensitiveValueForDisplay(const std::string& property_name, const std::string& value) {
  if (value.empty()) {
    return value;
  }
  if (isSensitivePropertyName(property_name)) {
    return "[redacted]";
  }
  return value;
}

absl::StatusOr<std::vector<GstPropertyInfo>> listElementProperties(GstElement* element) {
  if (!element) {
    return absl::InvalidArgumentError("Cannot list properties for a null GstElement");
  }

  guint property_count = 0;
  GParamSpec** properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(element), &property_count);
  std::vector<GstPropertyInfo> result;
  result.reserve(property_count);

  for (guint i = 0; i < property_count; ++i) {
    GParamSpec* pspec = properties[i];
    const bool readable = pspec->flags & G_PARAM_READABLE;
    const bool writable = pspec->flags & G_PARAM_WRITABLE;
    const bool construct = pspec->flags & G_PARAM_CONSTRUCT;
    const bool construct_only = pspec->flags & G_PARAM_CONSTRUCT_ONLY;
    const bool secret = isSensitivePropertyName(safe_string(pspec->name));
    const bool runtime_writable = writable && !construct_only;
    const RuntimeControlApplyMode apply_mode = propertyApplyMode(pspec);
    const bool live_writable = runtime_writable && apply_mode == RuntimeControlApplyMode::Live;
    std::string minimum_value;
    std::string maximum_value;
    populate_range_metadata(pspec, &minimum_value, &maximum_value);
    result.push_back({
        .name = safe_string(pspec->name),
        .type_name = safe_string(g_type_name(G_PARAM_SPEC_VALUE_TYPE(pspec))),
        .nick = safe_string(g_param_spec_get_nick(pspec)),
        .blurb = safe_string(g_param_spec_get_blurb(pspec)),
        .serialized_value = readable ? redactSensitiveValueForDisplay(
                                           safe_string(pspec->name), serialize_property_value(G_OBJECT(element), pspec))
                                     : "",
        .default_value = redactSensitiveValueForDisplay(safe_string(pspec->name), serialize_default_value(pspec)),
        .minimum_value = std::move(minimum_value),
        .maximum_value = std::move(maximum_value),
        .enum_values = enum_values(G_PARAM_SPEC_VALUE_TYPE(pspec)),
        .control_kind = propertyControlKind(G_PARAM_SPEC_VALUE_TYPE(pspec)),
        .apply_mode = apply_mode,
        .readable = readable,
        .writable = writable,
        .runtime_writable = runtime_writable,
        .live_writable = live_writable,
        .construct = construct,
        .construct_only = construct_only,
        .secret = secret,
        .unsafe = construct_only || (runtime_writable && apply_mode == RuntimeControlApplyMode::Restart),
        .advanced = !live_writable,
        .flags = G_TYPE_IS_FLAGS(G_PARAM_SPEC_VALUE_TYPE(pspec)),
    });
  }

  g_free(properties);
  std::sort(result.begin(), result.end(), [](const GstPropertyInfo& lhs, const GstPropertyInfo& rhs) {
    return lhs.name < rhs.name;
  });
  return result;
}

absl::Status setElementPropertyFromString(
    GstElement* element,
    const std::string& property_name,
    const std::string& value) {
  if (!element) {
    return absl::InvalidArgumentError("Cannot set property on a null GstElement");
  }
  auto state_or = effective_element_state(element);
  if (!state_or.ok()) {
    return state_or.status();
  }
  return set_element_property_from_string_in_state(element, property_name, value, *state_or);
}

std::vector<GstElementInfo> listElementTree(GstElement* root) {
  std::vector<GstElementInfo> result;
  collect_element_tree(root, "", &result);
  std::sort(result.begin(), result.end(), [](const GstElementInfo& lhs, const GstElementInfo& rhs) {
    return lhs.path < rhs.path;
  });
  return result;
}

} // namespace hm::pipeline
