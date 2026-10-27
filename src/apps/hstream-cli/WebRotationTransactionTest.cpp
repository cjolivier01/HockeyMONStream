#include "WebRotationTransaction.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/LiveOutputEpoch.h"
#include "hstream/src/libs/stitching/LiveRotationConfigOwner.h"

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
  using namespace hm::preview;
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("web-rotation-transaction-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  const auto hugin = create_hugin_generation(root);
  if (!hugin.ok())
    return 1;
  const std::string generation = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin->size()) + "\n" +
      *hugin + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
  const auto reset = [&]() { write_config(root / "config.yaml", generation); };
  const auto pending = [&]() {
    const auto config = YAML::LoadFile((root / "config.yaml").string());
    const auto value = config["rink"]["stitched_output_pending_authorization_id"];
    return value && value.IsScalar() ? value.as<std::string>() : std::string();
  };
  const RouteOwner::Session session{"session", 0, 1};
  const hm::program::RotationControlValues initial{
      {"Stitch_Rotate_Degrees", 90},
      {"Link_Fixed_Edge_Rotation_Left_Right", 1},
      {"Left_Fixed_Edge_Rotation_Angle_x10", 100},
      {"Right_Fixed_Edge_Rotation_Angle_x10", 100}};
  std::string blocked;
  uint64_t operation = 1;
  bool retire = false, fail_apply = false, conflict_commit = false;
  unsigned applied = 0;
  std::vector<YAML::Node> replies;
  WebRotationTransaction::Hooks hooks{
      [&](const auto& value, bool mutation) {
        return !(value == session) ? std::string("stale-session") : mutation ? blocked : std::string();
      },
      [&](const auto& controls, const auto& changed, const auto& authorization) -> absl::StatusOr<uint64_t> {
        ++applied;
        if (changed.count("Stitch_Rotate_Degrees"))
          ok &= expect(
              authorization && pending() == authorization->value.authorization_id,
              "runtime apply must own the exact durable epoch");
        if (controls.at("Link_Fixed_Edge_Rotation_Left_Right"))
          ok &= expect(
              controls.at("Left_Fixed_Edge_Rotation_Angle_x10") == controls.at("Right_Fixed_Edge_Rotation_Angle_x10"),
              "linked values mirror before apply");
        if (fail_apply)
          return absl::AbortedError("injected ambiguous apply failure");
        if (conflict_commit) {
          auto config = YAML::LoadFile((root / "config.yaml").string());
          config["rink"]["stitched_output_pending_authorization_id"] = "newer-owner";
          std::ofstream(root / "config.yaml") << config << '\n';
        }
        return ++operation;
      },
      [&]() { return operation; },
      [&](const auto& json) { replies.push_back(YAML::Load(json)); },
      [&]() { retire = true; }};
  const auto drain = [&](WebRotationTransaction& transaction) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (transaction.pending() && !retire && std::chrono::steady_clock::now() < deadline) {
      transaction.Poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !transaction.pending();
  };
  reset();
  {
    WebRotationTransaction transaction(root.string(), initial, false, hooks);
    ok &= expect(transaction.Handle({session, "1", {{"Stitch_Rotate_Degrees", 80}}, false}), "rotation admitted");
    ok &= expect(drain(transaction), "rotation completion drains");
    std::vector<std::string> phases;
    for (const auto& reply : replies)
      phases.push_back(reply["status"].as<std::string>());
    ok &= expect(
        phases == std::vector<std::string>{"authorizing", "applying", "committing", "applied"} &&
            replies.back()["operation"].as<std::string>() == "2" && !replies.back()["fixedEdgeSuppressed"].as<bool>() &&
            transaction.controls().at("Stitch_Rotate_Degrees") == 80,
        "all durable phases precede authoritative applied readback and exact operation");
    const unsigned before = applied;
    ok &= expect(
        !transaction.Handle({session, "1", {{"Stitch_Rotate_Degrees", 70}}, false}) && applied == before,
        "duplicate request never replays a property mutation");
    ok &= expect(
        transaction.Handle({session, "2", {{"Right_Fixed_Edge_Rotation_Angle_x10", 127}}, false}) &&
            transaction.controls().at("Left_Fixed_Edge_Rotation_Angle_x10") == 127 &&
            transaction.controls().at("Right_Fixed_Edge_Rotation_Angle_x10") == 127,
        "fixed-only linked edit mirrors without authorizing a new stitched generation");
    ok &= expect(transaction.Shutdown().ok() && pending().empty(), "retired graph releases unpublished generation");
  }
  reset();
  replies.clear();
  blocked.clear();
  {
    WebRotationTransaction transaction(root.string(), initial, false, hooks);
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    if (!lock.ok())
      return 1;
    const unsigned before = applied;
    transaction.Handle({session, "3", {{"Stitch_Rotate_Degrees", 70}}, false});
    ok &= expect(
        !transaction.Handle({session, "4", {}, true}) &&
            replies.back()["reason"].as<std::string>() == "rotation-pending",
        "query cannot claim settled state while disk authorization is pending");
    blocked = "preview-paused";
    (*lock).reset();
    ok &= expect(
        drain(transaction) && applied == before && pending().empty() && transaction.controls() == initial &&
            replies.back()["reason"].as<std::string>() == "preview-paused",
        "pause during disk authorization rolls back before runtime apply and preserves old readback");
    std::vector<std::string> phases;
    for (const auto& reply : replies)
      phases.push_back(reply["status"].as<std::string>());
    ok &= expect(
        phases == std::vector<std::string>{"authorizing", "failed", "rolling-back", "failed"},
        "rollback phase retains ownership until exact disk cleanup finishes");
  }
  reset();
  replies.clear();
  blocked.clear();
  fail_apply = true;
  {
    WebRotationTransaction transaction(root.string(), initial, false, hooks);
    transaction.Handle({session, "5", {{"Stitch_Rotate_Degrees", 60}}, false});
    drain(transaction);
    ok &= expect(
        retire && !pending().empty() && replies.back()["reason"].as<std::string>() == "retire-required" &&
            replies.back()["controls"].IsNull() && replies.back()["operation"].IsNull(),
        "ambiguous apply retains disk authorization and requires graph retirement");
    ok &=
        expect(transaction.Shutdown().ok() && pending().empty(), "post-graph shutdown clears ambiguous authorization");
  }
  reset();
  replies.clear();
  retire = false;
  fail_apply = false;
  conflict_commit = true;
  {
    WebRotationTransaction transaction(root.string(), initial, false, hooks);
    transaction.Handle({session, "6", {{"Stitch_Rotate_Degrees", 50}}, false});
    drain(transaction);
    ok &= expect(
        retire && pending() == "newer-owner" && transaction.controls() == initial &&
            replies.back()["reason"].as<std::string>() == "retire-required" && replies.back()["controls"].IsNull(),
        "durable finalization conflict retires the graph without claiming applied controls");
    const auto cleanup = transaction.Shutdown();
    ok &= expect(
        !cleanup.ok() && pending() == "newer-owner",
        "retirement reports conflicting ownership without removing another authorization");
  }
  reset();
  replies.clear();
  retire = false;
  conflict_commit = false;
  {
    WebRotationTransaction transaction(root.string(), initial, true, hooks);
    const unsigned before = applied;
    ok &= expect(
        !transaction.Handle({session, "7", {{"Left_Fixed_Edge_Rotation_Angle_x10", 30}}, false}) && applied == before &&
            replies.back()["fixedEdgeSuppressed"].as<bool>() &&
            replies.back()["reason"].as<std::string>() == "fixed-edge-suppressed",
        "leveled NONA rejects fixed-edge edits and advertises disabled controls");
  }
  fs::remove_all(root);
  return ok ? 0 : 1;
}
