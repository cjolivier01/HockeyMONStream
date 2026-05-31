#include "hstream/src/apps/apps-common/gst_plugin_properties.h"

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <string>

namespace {

bool expect_true(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  YAML::Node config = YAML::Load(R"yaml(
properties:
  silent: true
  sleep-time: 2500
private-properties:
  one-pass-mode: true
  runtime-output-max-width: 3840
)yaml");

  hm::gst::PluginProperties properties;
  if (!hm::gst::append_plugin_properties_from_yaml(config, "properties", &properties)) {
    std::cerr << "Expected valid public properties to parse\n";
    return 1;
  }
  if (!expect_true(properties.size() == 2, "Expected two public properties")) {
    return 1;
  }

  GstElement* identity = gst_element_factory_make("identity", "identity-under-test");
  if (!expect_true(identity != nullptr, "Could not create identity element")) {
    return 1;
  }

  if (!hm::gst::apply_plugin_properties(G_OBJECT(identity), properties)) {
    std::cerr << "Failed to apply identity properties\n";
    gst_object_unref(identity);
    return 1;
  }

  gboolean silent = FALSE;
  guint sleep_time = 0;
  g_object_get(G_OBJECT(identity), "silent", &silent, "sleep-time", &sleep_time, NULL);
  gst_object_unref(identity);

  if (!expect_true(silent == TRUE, "Expected silent=true")) {
    return 1;
  }
  if (!expect_true(sleep_time == 2500, "Expected sleep-time=2500")) {
    return 1;
  }

  hm::gst::PluginProperties private_properties;
  if (!hm::gst::append_plugin_private_properties_from_yaml(config, "private-properties", &private_properties)) {
    std::cerr << "Expected valid private properties to parse\n";
    return 1;
  }
  const std::string serialized = hm::gst::serialize_plugin_properties(private_properties, "show=1");
  if (!expect_true(
          serialized == "show=1;one-pass-mode=true;runtime-output-max-width=3840",
          "Unexpected private property serialization")) {
    std::cerr << serialized << '\n';
    return 1;
  }

  const YAML::Node malformed = YAML::Load(R"yaml(
properties:
  - silent=true
)yaml");
  hm::gst::PluginProperties ignored_properties;
  if (hm::gst::append_plugin_properties_from_yaml(malformed, "properties", &ignored_properties)) {
    std::cerr << "Expected malformed property group to fail\n";
    return 1;
  }

  const YAML::Node malformed_private = YAML::Load(R"yaml(
private-properties:
  good-key: bad=value
)yaml");
  hm::gst::PluginProperties ignored_private_properties;
  if (hm::gst::append_plugin_private_properties_from_yaml(
          malformed_private, "private-properties", &ignored_private_properties)) {
    std::cerr << "Expected private property delimiter to fail\n";
    return 1;
  }

  identity = gst_element_factory_make("identity", "identity-invalid-properties");
  if (!expect_true(identity != nullptr, "Could not create second identity element")) {
    return 1;
  }
  if (hm::gst::apply_plugin_properties(G_OBJECT(identity), {{"sleep-time", "-1"}})) {
    std::cerr << "Expected negative unsigned property to fail\n";
    gst_object_unref(identity);
    return 1;
  }
  if (hm::gst::apply_plugin_properties(G_OBJECT(identity), {{"sleep-time", "12ms"}})) {
    std::cerr << "Expected partial numeric property to fail\n";
    gst_object_unref(identity);
    return 1;
  }
  if (hm::gst::apply_plugin_properties(G_OBJECT(identity), {{"silent", "1junk"}})) {
    std::cerr << "Expected partial boolean property to fail\n";
    gst_object_unref(identity);
    return 1;
  }
  gst_object_unref(identity);

  return 0;
}
