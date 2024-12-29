#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <map>
#include <set>
#include <string>
#include <variant>

using ConfigValue = std::variant<guint, float, std::string, gboolean>;
using ConfigValueLocator = std::variant<guint*, size_t*, float*, std::string*, gboolean*, bool*>;

namespace hm {
namespace utils {

struct MyConfig {
  guint some_value_yah;
  std::string another_value;
  float another_value_still;
  gboolean a_bool_value_teh;
};

struct ConfigLocator {
  std::set<std::string> ignored;
  std::map<std::string, ConfigValueLocator> locators;
  std::map<std::string, std::pair<char*, size_t>> char_array_locators;
};

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

void set_config_from_yaml(const YAML::Node& yaml, const ConfigLocator& locator);

#define STRNLEN(__s$) (sizeof(__s$) / sizeof((__s$)[0]))

} // namespace utils
} // namespace hm
