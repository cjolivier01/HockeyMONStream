#pragma once

#include "hstream/src/libs/pipeline_controller/RuntimeTypes.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include <gst/gst.h>

#include <string>
#include <vector>

namespace hm::pipeline {

struct GstEnumValueInfo {
  std::string name;
  std::string nick;
  int value{0};
};

struct GstPropertyInfo {
  std::string name;
  std::string type_name;
  std::string nick;
  std::string blurb;
  std::string serialized_value;
  std::string default_value;
  std::string minimum_value;
  std::string maximum_value;
  std::vector<GstEnumValueInfo> enum_values;
  RuntimeControlKind control_kind{RuntimeControlKind::Text};
  RuntimeControlApplyMode apply_mode{RuntimeControlApplyMode::Restart};
  bool readable{false};
  bool writable{false};
  bool runtime_writable{false};
  bool live_writable{false};
  bool construct{false};
  bool construct_only{false};
  bool secret{false};
  bool unsafe{false};
  bool advanced{false};
  bool flags{false};
};

struct GstElementInfo {
  std::string path;
  std::string parent_path;
  std::string name;
  std::string type_name;
  std::string factory_name;
  std::string state_name;
  bool bin{false};
};

struct GstConnectionInfo {
  std::string source_path;
  std::string source_pad;
  std::string sink_path;
  std::string sink_pad;
};

struct GstPipelineGraphInfo {
  std::vector<GstElementInfo> elements;
  std::vector<GstConnectionInfo> connections;
};

RuntimeControlApplyMode propertyApplyMode(GParamSpec* pspec);

bool propertyMutableInCurrentState(GParamSpec* pspec, GstState state);

RuntimeControlKind propertyControlKind(GType value_type);

bool isSensitivePropertyName(const std::string& property_name);

std::string redactSensitiveValueForDisplay(const std::string& property_name, const std::string& value);

absl::StatusOr<std::vector<GstPropertyInfo>> listElementProperties(GstElement* element);

absl::Status setElementPropertyFromString(
    GstElement* element,
    const std::string& property_name,
    const std::string& value);

std::vector<GstElementInfo> listElementTree(GstElement* root);

GstPipelineGraphInfo inspectPipelineGraph(GstElement* root);

// Returns a referenced element whose exact structured path matches `path`.
// The caller owns the returned reference. Paths come from listElementTree()
// or inspectPipelineGraph(); arbitrary recursive name lookup is intentionally
// avoided because separate bins may contain children with the same name.
GstElement* findElementByPath(GstElement* root, const std::string& path);

} // namespace hm::pipeline
