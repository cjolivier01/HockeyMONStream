#include "hstream/src/libs/gpu/vaapi/VaElement.h"

#include <string>

namespace hm::gpu {
// Each VA factory is bound to one render node. A display context alone cannot
// move a factory to another device; select by the factory's device-path.
GstElement* MakeVaElement(const char* suffix, const std::filesystem::path& render) {
  auto* features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  GstElement* selected = nullptr;
  for (auto* item = features; item && !selected; item = item->next) {
    auto* factory = GST_ELEMENT_FACTORY(item->data);
    const char* plugin = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
    const std::string name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    const size_t length = std::char_traits<char>::length(suffix);
    if (!plugin || std::string(plugin) != "va" || name.size() < length ||
        name.compare(name.size() - length, length, suffix) != 0)
      continue;
    auto* element = gst_element_factory_create(factory, nullptr);
    if (!element)
      continue;
    gchar* device = nullptr;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device-path"))
      g_object_get(element, "device-path", &device, nullptr);
    std::error_code error;
    if (device && std::filesystem::canonical(device, error) == render && !error)
      selected = element;
    else
      gst_object_unref(element);
    g_free(device);
  }
  gst_plugin_feature_list_free(features);
  return selected;
}

} // namespace hm::gpu
