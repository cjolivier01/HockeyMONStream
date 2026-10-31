#include "hstream/src/libs/stitching/CameraSourceStart.h"

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  using namespace hm::stitching;
  try {
    const auto starts = ReadCameraSourceStarts(YAML::Load("{'GX010104.MP4': 5, 'GX010105.MP4': 108}"));
    check(starts.ok(), "Read source-bound starts");
    check(
        CameraSourceStart(*starts, "/game", "/game/GX010105.MP4") == 108,
        "Absolute and game-relative source identities must agree");
    check(CameraSourceStart(*starts, "/game", "unselected.MP4") == 0, "Absent camera trim means no skip");
    check(
        ValidateCameraSourceStarts(*starts, "/game", {"GX010105.MP4", "GX010104.MP4"}).ok(),
        "Changing camera order must preserve source identity");
    check(
        !ValidateCameraSourceStarts(*starts, "/game", {"GX010104.MP4", "other.MP4"}).ok(),
        "Do not silently ignore a trim belonging to a different source");
    check(
        !ValidateCameraSourceStarts(
             {{"GX010104.MP4", 5}, {"/game/GX010104.MP4", 108}}, "/game", {"GX010104.MP4", "GX010105.MP4"})
             .ok(),
        "Two spellings cannot assign conflicting starts to the same camera");
    check(*ReadCameraSourceStarts(WriteCameraSourceStarts(*starts)) == *starts, "Starts must round trip exactly");
    check(ReadCameraSourceStarts(YAML::Load("{camera: 0}"))->empty(), "Zero trims must canonicalize to absence");
    for (const char* invalid :
         {"{camera: -1}",
          "{camera: .nan}",
          "{camera: .inf}",
          "{camera: 86400}",
          "{camera: true}",
          "{a: 1, b: 2, c: 3}",
          "{a: 1, a: 2}",
          "[1, 2]"})
      check(!ReadCameraSourceStarts(YAML::Load(invalid)).ok(), "Invalid camera start accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
