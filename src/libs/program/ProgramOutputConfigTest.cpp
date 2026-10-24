#include "hstream/src/libs/program/ProgramOutputConfig.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "Expected baseline path");
    const auto baseline = YAML::LoadFile(argv[1]);
    const auto load = [&](const std::vector<std::string>& options = {},
                          const YAML::Node& game = YAML::Node(YAML::NodeType::Map)) {
      const auto merged = hm::config::ResolveProgramConfiguration(
          baseline, YAML::Node(YAML::NodeType::Map), game, YAML::Node(YAML::NodeType::Map), options);
      Check(merged.ok(), merged.status().ToString());
      return hm::program::ResolveProgramOutputConfig(*merged);
    };
    auto result = load();
    Check(result.ok(), result.status().ToString());
    Check(
        result->width == 3840 && result->height == 2160 && result->bitrate_kbps == 20000 &&
            result->keyframe_interval == 60 && !result->realtime && result->start_ns == 0 && !result->render.no_crop &&
            result->render.fixed_edge_rotation_angle_left == 10 &&
            result->render.fixed_edge_rotation_angle_right == 10 && result->render.shadow_lift_percent == 0 &&
            !result->render.lift_shadow_black_point && result->render.exposure == 0,
        "Shared defaults changed");
    result = load({"video_out.output_width=1920,video_out.bit_rate=1234567,runtime.amd.playback_start_seconds=1.25"});
    Check(
        result.ok() && result->width == 1920 && result->height == 1080 && result->bitrate_kbps == 1235 &&
            result->start_ns == 1250000000,
        "Canonical dimension/bitrate/start mapping changed");
    result = load({"video_out.output_height=720"});
    Check(result.ok() && result->width == 1280 && result->height == 720, "Single height did not preserve aspect");
    result = load({"video_out.output_width=1920,runtime.amd.output.width=1280,runtime.amd.output.height=720"});
    Check(result.ok() && result->width == 1280 && result->height == 720, "Native output lost same-layer precedence");
    result = load({"runtime.amd.output.width=1440,video_out.output_height=720"});
    Check(
        result.ok() && result->width == 1440 && result->height == 720,
        "Aspect preservation overwrote an explicitly selected native dimension");
    auto game = YAML::Load(
        "pipeline: {hmplaycropper: {no-crop: 1, fixed-edge-rotation-angle: 3, properties: {shadow-lift: 20}}}");
    result = load({}, game);
    Check(
        result.ok() && result->render.no_crop && result->render.fixed_edge_rotation_angle_left == 3 &&
            result->render.shadow_lift_percent == 20,
        "Saved native rendering settings ignored");
    game["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Load("[4, 5]");
    game["pipeline"]["hmplaycropper"].remove("fixed-edge-rotation-angle");
    result = load({"apply_camera.crop_output_image=true,hstream_ui.camera_controls.Bring_Up_Shadows=30"}, game);
    Check(
        result.ok() && !result->render.no_crop && result->render.fixed_edge_rotation_angle_left == 4 &&
            result->render.fixed_edge_rotation_angle_right == 5 && result->render.shadow_lift_percent == 30,
        "Higher canonical layer did not override saved native settings");
    result =
        load({"hstream_ui.camera_controls.Exposure_x100=110,hstream_ui.camera_controls.Lift_Shadow_Black_Point=1"});
    Check(
        result.ok() && std::abs(result->render.exposure - 1.1f) < .00001 && result->render.lift_shadow_black_point,
        "UI tone conversion changed");
    game = YAML::Load("stitching: {projection_framing: {rotation_degrees: [0, 3, 0]}}");
    result = load({"pipeline.hmplaycropper.private-properties.fixed-edge-rotation-angle-left=8"}, game);
    Check(
        result.ok() && result->render.fixed_edge_rotation_angle_left == 0 &&
            result->render.fixed_edge_rotation_angle_right == 0,
        "Leveled NONA panorama tilted a second time");
    result = load({"stitching.mapping_backend=opencv-magsac"}, game);
    Check(
        result.ok() && result->render.fixed_edge_rotation_angle_left == 10 &&
            result->render.fixed_edge_rotation_angle_right == 10,
        "Non-NONA edge rotation suppressed");
    game["stitching"]["projection_framing"]["rotation_degrees"] = YAML::Load("[4, 0, 0]");
    result = load({}, game);
    Check(result.ok() && result->render.fixed_edge_rotation_angle_left == 10, "Yaw-only framing suppressed tilt");
    for (const char* options :
         {"video_out.output_width=1919",
          "runtime.amd.output.height=0",
          "runtime.amd.playback_start_seconds=-1",
          "runtime.amd.playback_start_seconds=.inf",
          "hstream_ui.camera_controls.Exposure_x100=131",
          "hstream_ui.camera_controls.Bring_Up_Shadows=.nan"})
      Check(!load({options}).ok(), std::string("Invalid output accepted: ") + options);
    auto extreme = YAML::Clone(baseline);
    extreme["runtime"]["amd"]["output"]["width"] = 2;
    extreme["runtime"]["amd"]["output"]["height"] = 2147483646;
    const auto overflow = hm::config::ResolveProgramConfiguration(
        extreme,
        YAML::Node(YAML::NodeType::Map),
        YAML::Node(YAML::NodeType::Map),
        YAML::Node(YAML::NodeType::Map),
        {"video_out.output_width=2147483646"});
    Check(
        overflow.ok() && !hm::program::ResolveProgramOutputConfig(*overflow).ok(),
        "Derived output dimension overflow accepted");
    auto missing = YAML::Clone(baseline);
    missing["runtime"]["amd"]["output"].remove("width");
    auto merged = hm::config::ResolveProgramConfiguration(
        missing, YAML::Node(YAML::NodeType::Map), YAML::Node(YAML::NodeType::Map), YAML::Node(YAML::NodeType::Map), {});
    Check(
        merged.ok() && !hm::program::ResolveProgramOutputConfig(*merged).ok(),
        "Missing baseline setting silently defaulted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
