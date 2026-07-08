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
  std::string name;
  std::string type_name;
  std::string factory_name;
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

} // namespace hm::pipeline
