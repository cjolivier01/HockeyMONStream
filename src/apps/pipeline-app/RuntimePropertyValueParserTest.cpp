#include "RuntimePropertyValueParser.h"

#include <iostream>
#include <string_view>

namespace {

bool expect_parse(std::string_view property_name, std::string_view value, bool expected_success, bool expected_value) {
  bool parsed_value = !expected_value;
  const bool success = hm::pipeline::parse_runtime_boolean_property_value(property_name, value, &parsed_value);
  if (success != expected_success || (success && parsed_value != expected_value)) {
    std::cerr << "Unexpected boolean parse result for " << property_name << "='" << value << "'\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  constexpr std::string_view kStrictProperty = "shadow-lift-black-point";
  for (std::string_view value : {"1", "true", "TRUE", " True "}) {
    if (!expect_parse(kStrictProperty, value, true, true)) {
      return 1;
    }
  }
  for (std::string_view value : {"0", "false", "FALSE", " False "}) {
    if (!expect_parse(kStrictProperty, value, true, false)) {
      return 1;
    }
  }
  for (std::string_view value : {"yes", "no", "on", "off", "2", "", " "}) {
    if (!expect_parse(kStrictProperty, value, false, false)) {
      return 1;
    }
  }
  if (hm::pipeline::parse_runtime_boolean_property_value(kStrictProperty, "true", nullptr)) {
    std::cerr << "Boolean parser should reject a null output pointer\n";
    return 1;
  }
  if (!expect_parse("legacy-boolean", "yes", true, true) || !expect_parse("legacy-boolean", "off", true, false)) {
    std::cerr << "Existing runtime boolean aliases should remain compatible\n";
    return 1;
  }
  return 0;
}
