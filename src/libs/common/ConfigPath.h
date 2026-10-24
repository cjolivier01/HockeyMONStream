#pragma once

#include <optional>
#include <string>
#include <type_traits>

#include "yaml-cpp/yaml.h"

namespace hm {
bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null);

// Dot-separated map keys, optionally followed by :index to select a sequence
// item. A bare :index selects from the current sequence. Lookups never modify n.
std::optional<YAML::Node> get_node(const YAML::Node& n, const std::string& dot_string);

template <typename T>
inline T get_node_as(const YAML::Node& n, const std::string& dot_string, const T& dflt) {
  std::optional<YAML::Node> o_n = get_node(n, dot_string);
  if (!o_n.has_value()) {
    return dflt;
  }
  return o_n.value().as<T>();
}

YAML::Node set_node_value(YAML::Node node, const std::string& dot_string, const std::string& value);

template <typename T>
inline T get_node_value(const YAML::Node& n, const std::string& dot_string, const T& default_value) {
  std::optional<YAML::Node> o_n = hm::get_node(n, dot_string);
  if (!o_n.has_value()) {
    return default_value;
  }
  if constexpr (std::is_same_v<T, bool>) {
    // We serialize bools as int so that we don`'t have to write true/false all
    // the time
    return !!o_n.value().as<int>();
  }
  return o_n.value().as<T>();
}

} // namespace hm
