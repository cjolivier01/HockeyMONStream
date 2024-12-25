#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <string>
#include <map>
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
  std::map<std::string, ConfigValueLocator> locators;
};

#define SET_LOCATOR(config_locator$, cfg_struct$, member$) \
    config_locator$.locators[#member$] = &((cfg_struct$).member$)

void set_config_from_yaml(const YAML::Node& yaml, const ConfigLocator& locator);

#define STRNLEN(__s$) (sizeof(__s$) / sizeof((__s$)[0]))

} // namespace utils
} // namespace hm
