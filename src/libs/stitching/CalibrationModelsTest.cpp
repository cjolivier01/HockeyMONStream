#include "hstream/src/libs/stitching/CalibrationModels.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_model(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "test model\n";
}

} // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hstream-calibration-models-test-" + std::to_string(::getpid()));
  const fs::path package_models = root / "package-models";
  const fs::path user_models = root / "home/.cache/hstream/models";

  constexpr const char* rink = "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx";
  constexpr const char* superpoint = "superpoint-lightglue-pipeline-228994cea8c01014.onnx";
  constexpr const char* dedode = "dedode-lightglue-lc4v2-bupright-f8bd053e44d57a77.onnx";
  constexpr const char* loftr = "efficient-loftr-outdoor-opt-a2cbdcfef0ddb5cd.onnx";
  write_model(package_models / rink);
  write_model(package_models / dedode);
  write_model(package_models / loftr);
  write_model(user_models / superpoint);

  ::setenv("HOME", (root / "home").c_str(), 1);
  ::unsetenv("HM_NATIVE_MODEL_DIR");
  ::setenv("HM_PACKAGED_NATIVE_MODEL_DIR", package_models.c_str(), 1);
  ::unsetenv("HM_RINK_ONNX_MODEL");
  ::unsetenv("HM_FEATURE_MATCHER_ONNX_MODEL");
  ::unsetenv("HM_DEDODE_LIGHTGLUE_ONNX_MODEL");
  ::unsetenv("HM_LOFTR_ONNX_MODEL");

  bool ok = true;
  const auto rink_path = hm::stitching::rink_model_path();
  ok &=
      expect(rink_path.ok() && *rink_path == package_models / rink, "rink model must use the package model directory");
  const auto superpoint_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      superpoint_path.ok() && *superpoint_path == user_models / superpoint,
      "non-redistributable SuperPoint must use the user cache even when packaged models exist");
  const fs::path explicit_models = root / "explicit-models";
  write_model(explicit_models / superpoint);
  ::setenv("HM_NATIVE_MODEL_DIR", explicit_models.c_str(), 1);
  const auto explicit_superpoint_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      explicit_superpoint_path.ok() && *explicit_superpoint_path == explicit_models / superpoint,
      "an explicit generic model directory must remain usable for SuperPoint");
  ::unsetenv("HM_NATIVE_MODEL_DIR");
  const auto dedode_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue);
  ok &= expect(
      dedode_path.ok() && *dedode_path == package_models / dedode,
      "redistributable DeDoDe must use the package model directory");
  const auto loftr_path = hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kLoFTR);
  ok &= expect(
      loftr_path.ok() && *loftr_path == package_models / loftr,
      "redistributable LoFTR must use the package model directory");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
