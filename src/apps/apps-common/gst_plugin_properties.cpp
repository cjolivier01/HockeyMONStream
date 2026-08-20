#include "hstream/src/apps/apps-common/gst_plugin_properties.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace hm::gst {
namespace {

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

gint64 parse_int64_exact(const std::string& raw_value);

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

bool parse_strict_bool(const std::string& value) {
  const std::string lowered = to_lower(trim(value));
  if (lowered == "1" || lowered == "true") {
    return true;
  }
  if (lowered == "0" || lowered == "false") {
    return false;
  }
  throw std::invalid_argument("expected true, false, 1, or 0");
}

std::string yaml_scalar_to_string(const YAML::Node& value) {
  if (value.IsScalar()) {
    return value.as<std::string>();
  }
  YAML::Emitter emitter;
  emitter << value;
  return std::string(emitter.c_str());
}

gint64 parse_int64_exact(const std::string& raw_value) {
  const std::string value = trim(raw_value);
  if (value.empty()) {
    throw std::invalid_argument("empty integer");
  }
  std::size_t end = 0;
  const long long parsed = std::stoll(value, &end, 0);
  if (end != value.size()) {
    throw std::invalid_argument("integer contains trailing characters");
  }
  return static_cast<gint64>(parsed);
}

guint64 parse_uint64_exact(const std::string& raw_value) {
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
  return static_cast<guint64>(parsed);
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

void require_signed_range(gint64 value, gint64 minimum, gint64 maximum) {
  if (value < minimum || value > maximum) {
    throw std::out_of_range("integer value outside property range");
  }
}

void require_unsigned_range(guint64 value, guint64 minimum, guint64 maximum) {
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
    const gint64 parsed = parse_int64_exact(raw_value);
    require_signed_range(parsed, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    return static_cast<int>(parsed);
  }
  const int result = enum_value->value;
  g_type_class_unref(enum_class);
  return result;
}

gboolean set_typed_value(GObject* object, GParamSpec* pspec, const std::string& raw_value) {
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));

  try {
    const GType value_type = G_VALUE_TYPE(&value);
    if (value_type == G_TYPE_BOOLEAN) {
      const bool strict_boolean = std::string(pspec->name) == "shadow-lift-black-point";
      g_value_set_boolean(&value, strict_boolean ? parse_strict_bool(raw_value) : parse_bool(raw_value));
    } else if (value_type == G_TYPE_INT) {
      const gint64 parsed = parse_int64_exact(raw_value);
      GParamSpecInt* int_spec = G_PARAM_SPEC_INT(pspec);
      require_signed_range(parsed, int_spec->minimum, int_spec->maximum);
      g_value_set_int(&value, static_cast<gint>(parsed));
    } else if (value_type == G_TYPE_UINT) {
      const guint64 parsed = parse_uint64_exact(raw_value);
      GParamSpecUInt* uint_spec = G_PARAM_SPEC_UINT(pspec);
      require_unsigned_range(parsed, uint_spec->minimum, uint_spec->maximum);
      g_value_set_uint(&value, static_cast<guint>(parsed));
    } else if (value_type == G_TYPE_LONG) {
      const gint64 parsed = parse_int64_exact(raw_value);
      GParamSpecLong* long_spec = G_PARAM_SPEC_LONG(pspec);
      require_signed_range(parsed, long_spec->minimum, long_spec->maximum);
      g_value_set_long(&value, static_cast<glong>(parsed));
    } else if (value_type == G_TYPE_ULONG) {
      const guint64 parsed = parse_uint64_exact(raw_value);
      GParamSpecULong* ulong_spec = G_PARAM_SPEC_ULONG(pspec);
      require_unsigned_range(parsed, ulong_spec->minimum, ulong_spec->maximum);
      g_value_set_ulong(&value, static_cast<gulong>(parsed));
    } else if (value_type == G_TYPE_INT64) {
      const gint64 parsed = parse_int64_exact(raw_value);
      GParamSpecInt64* int64_spec = G_PARAM_SPEC_INT64(pspec);
      require_signed_range(parsed, int64_spec->minimum, int64_spec->maximum);
      g_value_set_int64(&value, parsed);
    } else if (value_type == G_TYPE_UINT64) {
      const guint64 parsed = parse_uint64_exact(raw_value);
      GParamSpecUInt64* uint64_spec = G_PARAM_SPEC_UINT64(pspec);
      require_unsigned_range(parsed, uint64_spec->minimum, uint64_spec->maximum);
      g_value_set_uint64(&value, parsed);
    } else if (value_type == G_TYPE_FLOAT) {
      const double parsed = parse_double_exact(raw_value);
      GParamSpecFloat* float_spec = G_PARAM_SPEC_FLOAT(pspec);
      require_double_range(parsed, float_spec->minimum, float_spec->maximum);
      g_value_set_float(&value, static_cast<gfloat>(parsed));
    } else if (value_type == G_TYPE_DOUBLE) {
      const double parsed = parse_double_exact(raw_value);
      GParamSpecDouble* double_spec = G_PARAM_SPEC_DOUBLE(pspec);
      require_double_range(parsed, double_spec->minimum, double_spec->maximum);
      g_value_set_double(&value, parsed);
    } else if (value_type == G_TYPE_STRING) {
      g_value_set_string(&value, raw_value.c_str());
    } else if (G_TYPE_IS_ENUM(value_type)) {
      g_value_set_enum(&value, parse_enum_value(value_type, raw_value));
    } else {
      std::cerr << "Unsupported GObject property type for " << pspec->name << '\n';
      g_value_unset(&value);
      return FALSE;
    }
  } catch (const std::exception& ex) {
    std::cerr << "Could not parse property " << pspec->name << " value '" << raw_value << "': " << ex.what() << '\n';
    g_value_unset(&value);
    return FALSE;
  }

  if (g_param_value_validate(pspec, &value)) {
    std::cerr << "Property " << pspec->name << " value '" << raw_value << "' is outside allowed property constraints\n";
    g_value_unset(&value);
    return FALSE;
  }

  g_object_set_property(object, pspec->name, &value);
  g_value_unset(&value);
  return TRUE;
}

bool contains_any(const std::string& value, const char* chars) {
  return value.find_first_of(chars) != std::string::npos;
}

gboolean append_plugin_properties_from_yaml_impl(
    const YAML::Node& parent,
    const char* key,
    PluginProperties* properties,
    bool serialized_private_config) {
  if (!properties || !parent || !parent[key]) {
    return TRUE;
  }

  const YAML::Node node = parent[key];
  if (!node.IsMap()) {
    std::cerr << "Plugin property group '" << key << "' must be a YAML map\n";
    return FALSE;
  }

  for (const auto& item : node) {
    PluginProperty property{item.first.as<std::string>(), yaml_scalar_to_string(item.second)};
    if (serialized_private_config) {
      if (contains_any(property.name, "=;")) {
        std::cerr << "Private plugin property name '" << property.name << "' must not contain '=' or ';'\n";
        return FALSE;
      }
      if (contains_any(property.value, "=;")) {
        std::cerr << "Private plugin property value for '" << property.name << "' must not contain '=' or ';'\n";
        return FALSE;
      }
    }
    properties->push_back(std::move(property));
  }
  return TRUE;
}

} // namespace

gboolean append_plugin_properties_from_yaml(const YAML::Node& parent, const char* key, PluginProperties* properties) {
  return append_plugin_properties_from_yaml_impl(parent, key, properties, /*serialized_private_config=*/false);
}

gboolean append_plugin_private_properties_from_yaml(
    const YAML::Node& parent,
    const char* key,
    PluginProperties* properties) {
  return append_plugin_properties_from_yaml_impl(parent, key, properties, /*serialized_private_config=*/true);
}

std::string serialize_plugin_properties(const PluginProperties& properties, const std::string& prefix) {
  std::ostringstream out;
  out << prefix;
  for (const PluginProperty& property : properties) {
    if (property.name.empty()) {
      continue;
    }
    if (out.tellp() > 0) {
      out << ';';
    }
    out << property.name << '=' << property.value;
  }
  return out.str();
}

gboolean set_plugin_property_from_string(GObject* object, const PluginProperty& property) {
  if (!object) {
    std::cerr << "Cannot set property '" << property.name << "' on a null GObject\n";
    return FALSE;
  }
  if (property.name.empty()) {
    std::cerr << "Cannot set a plugin property with an empty name\n";
    return FALSE;
  }

  GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), property.name.c_str());
  if (!pspec) {
    std::cerr << "Property '" << property.name << "' does not exist on " << G_OBJECT_TYPE_NAME(object) << '\n';
    return FALSE;
  }
  if (!(pspec->flags & G_PARAM_WRITABLE)) {
    std::cerr << "Property '" << property.name << "' is not writable on " << G_OBJECT_TYPE_NAME(object) << '\n';
    return FALSE;
  }

  return set_typed_value(object, pspec, property.value);
}

gboolean apply_plugin_properties(GObject* object, const PluginProperties& properties) {
  for (const PluginProperty& property : properties) {
    if (!set_plugin_property_from_string(object, property)) {
      return FALSE;
    }
  }
  return TRUE;
}

} // namespace hm::gst
