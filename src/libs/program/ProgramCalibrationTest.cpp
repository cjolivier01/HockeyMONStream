#include "hstream/src/libs/program/ProgramCalibration.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    char name[] = "/tmp/hstream-program-calibration-XXXXXX";
    Check(mkdtemp(name) != nullptr, "Cannot create calibration fixture");
    path = name;
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
} // namespace

int main() {
  using hm::program::ProgramCalibrationRequest;
  using hm::program::ValidateLockedProgramCalibration;
  using hm::program::ValidateProgramCalibrationSelection;
  try {
    const auto saved = YAML::Load(
        "stitching: {stitch_frame_time: '00:03:39.000'}\n"
        "hstream_ui: {stitching_calibration: {invalidation_id: original, status: complete}}\n");
    const ProgramCalibrationRequest requested{"00:03:39", "original"};
    const auto original = YAML::Dump(saved);
    Check(ValidateProgramCalibrationSelection(saved, requested).ok(), "Equivalent reference spellings differ");
    Check(ValidateProgramCalibrationSelection(saved, {"219", "original"}).ok(), "Seconds reference differs");
    auto fractional = YAML::Clone(saved);
    fractional["stitching"]["stitch_frame_time"] = "00:03:39.125";
    Check(
        ValidateProgramCalibrationSelection(fractional, {"219.125", "original"}).ok(), "Fractional reference differs");
    Check(!ValidateProgramCalibrationSelection(fractional, requested).ok(), "Different reference accepted");
    fractional["stitching"]["stitch_frame_time"] = "00:01:02.099";
    for (const auto* time : {"62.099", "01:02.099", "00:01:02.0990000000"})
      Check(ValidateProgramCalibrationSelection(fractional, {time, "original"}).ok(), "Decimal reference differs");
    for (const auto* time : {"62.098999999", "62.099000001", "62.0990000001"})
      Check(!ValidateProgramCalibrationSelection(fractional, {time, "original"}).ok(), "Distinct reference accepted");
    fractional["stitching"]["stitch_frame_time"] = "-00:01:02.099";
    for (const auto* time : {"-62.099", "-01:02.099", "-00:01:02.0990000000"})
      Check(ValidateProgramCalibrationSelection(fractional, {time, "original"}).ok(), "End-relative reference differs");
    for (const auto* time : {"62.099", "-62.098999999", "-62.099000001", "--62.099", "-0"})
      Check(
          !ValidateProgramCalibrationSelection(fractional, {time, "original"}).ok(),
          "Distinct end-relative reference accepted");
    for (const auto* time : {"", "bad", "nan", "1e50", "0:0:0:219", "219.", "0::219", "9223372036.854775808"})
      Check(!ValidateProgramCalibrationSelection(saved, {time, "original"}).ok(), "Invalid reference accepted");
    auto boundary = YAML::Clone(saved);
    boundary["stitching"]["stitch_frame_time"] = "9223372036.854775807";
    Check(
        ValidateProgramCalibrationSelection(boundary, {"2562047:47:16.854775807", "original"}).ok(),
        "Largest representable reference differs");
    for (const auto* owner : {"", "replacement"})
      Check(!ValidateProgramCalibrationSelection(saved, {"219", owner}).ok(), "Wrong owner accepted");
    auto pending = YAML::Clone(saved);
    pending["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    Check(!ValidateProgramCalibrationSelection(pending, requested).ok(), "Pending calibration accepted");
    pending = YAML::Clone(saved);
    pending["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
    Check(!ValidateProgramCalibrationSelection(pending, requested).ok(), "Invalidated calibration accepted");
    Check(!ValidateProgramCalibrationSelection(YAML::Node(), requested).ok(), "Missing calibration accepted");
    Check(YAML::Dump(saved) == original, "Validation mutated the launch snapshot");

    Directory game;
    const auto publish = [&](const YAML::Node& config) {
      auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game.path);
      Check(lock.ok(), lock.status().ToString());
      const auto status = hm::stitching::publish_game_config(game.path, YAML::Dump(config));
      Check(status.ok(), status.ToString());
    };
    publish(saved);
    {
      auto artifacts = hm::stitching::HuginProject::RecoverAndLock(game.path);
      Check(artifacts.ok(), artifacts.status().ToString());
      Check(ValidateLockedProgramCalibration(game.path, requested, **artifacts).ok(), "Current owner was rejected");
    }
    // The source/model setup interval can contain another completed solve.
    // The old launch snapshot still passes, but the artifact-loading boundary
    // must reject that stale request using the current locked configuration.
    auto replaced = YAML::Clone(saved);
    replaced["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "replacement";
    publish(replaced);
    Check(ValidateProgramCalibrationSelection(saved, requested).ok(), "Launch snapshot changed during replacement");
    {
      auto artifacts = hm::stitching::HuginProject::RecoverAndLock(game.path);
      Check(artifacts.ok(), artifacts.status().ToString());
      Check(
          !ValidateLockedProgramCalibration(game.path, requested, **artifacts).ok(),
          "Replacement calibration passed the artifact-loading check");
    }
    publish(fractional);
    {
      auto artifacts = hm::stitching::HuginProject::RecoverAndLock(game.path);
      Check(artifacts.ok(), artifacts.status().ToString());
      Check(
          !ValidateLockedProgramCalibration(game.path, requested, **artifacts).ok(),
          "Changed reference passed the artifact-loading check");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
