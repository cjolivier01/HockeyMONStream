#pragma once

#include <vector>

// Shared presentation ranges and runtime-unit conversion for Program and
// Experiment controls. Each surface owns its own widget/parameter state.
struct CameraSliderSpec {
  const char* id;
  const char* label;
  int minimum;
  int maximum;
  int default_value;
  const char* runtime_key{nullptr};
  int divisor{1};
};

template <typename Defaults>
std::vector<CameraSliderSpec> tracking_control_specs(Defaults default_value) {
  return {
      {"Zoom_In_Aggressiveness",
       "Zoom-in aggressiveness",
       0,
       100,
       default_value("Zoom_In_Aggressiveness"),
       "zoom-in-aggressiveness",
       1},
      {"Stop_Direction_Change_Delay_Frames",
       "Stop direction-change delay frames",
       0,
       60,
       default_value("Stop_Direction_Change_Delay_Frames"),
       "stop-translation-on-dir-change-delay",
       1},
      {"Cancel_Stop_On_Opposite_Direction",
       "Cancel stop on opposite direction",
       0,
       1,
       default_value("Cancel_Stop_On_Opposite_Direction"),
       "cancel-stop-on-opposite-dir",
       1},
      {"Stop_Cancel_Hysteresis_Frames",
       "Stop cancel hysteresis frames",
       0,
       10,
       default_value("Stop_Cancel_Hysteresis_Frames"),
       "cancel-stop-hysteresis-frames",
       1},
      {"Stop_Delay_Cooldown_Frames",
       "Stop-delay cooldown frames",
       0,
       30,
       default_value("Stop_Delay_Cooldown_Frames"),
       "stop-delay-cooldown-frames",
       1},
      {"Time_To_Dest_Speed_Limit_Frames",
       "Time-to-destination speed limit frames",
       0,
       120,
       default_value("Time_To_Dest_Speed_Limit_Frames"),
       "time-to-dest-speed-limit-frames",
       1},
      {"Apply_To_Fast_Box", "Apply to fast box", 0, 1, default_value("Apply_To_Fast_Box")},
      {"Apply_To_Follower_Box", "Apply to follower box", 0, 1, default_value("Apply_To_Follower_Box")},
  };
}

template <typename Defaults>
std::vector<CameraSliderSpec> motion_control_specs(Defaults default_value) {
  return {
      {"Overshoot_Stop_Delay_Frames",
       "Overshoot stop-delay frames",
       0,
       60,
       default_value("Overshoot_Stop_Delay_Frames"),
       "overshoot-stop-delay-count",
       1},
      {"Post_Nonstop_Stop_Delay_Frames",
       "Post-nonstop stop-delay frames",
       0,
       60,
       default_value("Post_Nonstop_Stop_Delay_Frames"),
       "post-nonstop-stop-delay-count",
       1},
      {"Overshoot_Speed_Ratio_x100",
       "Overshoot speed ratio x100",
       0,
       200,
       default_value("Overshoot_Speed_Ratio_x100"),
       "overshoot-scale-speed-ratio",
       100},
      {"Max_Speed_X_x10",
       "Max speed X override x10 (0 = configured)",
       0,
       2000,
       default_value("Max_Speed_X_x10"),
       "max-speed-x",
       10},
      {"Max_Speed_Y_x10",
       "Max speed Y override x10 (0 = configured)",
       0,
       2000,
       default_value("Max_Speed_Y_x10"),
       "max-speed-y",
       10},
      {"Max_Accel_X_x10",
       "Max accel X override x10 (0 = configured)",
       0,
       1000,
       default_value("Max_Accel_X_x10"),
       "max-accel-x",
       10},
      {"Max_Accel_Y_x10",
       "Max accel Y override x10 (0 = configured)",
       0,
       1000,
       default_value("Max_Accel_Y_x10"),
       "max-accel-y",
       10},
  };
}
