#include "ConfigYaml.h"

#include <cassert>
#include <iostream>
#include <string>

#include <string.h>

namespace hm {
namespace utils {

void set_config_from_yaml(const YAML::Node& yaml, const ConfigLocator& locator) {
  for (const auto& it : yaml) {
    std::string key = it.first.as<std::string>();
    const YAML::Node& value = it.second;
    bool ignored = !!locator.ignored.count(key);
    auto loc = locator.locators.find(key);
    auto loc_char = locator.char_array_locators.find(key);
    // Check not in both
    assert(loc == locator.locators.end() || loc_char == locator.char_array_locators.end());
    if (loc == locator.locators.end() && loc_char == locator.char_array_locators.end()) {
      std::replace(key.begin(), key.end(), '-', '_');
      loc = locator.locators.find(key);
      loc_char = locator.char_array_locators.find(key);
      ignored |= !!locator.ignored.count(key);
      // Check not in both
      assert(loc == locator.locators.end() || loc_char == locator.char_array_locators.end());
    }
    if (loc != locator.locators.end()) {
      assert(!ignored);
      std::visit(
          [&](auto&& target) {
            using T = std::decay_t<decltype(*target)>;
            try {
              *target = value.as<T>();
            } catch (const std::exception& e) {
              std::cerr << "Error setting value for key '" << key << "': " << e.what() << '\n';
            }
          },
          loc->second);
    } else if (loc_char != locator.char_array_locators.end()) {
      assert(!ignored);
      ::strncpy(loc_char->second.first, value.as<std::string>().c_str(), loc_char->second.second);
    } else if (!ignored) {
      std::cerr << "Warning: Unrecognized key in YAML: " << key << '\n';
    }
  }
}

int config_yaml_test_main() {
  // Example configuration struct and locator
  MyConfig config = {0, "", 0.0f, false};
  ConfigLocator locator;

  // Set locators using the macro
  SET_LOCATOR(locator, config, some_value_yah);
  SET_LOCATOR(locator, config, another_value);
  SET_LOCATOR(locator, config, another_value_still);
  SET_LOCATOR(locator, config, a_bool_value_teh);

  // Path to the YAML configuration file
  std::string cfg_file_path = "config.yaml";

  try {
    YAML::Node yaml = YAML::LoadFile(cfg_file_path);
    set_config_from_yaml(yaml, locator);
  } catch (const std::exception& e) {
    std::cerr << "Error loading YAML file: " << e.what() << '\n';
    return 1;
  }

  // Output the loaded configuration
  std::cout << "some_value_yah: " << config.some_value_yah << '\n';
  std::cout << "another_value: " << config.another_value << '\n';
  std::cout << "another_value_still: " << config.another_value_still << '\n';
  std::cout << "a_bool_value_teh: " << (config.a_bool_value_teh ? "true" : "false") << '\n';

  return 0;
}

} // namespace utils
} // namespace hm
