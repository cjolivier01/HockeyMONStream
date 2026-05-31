#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <string>
#include <vector>

namespace hm::gst {

struct PluginProperty {
  std::string name;
  std::string value;
};

using PluginProperties = std::vector<PluginProperty>;

gboolean append_plugin_properties_from_yaml(const YAML::Node& parent, const char* key, PluginProperties* properties);

gboolean append_plugin_private_properties_from_yaml(
    const YAML::Node& parent,
    const char* key,
    PluginProperties* properties);

std::string serialize_plugin_properties(const PluginProperties& properties, const std::string& prefix = "");

gboolean set_plugin_property_from_string(GObject* object, const PluginProperty& property);

gboolean apply_plugin_properties(GObject* object, const PluginProperties& properties);

} // namespace hm::gst
