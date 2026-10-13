#include "hstream/src/libs/stitching/GameConfig.h"

#include <iostream>
#include <string>

using namespace hm::stitching;

namespace {
bool expect(bool value, const char* message) {
  if (!value)
    std::cerr << "FAIL: " << message << '\n';
  return value;
}
} // namespace

int main() {
  StitchInputLens lens;
  lens.projection = "full-frame-fisheye";
  lens.optimization = "fixed";
  lens.distortion = {0, 0.0208293582808693, 0};
  lens.center_offset = {-10.102540338009 / 7680, 620.178405843678 / 4320};
  auto parsed = ParseStitchInputLens(SerializeStitchInputLens(lens));
  bool ok = expect(parsed.ok() && *parsed == lens, "optics must round-trip at full precision");
  for (const auto* invalid :
       {"[]",
        "{projection: panini}",
        "{optimization: guess}",
        "{distortion: [0, 1]}",
        "{distortion: [0, .nan, 0]}",
        "{center_offset: [0, 1]}",
        "{typo: 1}"})
    ok &= expect(!ParseStitchInputLens(invalid).ok(), "invalid optics must fail validation");
  const std::string pto =
      "p f19 w1920 h1080 v200 P\"100 -10 -10\"\n"
      "i w7680 h4320 f0 v121.413487813061 a0 b0 c0 d0 e0 r0 p0 y0 n\"left.png\"\n"
      "i w7680 h4320 f0 v=0 a=0 b=0 c=0 d=0 e=0 r0 p0 y0 n\"right.png\"\n"
      "v v0 a0 b0 c0 d0 e0 r0 p0 r1 p1 y1\nv\nc n0 N1 x12 y13 X14 Y15 t0\n";
  auto legacy = ApplyStitchInputLens(pto, {});
  ok &= expect(legacy.ok() && *legacy == pto, "legacy defaults must preserve PTO bytes");
  auto fixed = ApplyStitchInputLens(pto, lens);
  ok &= expect(fixed.ok(), "fixed optics must apply");
  if (fixed.ok()) {
    ok &= expect(
        fixed->find("p f19 w1920 h1080 v200 P\"100 -10 -10\"") == 0,
        "input optics must leave the output General Panini projection untouched");
    ok &= expect(
        fixed->find("f3 v121.413487813061") != std::string::npos &&
            fixed->find("d-10.102540338") != std::string::npos && fixed->find("e620.178405843") != std::string::npos &&
            fixed->find("a=0 b=0 c=0 d=0 e=0") != std::string::npos &&
            fixed->find("v r0 p0 r1 p1 y1\nv\n") != std::string::npos && fixed->find("v v0") == std::string::npos,
        "fixed optics must scale center shifts and optimize only camera poses");
  }
  lens.optimization = "refine";
  auto refine = ApplyStitchInputLens(pto, lens);
  ok &= expect(
      refine.ok() && refine->find("v r0 p0 r1 p1 y1 v0 b0 d0 e0\nv") != std::string::npos,
      "refine must fit FOV, b and center while retaining a and c");
  lens.center_offset = {-0.01, 0.125};
  auto scaled = ApplyStitchInputLens("i w1920 h1080 f0\ni w1920 h1080 f0\n", lens);
  ok &= expect(
      scaled.ok() && scaled->find("d-19.199") != std::string::npos && scaled->find("e135") != std::string::npos,
      "center offsets must scale with source resolution");
  ok &= expect(
      !ApplyStitchInputLens("i w1920 h1080 f0\ni w7680 h4320 f0\n", lens).ok(),
      "shared intrinsics must reject unequal source dimensions");
  YAML::Node config;
  StitchCameraSelection camera{"gopro-hero-11", 121.413487813061, 90, lens};
  write_stitch_camera_selection(config, camera);
  auto loaded = read_stitch_camera_selection(config);
  ok &= expect(loaded.ok() && *loaded == camera, "camera selection must retain input optics");
  auto changed = camera;
  changed.input_lens.center_offset[1] += 0.01;
  ok &= expect(changed != camera, "lens changes must change calibration ownership");
  camera.input_lens = {};
  write_stitch_camera_selection(config, camera);
  ok &= expect(
      config["stitching"]["input_lens"].IsMap() &&
          read_stitch_camera_selection(config)->input_lens == StitchInputLens{},
      "saving default optics must explicitly override inherited correction");
  return ok ? 0 : 1;
}
