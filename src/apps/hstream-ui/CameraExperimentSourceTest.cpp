#include "src/apps/hstream-ui/CameraExperimentSource.h"

#include <iostream>
#include <stdexcept>

int main() {
  try {
    hm::playtracker_replay::StitchingMedia media;
    auto resolve = [&](const std::string& yaml, bool automatic = false) {
      const auto result = ResolveExperimentStitchingSettings(YAML::Load(yaml), automatic, &media);
      if (!result.ok())
        throw std::runtime_error(result.ToString());
    };
    resolve("{}");
    if (media.blend_mode != "laplacian" || media.blend_feather_fraction != 0.05)
      throw std::runtime_error("Missing blend settings must use the production defaults");
    resolve("stitching: {blend_mode: alpha, blend_feather_fraction: 0}");
    if (media.blend_mode != "alpha" || media.blend_feather_fraction != 0)
      throw std::runtime_error("Camera experiments must retain an explicit zero feather width");
    resolve(
        "stitching: {blend_mode: laplacian, blend_feather_fraction: 0.1}\n"
        "pipeline: {hmstitcher: {blend_mode: hard-seam, blend_feather_fraction: 0.2}}");
    if (media.blend_mode != "gpu-hard-seam" || media.blend_feather_fraction != 0.2)
      throw std::runtime_error("Native blend aliases must override canonical values in the same layer");
    const auto layered = ResolveExperimentStitchingSettings(
        YAML::Load("stitching: {blend_mode: alpha}"),
        false,
        &media,
        YAML::Load("stitching: {blend_mode: laplacian, blend_feather_fraction: 0.05}"),
        YAML::Load("pipeline: {hmstitcher: {blend-mode: hard-seam, blend-feather-fraction: 0.25}}"));
    if (!layered.ok() || media.blend_mode != "alpha" || media.blend_feather_fraction != 0.25)
      throw std::runtime_error("Camera experiments must resolve baseline, user and game blend layers");
    for (const char* invalid :
         {"stitching: {blend_mode: multiblend}",
          "pipeline: {hmstitcher: {blend-mode: invalid}}",
          "stitching: {blend_feather_fraction: .nan}",
          "stitching: {blend_feather_fraction: 1.1}"}) {
      if (ResolveExperimentStitchingSettings(YAML::Load(invalid), false, &media).ok())
        throw std::runtime_error("Invalid experiment blend settings must fail before constructing the graph");
    }
    resolve("stitching: {post_stitch_rotate_degrees: null}");
    if (media.rotation != 0)
      return 1;
    resolve(
        "pipeline: {hmstitcher: {post-stitch-rotate-degrees: null, post_stitch_rotate_degrees: null}}\n"
        "stitching: {post_stitch_rotate_degrees: 2.5}");
    if (media.rotation != 2.5)
      return 2;
    resolve(
        "pipeline: {hmstitcher: {post-stitch-rotate-degrees: 4.5, post_stitch_rotate_degrees: invalid}}\n"
        "stitching: {post_stitch_rotate_degrees: invalid}");
    if (media.rotation != 4.5)
      return 3;
    for (bool automatic : {false, true}) {
      resolve("pipeline: {hmstitcher: {properties: {high-bit-depth: auto}}}", automatic);
      if (media.high_bit_depth != automatic)
        return 4;
      resolve("pipeline: {hmstitcher: {properties: {high-bit-depth: null}}}", automatic);
      if (media.high_bit_depth != automatic)
        return 5;
      for (const char* on : {"1", "true", "TRUE"}) {
        resolve(std::string("pipeline: {hmstitcher: {properties: {high-bit-depth: ") + on + "}}}", automatic);
        if (!media.high_bit_depth)
          return 6;
      }
      for (const char* off : {"0", "false", "FALSE"}) {
        resolve(std::string("pipeline: {hmstitcher: {properties: {high-bit-depth: ") + off + "}}}", automatic);
        if (media.high_bit_depth)
          return 7;
      }
    }
    resolve("hstream_ui: {camera_controls: {Use_10_Bit_Grading: 1}}");
    if (!media.high_bit_depth)
      return 8;
    resolve(
        "pipeline: {hmstitcher: {properties: {high-bit-depth: false, exposure: 0.1}}, "
        "hmplaycropper: {properties: {exposure: 0.8, shadow-lift: 50, shadow-lift-black-point: 1}}}");
    if (media.exposure != 0.8 || media.shadow_lift != 50 || !media.shadow_lift_black_point)
      return 11;
    resolve(
        "pipeline: {hmstitcher: {properties: {high-bit-depth: true, exposure: 0.6}}, "
        "hmplaycropper: {properties: {exposure: 0.8, shadow-lift: 50, shadow-lift-black-point: true}}}");
    if (media.exposure != 0.6 || media.shadow_lift != 50 || !media.shadow_lift_black_point)
      return 12;
    resolve(
        "hstream_ui: {camera_controls: {Exposure_x100: 34, Bring_Up_Shadows: 60, Lift_Shadow_Black_Point: false}}\n"
        "pipeline: {hmstitcher: {properties: {exposure: 0.9, shadow-lift-black-point: 1}}}");
    if (media.exposure != 0.34 || media.shadow_lift != 60 || media.shadow_lift_black_point)
      return 13;
    if (ResolveExperimentStitchingSettings(
            YAML::Load("pipeline: {hmstitcher: {properties: {high-bit-depth: invalid}}}"), false, &media)
            .ok())
      return 9;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 10;
  }
}
