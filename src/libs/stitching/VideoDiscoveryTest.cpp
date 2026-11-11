#include "hstream/src/libs/stitching/VideoDiscovery.h"

#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
absl::Status Validate(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  return hm::stitching::ValidateCameraChapters({{"left", left}, {"right", right}});
}
} // namespace

int main() {
  try {
    const std::vector<std::string> left{"left/GX010011.MP4", "left/GX020011.MP4", "left/GX030011.MP4"};
    const std::vector<std::string> right{"right/GH010022.MP4", "right/GH020022.MP4", "right/GH030022.MP4"};
    Check(Validate(left, right).ok(), "Different camera recording IDs must be accepted");
    Check(!Validate(left, {right[1], right[2]}).ok(), "Missing first chapter must be diagnosed");
    Check(
        !hm::stitching::ValidateCameraChapters({{"a", {left[0]}}, {"b", left}, {"c", {right[0], right[2]}}}).ok(),
        "A shorter first camera must not hide gaps between later cameras");
    const auto gap = Validate(left, {right[0], right[2]});
    Check(
        !gap.ok() && gap.message().find("Camera right is missing GoPro chapter 2") != std::string::npos &&
            gap.message().find("GX020011.MP4") != std::string::npos,
        "Missing middle chapter must identify camera, chapter and peer filename");
    Check(!Validate({left[0], left[2]}, right).ok(), "Missing left-camera chapter must be rejected");
    Check(Validate(left, {right[0], right[1]}).ok(), "Shorter trailing right-camera list must be accepted");
    Check(Validate({left[0], left[1]}, right).ok(), "Shorter trailing left-camera list must be accepted");
    Check(
        Validate({left[0], left[2]}, {right[0], right[2]}).ok(),
        "Matching gaps do not establish a missing peer chapter");
    Check(
        !Validate({left[0], left[2], "GX040011.MP4"}, {right[0], right[1], "GH040022.MP4"}).ok(),
        "Equal counts with different internal gaps must be rejected");
    Check(
        Validate({"clip-a.mp4", "clip-b.mp4"}, {"clip.mp4"}).ok(), "Arbitrary MP4 boundaries must remain independent");
    Check(
        Validate(left, {"VID_20260101_120000_001.MP4"}).ok(),
        "Different naming schemes must not imply matching chapters");
    Check(
        !Validate(
             {"VID_20260101_120000_001.MP4", "VID_20260101_120000_002.MP4", "VID_20260101_120000_003.MP4"},
             {"VID_20260101_130000_001.MP4", "VID_20260101_130000_003.MP4"})
             .ok(),
        "Insta360 middle chapters must be checked across camera timestamps");
    Check(
        !Validate({"left-2.mp4", "left-10.mp4", "left-12.mp4"}, {"right-2.mp4", "right-12.mp4"}).ok(),
        "Numbered parts must compare numeric chapter identities");
    Check(
        Validate({"left-2.mp4", "left-10.mp4"}, {"right-2.mp4"}).ok(),
        "Extra trailing numbered parts must be accepted");
    Check(
        !Validate(
             {"GX010001.MP4", "GX020001.MP4", "GX030001.MP4", "GX010002.MP4"},
             {"GH010011.MP4", "GH030011.MP4", "GH010012.MP4", "GH020012.MP4"})
             .ok(),
        "A later recording must not hide an earlier recording's missing chapter");
    Check(
        Validate({left[2], left[0], left[1]}, {right[2], right[1], right[0]}).ok(),
        "Input order must not change validation");
    char directory[] = "/tmp/hstream-chapters-XXXXXX";
    Check(mkdtemp(directory), "Cannot create discovery fixture");
    const std::filesystem::path root(directory);
    for (const auto& relative :
         {"cam1/GX010001.MP4", "cam1/GX020001.MP4", "cam1/GX030001.MP4", "cam2/GX010002.MP4", "cam2/GX030002.MP4"}) {
      const auto file = root / relative;
      std::filesystem::create_directories(file.parent_path());
      std::ofstream(file).put('x');
    }
    const auto discovered = hm::stitching::get_available_videos(directory);
    Check(discovered.ok(), "Cannot discover camera directories");
    Check(
        !hm::stitching::ValidateDiscoveredCameraChapters(*discovered).ok(),
        "Renumbered Auto maps must retain filename gap detection");
    auto ignored_output = *discovered;
    ignored_output.erase("cam2");
    ignored_output["stitched"] = {{1, "GX010009.MP4"}, {3, "GX030009.MP4"}};
    Check(
        hm::stitching::ValidateDiscoveredCameraChapters(ignored_output).ok(),
        "Published output must not be treated as a camera");
    // Root Left/Right playlists own playback even beside unused vendor files.
    const auto selected_root = root / "selected";
    std::filesystem::create_directories(selected_root);
    for (const auto* file :
         {"left-1.mp4",
          "left-2.mp4",
          "left-3.mp4",
          "right-1.mp4",
          "right-2.mp4",
          "right-3.mp4",
          "GX010001.MP4",
          "GX020001.MP4",
          "GX030001.MP4",
          "GH010002.MP4",
          "GH030002.MP4"})
      std::ofstream(selected_root / file).put('x');
    auto selected = hm::stitching::get_available_videos(selected_root.string());
    Check(
        selected.ok() && hm::stitching::ValidateDiscoveredCameraChapters(*selected).ok(),
        "Unused vendor recordings must not warn when Left/Right own playback");
    std::filesystem::remove(selected_root / "right-2.mp4");
    selected = hm::stitching::get_available_videos(selected_root.string());
    Check(
        selected.ok() && !hm::stitching::ValidateDiscoveredCameraChapters(*selected).ok(),
        "Gaps in the selected Left/Right playlists must still warn");
    std::filesystem::remove_all(root);
    std::cout << "Camera chapter validation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
