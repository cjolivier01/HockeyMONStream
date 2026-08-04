#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main() {
  bool ok = true;
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("field-mask-persistence-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  {
    std::ofstream config(root / "config.yaml");
    config << "unrelated:\n  keep: true\n";
  }
  cv::Mat first(24, 32, CV_8U, cv::Scalar(0));
  cv::Mat second(24, 32, CV_8U, cv::Scalar(0));
  first(cv::Rect(2, 3, 10, 8)).setTo(255);
  second(cv::Rect(20, 10, 8, 10)).setTo(255);
  hm::stitching::RinkProfile profile;
  profile.masks = {first, second};
  profile.centroid = {15.25, 11.5};
  profile.combined_bbox = {2.0, 3.0, 26.0, 17.0};
  auto status = hm::stitching::save_rink_profile(root.string(), profile);
  ok &= expect(status.ok(), "valid rink profile must persist");
  if (status.ok()) {
    ok &=
        expect(!cv::imread((root / "rink_mask_0.png").string(), cv::IMREAD_GRAYSCALE).empty(), "first mask must load");
    ok &=
        expect(!cv::imread((root / "rink_mask_1.png").string(), cv::IMREAD_GRAYSCALE).empty(), "second mask must load");
    const YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(config["unrelated"]["keep"].as<bool>(), "unrelated config must survive");
    ok &= expect(config["rink"]["ice_contours_mask_count"].as<int>() == 2, "mask count must match files");
    const YAML::Node bbox = config["rink"]["ice_contours_combined_bbox"];
    ok &= expect(bbox[0].as<double>() == 2.0 && bbox[2].as<double>() == 28.0, "bbox must persist as x1,y1,x2,y2");

    // Simulate SIGKILL after a prepared transaction published only part of a
    // new generation. The next field-mask read must restore the complete old
    // generation before consuming any artifact.
    const fs::path interrupted = root / ".hmstream-rink-interrupted";
    fs::create_directories(interrupted / "previous");
    fs::copy_file(root / "config.yaml", interrupted / "previous" / "config.yaml");
    fs::copy_file(root / "rink_mask_0.png", interrupted / "previous" / "rink_mask_0.png");
    fs::copy_file(root / "rink_mask_1.png", interrupted / "previous" / "rink_mask_1.png");
    {
      std::ofstream(interrupted / "new-files") << "rink_mask_0.png\nrink_mask_1.png\nconfig.yaml\n";
      std::ofstream(interrupted / "state") << "PREPARED\n";
      std::ofstream(root / "config.yaml") << "interrupted: true\n";
      cv::imwrite((root / "rink_mask_0.png").string(), cv::Mat(2, 2, CV_8U, cv::Scalar(255)));
    }
    ok &= expect(hm::stitching::is_field_mask_configured(root.string()), "prepared rink transaction must recover");
    const YAML::Node recovered = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(recovered["unrelated"]["keep"].as<bool>(), "rink recovery must restore the prior config");
    ok &= expect(
        cv::imread((root / "rink_mask_0.png").string(), cv::IMREAD_GRAYSCALE).size() == cv::Size(32, 24),
        "rink recovery must restore the prior mask generation");
    ok &= expect(!fs::exists(interrupted), "recovered rink transaction must be cleaned");
  }
  profile.masks[1] = cv::Mat(10, 10, CV_8U);
  ok &= expect(!hm::stitching::save_rink_profile(root.string(), profile).ok(), "mixed mask dimensions must fail");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
