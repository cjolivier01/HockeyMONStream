#include "hstream/src/libs/stitching/LiveRotationConfigOwner.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/LiveOutputEpoch.h"

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_config(const std::filesystem::path& path, const std::string& generation) {
  YAML::Node config(YAML::NodeType::Map);
  config["unrelated"] = "preserved";
  config["rink"]["stitched_output_generation"] = generation;
  config["rink"]["scoreboard"]["perspective_polygon"] = std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}};
  std::ofstream(path) << config << '\n';
}

bool write_mapping_tiff(const std::filesystem::path& path, uint32_t width, uint32_t height, float x_position) {
  TIFF* tiff = TIFFOpen(path.c_str(), "w");
  if (!tiff)
    return false;
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, height);
  TIFFSetField(tiff, TIFFTAG_XRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_YRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_XPOSITION, x_position);
  TIFFSetField(tiff, TIFFTAG_YPOSITION, 0.0f);
  std::vector<uint8_t> row(width, 0);
  bool ok = true;
  for (uint32_t y = 0; y < height; ++y)
    ok = ok && TIFFWriteScanline(tiff, row.data(), y, 0) >= 0;
  TIFFClose(tiff);
  return ok;
}

absl::StatusOr<std::string> create_hugin_generation(const std::filesystem::path& root) {
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"}) {
    std::ofstream(root / name) << name << '\n';
  }
  if (!write_mapping_tiff(root / "mapping_0000.tif", 64, 32, 0.0f) ||
      !write_mapping_tiff(root / "mapping_0001.tif", 64, 32, 32.0f)) {
    return absl::InternalError("Unable to write mapping TIFF fixtures");
  }
  const cv::Mat remap(32, 64, CV_16U, cv::Scalar(0));
  for (const char* name : {
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    if (!cv::imwrite((root / name).string(), remap))
      return absl::InternalError("Unable to write remap TIFF fixtures");
  }
  cv::Mat seam(32, 96, CV_8U, cv::Scalar(0));
  seam.colRange(48, seam.cols).setTo(255);
  if (!cv::imwrite((root / "seam_file.png").string(), seam))
    return absl::InternalError("Unable to write seam fixture");
  auto lock = hm::stitching::lock_canvas_constraint_artifacts(root);
  if (!lock.ok())
    return lock.status();
  return hm::stitching::stitch_artifact_generation_id_locked(root);
}

} // namespace

int main() {
  namespace fs = std::filesystem;
  using hm::stitching::LiveRotationConfigOwner;
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("live-rotation-owner-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  const auto hugin = create_hugin_generation(root);
  if (!hugin.ok())
    return 1;
  const std::string generation = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin->size()) + "\n" +
      *hugin + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
  const auto reset = [&]() { write_config(root / "config.yaml", generation); };
  const auto pending = [&]() {
    auto config = YAML::LoadFile((root / "config.yaml").string());
    const auto value = config["rink"]["stitched_output_pending_authorization_id"];
    return value && value.IsScalar() ? value.as<std::string>() : std::string();
  };
  const auto drain = [&](LiveRotationConfigOwner& owner) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (owner.pending() && std::chrono::steady_clock::now() < deadline) {
      owner.Poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !owner.pending();
  };

  reset();
  {
    LiveRotationConfigOwner owner;
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    ok &= expect(lock.ok(), "test owns the real config transaction lock");
    if (!lock.ok())
      return 1;
    int authorized = 0, retired = 0;
    ok &= expect(
        owner.Authorize(
            root.string(),
            4,
            "locked",
            [&](auto result) {
              ok &= expect(result.status.ok(), "blocked authorization completes successfully before retirement");
              ++authorized;
            }),
        "authorize is admitted without waiting for disk lock");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    owner.Poll();
    ok &= expect(owner.pending() && authorized == 0, "config contention does not call back or block event loop");
    ok &= expect(
        owner.Retire(
            root.string(),
            [&](auto result) {
              ok &=
                  expect(result.status.ok() && !result.authorization, "retirement unwinds exact active authorization");
              ++retired;
            }),
        "Stop retirement queues behind in-flight authorization");
    ok &= expect(!owner.Authorize(root.string(), 6, "late", {}), "retirement forbids late edits");
    ok &= expect(!owner.AllowGame(root.string()), "restart cannot overtake pending retirement");
    (*lock).reset();
    ok &= expect(
        drain(owner) && authorized == 1 && retired == 1 && pending().empty(),
        "ordered retirement leaves no authorization on disk");
    ok &= expect(owner.AllowGame(root.string()), "completed retirement permits a new run");
    ok &= expect(owner.Shutdown().ok(), "idle shutdown succeeds");
  }

  reset();
  {
    LiveRotationConfigOwner owner;
    std::optional<LiveRotationConfigOwner::Authorization> first, second;
    owner.Authorize(root.string(), 4, "first", [&](auto result) {
      ok &= expect(result.status.ok(), "first epoch authorized");
      first = result.authorization;
    });
    ok &= expect(drain(owner) && first.has_value(), "first completion delivered on Poll");
    owner.Authorize(root.string(), 6, "second", [&](auto result) {
      ok &= expect(result.status.ok(), "second epoch authorized");
      second = result.authorization;
    });
    ok &= expect(drain(owner) && second.has_value() && pending() == "second", "new epoch supersedes old one");
    bool stale = false;
    owner.Rollback(*first, [&](auto result) { stale = absl::IsAborted(result.status); });
    ok &= expect(drain(owner) && stale && pending() == "second", "stale rollback cannot remove current epoch");
    ok &= expect(owner.Shutdown().ok() && pending().empty(), "shutdown unwinds current and restored predecessor");
  }

  reset();
  {
    LiveRotationConfigOwner owner;
    std::optional<LiveRotationConfigOwner::Authorization> authorization;
    owner.Authorize(root.string(), 8, "commit", [&](auto result) { authorization = result.authorization; });
    ok &= expect(drain(owner) && authorization.has_value(), "commit fixture authorization is owned");
    bool committed = false;
    owner.Commit(*authorization, [&](auto result) { committed = result.status.ok(); });
    ok &= expect(
        drain(owner) && committed && pending() == "commit", "invalidating commit retains pending producer epoch");
    ok &= expect(owner.Shutdown().ok() && pending().empty(), "shutdown owns committed but unpublished epoch");
  }

  reset();
  {
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    if (!lock.ok())
      return 1;
    bool delivered = false;
    std::thread unlocker([lock = std::move(*lock)]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      lock.reset();
    });
    {
      LiveRotationConfigOwner owner;
      owner.Authorize(root.string(), 10, "destroyed", [&](auto) { delivered = true; });
    }
    unlocker.join();
    ok &=
        expect(!delivered && pending().empty(), "forced destruction joins work and cleans disk without stale callback");
  }
  fs::remove_all(root);
  return ok ? 0 : 1;
}
