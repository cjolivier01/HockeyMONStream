#pragma once

#include "hstream/src/apps/apps-common/gst_plugin_properties.h"

#include <gst/gst.h>

#include <string>
#include <vector>

namespace hm::gst::test {

struct PropertyExpectation {
  std::string name;
  GType value_type;
  bool writable;
};

struct PadExpectation {
  std::string name;
  GstPadDirection direction;
  GstPadPresence presence;
};

bool load_plugin_from_runfiles(const std::string& workspace_relative_path);

bool expect_factory(const std::string& factory_name);

bool expect_element_contract(
    const std::string& factory_name,
    const std::vector<PropertyExpectation>& properties,
    const std::vector<PadExpectation>& pads);

bool apply_and_expect_properties(GstElement* element, const PluginProperties& properties);

} // namespace hm::gst::test
