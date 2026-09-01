#include "hockeymon/csrc/play_tracker/LivingBoxImpl.h"

#include <cmath>
#include <iostream>
#include <optional>

namespace {

bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.0001f;
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  hm::play_tracker::AllLivingBoxConfig config;
  config.arena_box = hm::BBox(0, 0, 1000, 500);
  config.max_width = 1000;
  config.max_height = 500;
  config.max_speed_x = 100;
  config.max_speed_y = 100;
  config.max_accel_x = 100;
  config.max_accel_y = 100;
  config.resizing_enabled = false;
  config.time_to_dest_speed_limit_frames = 0;

  hm::play_tracker::LivingBox box("test", hm::BBox(450, 200, 550, 300), config);
  box.adjust_speed(-12.0f, std::nullopt, 1.0f, std::nullopt);
  box.begin_stop_delay(4, std::nullopt);

  const hm::BBox destination(0, 200, 100, 300);
  box.forward(destination);
  auto translation_state = [&box]() -> const hm::play_tracker::TranslationState& {
    return static_cast<const hm::play_tracker::TranslatingBox&>(box).get_state();
  };
  bool ok = true;
  ok &= expect(
      near(translation_state().current_speed_x, -9.0f), "The first braking frame should decelerate linearly");
  ok &= expect(
      translation_state().stop_delay_x_counter == 1, "The first braking frame should advance the deadline");

  box.begin_stop_delay(4, std::nullopt);
  ok &= expect(
      translation_state().stop_delay_x_counter == 1 && near(translation_state().stop_decel_x, 3.0f),
      "A repeated braking request must preserve the active deadline and deceleration");

  for (int frame = 1; frame < 4; ++frame) {
    box.begin_stop_delay(4, std::nullopt);
    box.forward(destination);
  }
  ok &= expect(
      near(translation_state().current_speed_x, 0.0f) && translation_state().stop_delay_x == 0,
      "Repeated overshoot requests must still stop the box on the original deadline");
  return ok ? 0 : 1;
}
