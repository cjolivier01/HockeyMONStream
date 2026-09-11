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
