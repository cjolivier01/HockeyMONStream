#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <algorithm> // for std::replace
#include <cassert>
#include <cstring> // for strncpy
#include <map>
#include <set>
#include <sstream> // for parsing CSV string
#include <string>
#include <variant>

using ConfigValue = std::variant<guint, float, std::string, gboolean>;
using ConfigValueLocator = std::variant<guint*, size_t*, float*, std::string*, gboolean*, bool*>;

namespace hm {
namespace utils {

// Example configuration struct including an int array.
struct MyConfig {
  guint some_value_yah;
  std::string another_value;
  float another_value_still;
  gboolean a_bool_value_teh;
  int int_array_example[10]; // sample int array member
};

// Modified locator structure now includes a mapping for int arrays.
struct ConfigLocator {
  std::set<std::string> ignored;
  std::map<std::string, ConfigValueLocator> locators;
  std::map<std::string, std::pair<char*, size_t>> char_array_locators;
  std::map<std::string, std::pair<int*, size_t>> int_array_locators; // NEW: for int arrays
};

// Original macros remain the same…
#define SET_LOCATOR(config_locator$, cfg_struct$, member$)         \
  do {                                                             \
    assert(!config_locator$.locators.count(#member$));             \
    config_locator$.locators[#member$] = &((cfg_struct$).member$); \
  } while (false)

#define SET_LOCATOR_CHARS(config_locator$, cfg_struct$, member$)                                        \
  do {                                                                                                  \
    assert(!config_locator$.locators.count(#member$));                                                  \
    constexpr size_t var_size$ = sizeof((cfg_struct$).member$);                                         \
    config_locator$.char_array_locators[#member$] = std::make_pair(((cfg_struct$).member$), var_size$); \
  } while (false)

// New macro to support int arrays.
#define SET_LOCATOR_INTS(config_locator$, cfg_struct$, member$)                                         \
  do {                                                                                                  \
    assert(!config_locator$.int_array_locators.count(#member$));                                        \
    constexpr size_t var_count$ = sizeof((cfg_struct$).member$) / sizeof(int);                          \
    config_locator$.int_array_locators[#member$] = std::make_pair(((cfg_struct$).member$), var_count$); \
  } while (false)

void set_config_from_yaml(const YAML::Node& yaml, const ConfigLocator& locator);

#define STRNLEN(__s$) (sizeof(__s$) / sizeof((__s$)[0]))

template <typename T>
bool parse_chracter_buffer(
    T& config_file_dest_buffer,
    const YAML::Node& yaml_node,
    const char* key,
    const std::string& default_prefix_path = "") {
  if (yaml_node[key]) {
    std::string config_file = yaml_node[key].as<std::string>();
    if (!config_file.empty()) {
      if (config_file[0] != '/' && !default_prefix_path.empty()) {
        config_file = default_prefix_path + '/' + config_file;
      }
      strncpy(config_file_dest_buffer, config_file.c_str(), STRNLEN(config_file_dest_buffer) - 1);
      return true;
    }
  }
  return false;
}

} // namespace utils
} // namespace hm
