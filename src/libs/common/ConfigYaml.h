#pragma once

#include <string>
#include <unordered_map>
#include <variant>

using ConfigValue = std::variant<int, float, std::string, bool>;
using ConfigValueLocator = std::variant<int*, float*, std::string*, bool*>;

namespace hm {
namespace utils {

struct MyConfig {
  int some_value_yah;
  std::string another_value;
  float another_value_still;
  bool a_bool_value_teh;
};

struct ConfigLocator {
  std::unordered_map<std::string, ConfigValueLocator> locators;
};

#define SET_LOCATOR(config_locator$, cfg_struct$, member$) \
    (config_locator$).locators[#member$] = &((cfg_struct$).member$)

} // namespace utils
} // namespace hm
