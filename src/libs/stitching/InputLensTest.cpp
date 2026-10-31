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
      "v v0 a0 b0 c0 d0 e0 r0 p0 r1 p1 y1\nv\nc n0 N1 x12 y13 X14 Y15 t0\n"
      "#hugin_optimizerMasterSwitch 1\n";
  auto legacy = ApplyStitchInputLens(pto, {});
  ok &= expect(legacy.ok() && *legacy == pto, "legacy defaults must preserve PTO bytes");
  auto fixed = ApplyStitchInputLens(pto, lens);
  ok &= expect(fixed.ok(), "fixed optics must apply");
  if (fixed.ok()) {
    ok &= expect(
        fixed->find("#hugin_optimizerMasterSwitch 0\n") != std::string::npos &&
            fixed->find("#hugin_optimizerMasterSwitch 1") == std::string::npos,
        "explicit optics must disable the Hugin preset that replaces custom optimizer variables");
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
      refine.ok() && refine->find("#hugin_optimizerMasterSwitch 0\n") != std::string::npos &&
          refine->find("v r0 p0 r1 p1 y1 v0 b0 d0 e0\nv") != std::string::npos,
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
  std::array<StitchCameraOptics, 2> separate{
      {{125.12273264892974, 70.3657608, lens}, {97.077571333536923, 54.5934677, lens}}};
  separate[0].input_lens.optimization = "fixed";
  separate[1].input_lens.optimization = "refine";
  separate[1].input_lens.distortion[1] = 0.021905029420311552;
  separate[1].input_lens.center_offset = {0.02, -0.03};
  const auto independent = ApplyStitchInputCameras(pto, separate);
  ok &= expect(
      independent.ok() && independent->find("v=0") == std::string::npos &&
          independent->find("b=0") == std::string::npos &&
          independent->find("v97.077571333536923") != std::string::npos &&
          independent->find("v125.12273264892974") != std::string::npos &&
          independent->find("v r0 p0 r1 p1 y1 v1 b1 d1 e1") != std::string::npos,
      "Separate camera optics must unlink FOV and radial terms while honoring each optimization policy");
  camera.input_cameras = {{"GX010103.MP4", separate[0]}, {"GX010104.MP4", separate[1]}};
  write_stitch_camera_selection(config, camera);
  loaded = read_stitch_camera_selection(config);
  ok &= expect(loaded.ok() && *loaded == camera, "Both source-bound prescriptions must round-trip exactly");
  auto same_shared = camera;
  same_shared.input_cameras.at("GX010104.MP4").horizontal_fov += 1;
  ok &= expect(same_shared != camera, "Changing only the second camera must invalidate calibration ownership");
  config["game"]["videos"]["left"].push_back("GX010104.MP4");
  config["game"]["videos"]["right"].push_back("GX010103.MP4");
  const auto ordered = resolve_stitch_input_camera_order("/game", config, camera);
  ok &= expect(
      ordered.ok() && (*ordered)[0] == separate[1] && (*ordered)[1] == separate[0],
      "Camera optics must follow discovered orientation, not enumeration order");
  config["game"]["videos"]["left"][0] = "replacement.MP4";
  ok &= expect(
      !resolve_stitch_input_camera_order("/game", config, camera).ok(),
      "A saved camera snapshot must reject different source recordings");
  const auto encoded = SerializeStitchInputCameras(camera.input_cameras);
  const auto decoded = ParseStitchInputCameras(encoded);
  ok &= expect(decoded.ok() && *decoded == camera.input_cameras, "Provenance serialization must retain both optics");
  auto invalid_cameras = WriteStitchInputCameras(camera.input_cameras);
  invalid_cameras["GX010103.MP4"]["input_lens"]["projection"] = YAML::Node();
  ok &= expect(!ReadStitchInputCameras(invalid_cameras).ok(), "Independent prescriptions cannot omit a lens component");
  auto snapshot = camera;
  auto current = camera;
  current.auto_fov = true;
  current.auto_lens = false;
  current.input_cameras.at("GX010104.MP4").input_lens.center_offset[0] = 0.04;
  const auto merged = merge_stitch_camera_snapshot(snapshot, current);
  ok &= expect(
      merged.input_cameras.at("GX010104.MP4").input_lens.center_offset[0] == 0.04 &&
          merged.input_cameras.at("GX010103.MP4") == snapshot.input_cameras.at("GX010103.MP4"),
      "Frozen automatic FOV must retain each source's manual lens correction");
  snapshot.input_cameras.clear();
  const auto from_shared = merge_stitch_camera_snapshot(snapshot, current);
  ok &= expect(
      from_shared.input_cameras.size() == 2 &&
          from_shared.input_cameras.at("GX010104.MP4").input_lens.center_offset[0] == 0.04 &&
          from_shared.input_cameras.at("GX010104.MP4").horizontal_fov == snapshot.horizontal_fov,
      "A shared frozen FOV must retain new independent manual lens corrections");
  return ok ? 0 : 1;
}
