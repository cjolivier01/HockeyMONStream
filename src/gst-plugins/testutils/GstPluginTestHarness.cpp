#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace hm::gst::test {
namespace {

std::vector<std::filesystem::path>& loaded_plugin_paths() {
  static std::vector<std::filesystem::path> paths;
  return paths;
}

std::filesystem::path runfile_path(const std::string& workspace_relative_path) {
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (test_srcdir && test_workspace) {
    std::filesystem::path candidate = std::filesystem::path(test_srcdir) / test_workspace / workspace_relative_path;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  std::filesystem::path direct = workspace_relative_path;
  if (std::filesystem::exists(direct)) {
    return direct;
  }

  return {};
}

std::filesystem::path canonical_path(const char* path) {
  if (!path) {
    return {};
  }
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    return std::filesystem::absolute(path);
  }
  return canonical;
}

bool is_loaded_runfile_factory(GstElementFactory* factory) {
  GstPlugin* plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
  if (!plugin) {
    return false;
  }
  const std::filesystem::path factory_path = canonical_path(gst_plugin_get_filename(plugin));
  gst_object_unref(plugin);

  for (const std::filesystem::path& loaded_path : loaded_plugin_paths()) {
    if (factory_path == loaded_path) {
      return true;
    }
  }
  return false;
}

GstElementFactory* find_loaded_runfile_factory(const std::string& factory_name) {
  GstElementFactory* factory = gst_element_factory_find(factory_name.c_str());
  if (!factory) {
    std::cerr << "Missing factory: " << factory_name << '\n';
    return nullptr;
  }
  if (!is_loaded_runfile_factory(factory)) {
    std::cerr << "Factory " << factory_name << " did not come from a loaded Bazel runfile plugin\n";
    gst_object_unref(factory);
    return nullptr;
  }
  return factory;
}

} // namespace

bool load_plugin_from_runfiles(const std::string& workspace_relative_path) {
  const std::filesystem::path plugin_path = runfile_path(workspace_relative_path);
  if (plugin_path.empty()) {
    std::cerr << "Could not resolve runfile: " << workspace_relative_path << '\n';
    return false;
  }

  GError* error = nullptr;
  GstPlugin* plugin = gst_plugin_load_file(plugin_path.c_str(), &error);
  if (!plugin) {
    std::cerr << "Could not load plugin " << plugin_path << ": " << (error ? error->message : "unknown error") << '\n';
    if (error) {
      g_error_free(error);
    }
    return false;
  }
  loaded_plugin_paths().push_back(canonical_path(plugin_path.c_str()));
  gst_object_unref(plugin);
  return true;
}

bool expect_factory(const std::string& factory_name) {
  GstElementFactory* factory = find_loaded_runfile_factory(factory_name);
  if (!factory) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

bool expect_element_contract(
    const std::string& factory_name,
    const std::vector<PropertyExpectation>& properties,
    const std::vector<PadExpectation>& pads) {
  GstElementFactory* factory = find_loaded_runfile_factory(factory_name);
  if (!factory) {
    return false;
  }

  GstElement* element = gst_element_factory_create(factory, nullptr);
  gst_object_unref(factory);
  if (!element) {
    std::cerr << "Could not create element from factory: " << factory_name << '\n';
    return false;
  }

  bool ok = true;
  for (const PropertyExpectation& property : properties) {
    GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property.name.c_str());
    if (!pspec) {
      std::cerr << factory_name << " missing property: " << property.name << '\n';
      ok = false;
      continue;
    }
    if (property.value_type != G_TYPE_INVALID && G_PARAM_SPEC_VALUE_TYPE(pspec) != property.value_type) {
      std::cerr << factory_name << " property " << property.name << " has unexpected type\n";
      ok = false;
    }
    if (property.writable && !(pspec->flags & G_PARAM_WRITABLE)) {
      std::cerr << factory_name << " property " << property.name << " is not writable\n";
      ok = false;
    }
  }

  GstElementClass* element_class = GST_ELEMENT_GET_CLASS(element);
  for (const PadExpectation& pad : pads) {
    GstPadTemplate* template_pad = gst_element_class_get_pad_template(element_class, pad.name.c_str());
    if (!template_pad) {
      std::cerr << factory_name << " missing pad template: " << pad.name << '\n';
      ok = false;
      continue;
    }
    if (GST_PAD_TEMPLATE_DIRECTION(template_pad) != pad.direction ||
        GST_PAD_TEMPLATE_PRESENCE(template_pad) != pad.presence) {
      std::cerr << factory_name << " pad template " << pad.name << " has unexpected direction/presence\n";
      ok = false;
    }
  }

  gst_object_unref(element);
  return ok;
}

bool apply_and_expect_properties(GstElement* element, const PluginProperties& properties) {
  return hm::gst::apply_plugin_properties(G_OBJECT(element), properties);
}

} // namespace hm::gst::test
