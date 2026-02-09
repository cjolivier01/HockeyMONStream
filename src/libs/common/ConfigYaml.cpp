#include "hstream/src/libs/common/ConfigYaml.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace hm {
namespace utils {

// Helper for overloaded lambdas.
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void set_config_from_yaml(const YAML::Node& yaml, const ConfigLocator& locator, bool quiet) {
  if (!yaml.IsDefined()) {
    return;
  }
  for (const auto& it : yaml) {
    std::string key = it.first.as<std::string>();
    const YAML::Node& value = it.second;
    bool ignored = !!locator.ignored.count(key);

    auto loc = locator.locators.find(key);
    auto loc_char = locator.char_array_locators.find(key);
    auto loc_char_ptr = locator.char_ptr_locators.find(key);
    auto loc_int = locator.int_array_locators.find(key);

    // If the key isn't found, try replacing '-' with '_' and check again.
    if (loc == locator.locators.end() && loc_char == locator.char_array_locators.end() &&
        loc_char_ptr == locator.char_ptr_locators.end() && loc_int == locator.int_array_locators.end()) {
      std::replace(key.begin(), key.end(), '-', '_');
      loc = locator.locators.find(key);
      loc_char = locator.char_array_locators.find(key);
      loc_char_ptr = locator.char_ptr_locators.find(key);
      loc_int = locator.int_array_locators.find(key);
      ignored |= !!locator.ignored.count(key);
      assert(
          (loc == locator.locators.end() ? 1 : 0) + (loc_char == locator.char_array_locators.end() ? 1 : 0) +
              (loc_int == locator.int_array_locators.end() ? 1 : 0) >=
          2);
    }

    if (loc != locator.locators.end()) {
      assert(!ignored);
      std::visit(
          overloaded{// Handle pointer types (the original alternatives).
                     [&](auto* target) {
                       using T = std::decay_t<decltype(*target)>;
                       try {
                         *target = value.as<T>();
                       } catch (const std::exception& e) {
                         std::cerr << "Error setting value for key '" << key << "': " << e.what() << '\n';
                       }
                     },
                     // Handle enum locators.
                     [&](const ConfigEnumLocator& enumLoc) {
                       try {
                         int int_val = value.as<int>();
                         enumLoc.assign(enumLoc.ptr, int_val);
                       } catch (const std::exception& e) {
                         std::cerr << "Error setting enum value for key '" << key << "': " << e.what() << '\n';
                       }
                     }},
          loc->second);
    } else if (loc_char != locator.char_array_locators.end()) {
      assert(!ignored);
      ::strncpy(loc_char->second.first, value.as<std::string>().c_str(), loc_char->second.second);
    } else if (loc_char_ptr != locator.char_ptr_locators.end()) {
      assert(!ignored);
      *loc_char_ptr->second.first = strdup(value.as<std::string>().c_str());
    } else if (loc_int != locator.int_array_locators.end()) {
      assert(!ignored);
      // Parse a comma-separated list of integers.
      std::string csv = value.as<std::string>();
      std::istringstream ss(csv);
      std::string token;
      int* array_ptr = loc_int->second.first;
      size_t max_count = loc_int->second.second;
      size_t count = 0;
      while (std::getline(ss, token, ',') && count < max_count) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        try {
          array_ptr[count] = std::stoi(token);
        } catch (const std::exception& e) {
          std::cerr << "Error converting token '" << token << "' to int for key '" << key << "': " << e.what() << '\n';
        }
        count++;
      }
    } else if (!ignored) {
      if (!quiet) {
        std::cerr << "Warning: Unrecognized key in YAML: " << key << '\n';
      }
    }
  }
}

int config_yaml_test_main() {
  // Example configuration struct and locator.
  MyConfig config = {0, "", 0.0f, false, {0}};
  ConfigLocator locator;

  // Set locators using the macros.
  SET_LOCATOR(locator, config, some_value_yah);
  SET_LOCATOR(locator, config, another_value);
  SET_LOCATOR(locator, config, another_value_still);
  SET_LOCATOR(locator, config, a_bool_value_teh);
  SET_LOCATOR_INTS(locator, config, int_array_example);

  // Path to the YAML configuration file.
  std::string cfg_file_path = "config.yaml";

  try {
    YAML::Node yaml = YAML::LoadFile(cfg_file_path);
    set_config_from_yaml(yaml, locator);
  } catch (const std::exception& e) {
    std::cerr << "Error loading YAML file: " << e.what() << '\n';
    return 1;
  }

  // Output the loaded configuration.
  std::cout << "some_value_yah: " << config.some_value_yah << '\n';
  std::cout << "another_value: " << config.another_value << '\n';
  std::cout << "another_value_still: " << config.another_value_still << '\n';
  std::cout << "a_bool_value_teh: " << (config.a_bool_value_teh ? "true" : "false") << '\n';

  std::cout << "int_array_example: ";
  for (int i = 0; i < 10; i++) {
    std::cout << config.int_array_example[i] << " ";
  }
  std::cout << '\n';

  return 0;
}

} // namespace utils
} // namespace hm
