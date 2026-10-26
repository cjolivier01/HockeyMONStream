#include "hstream/src/libs/program/ToneControls.h"

#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Command(const char* id, double value, const char* property, const char* wire_value) {
  for (bool high_bit : {false, true}) {
    const auto command = hm::program::BuildTonePropertyCommand(id, value, high_bit);
    Check(command.ok(), "Valid presentation value rejected");
    Check(command->element == (high_bit ? "hmstitcher0" : "playcropper0"), "Wrong active precision target");
    Check(command->property == property && command->value == wire_value, "Legacy property/value changed");
  }
}
struct CommaDecimal : std::numpunct<char> {
  char do_decimal_point() const override {
    return ',';
  }
};
} // namespace

int main() {
  try {
    Command("Bring_Up_Shadows", 0, "shadow-lift", "0");
    Command("Bring_Up_Shadows", 100, "shadow-lift", "100");
    Command("Lift_Shadow_Black_Point", 0, "shadow-lift-black-point", "0");
    Command("Lift_Shadow_Black_Point", 1, "shadow-lift-black-point", "1");
    Command("Exposure_x100", 0, "exposure", "0.00");
    Command("Exposure_x100", 1, "exposure", "0.01");
    Command("Exposure_x100", 9, "exposure", "0.09");
    Command("Exposure_x100", 10, "exposure", "0.10");
    Command("Exposure_x100", 29, "exposure", "0.29");
    Command("Exposure_x100", 99, "exposure", "0.99");
    Command("Exposure_x100", 100, "exposure", "1.00");
    Command("Exposure_x100", 130, "exposure", "1.30");
    const auto previous_locale = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    Command("Exposure_x100", 119, "exposure", "1.19");
    std::locale::global(previous_locale);
    for (const auto& spec : hm::program::kToneControls) {
      Check(hm::program::FindToneControl(spec.id) == &spec, "Control lookup lost its authoritative specification");
      for (double invalid :
           {-1.0,
            double(spec.maximum + 1),
            0.5,
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        for (bool high_bit : {false, true})
          Check(!hm::program::BuildTonePropertyCommand(spec.id, invalid, high_bit).ok(), "Invalid value accepted");
      }
    }
    for (const char* unknown : {"", "exposure_x100", "Exposure_x100;other", "exposure", "runtime-tuning-config-file"}) {
      Check(hm::program::FindToneControl(unknown) == nullptr, "Unknown ID matched a tone control");
      Check(!hm::program::BuildTonePropertyCommand(unknown, 1, false).ok(), "Unknown ID accepted");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
