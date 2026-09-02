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
  write_model(user_models / rink);
  write_model(user_models / superpoint);
  write_model(user_models / dedode);

  ::setenv("HOME", (root / "home").c_str(), 1);
  ::unsetenv("HM_NATIVE_MODEL_DIR");
  ::setenv("HM_PACKAGED_NATIVE_MODEL_DIR", package_models.c_str(), 1);
  ::unsetenv("HM_RINK_ONNX_MODEL");
  ::unsetenv("HM_FEATURE_MATCHER_ONNX_MODEL");
  ::unsetenv("HM_DEDODE_LIGHTGLUE_ONNX_MODEL");
  ::unsetenv("HM_LOFTR_ONNX_MODEL");

  bool ok = true;
  ok &= expect(
      !hm::stitching::feature_matcher_model_override_configured(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue),
      "the default user cache must not be mistaken for an explicit DeDoDe override");
  const auto rink_path = hm::stitching::rink_model_path();
  ok &= expect(
      rink_path.ok() && *rink_path == user_models / rink,
      "non-redistributable rink model must use the user cache even when packaged models exist");
  const auto superpoint_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      superpoint_path.ok() && *superpoint_path == user_models / superpoint,
      "non-redistributable SuperPoint must use the user cache even when packaged models exist");
  const auto cached_superpoint_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      cached_superpoint_asset.ok() && *cached_superpoint_asset == "superpoint-lightglue",
      "a stock cached SuperPoint graph must still select its asset declaration for SHA-256 verification");
  const fs::path explicit_models = root / "explicit-models";
  write_model(explicit_models / superpoint);
  ::setenv("HM_NATIVE_MODEL_DIR", explicit_models.c_str(), 1);
  ok &= expect(
      hm::stitching::feature_matcher_model_override_configured(
          hm::stitching::ControlPointMatcher::kSuperPointLightGlue),
      "the generic native-model directory must count as an explicit matcher override");
  const auto explicit_superpoint_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      explicit_superpoint_path.ok() && *explicit_superpoint_path == explicit_models / superpoint,
      "an explicit generic model directory must remain usable for SuperPoint");
  ::unsetenv("HM_NATIVE_MODEL_DIR");
  const auto dedode_path =
      hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue);
  ok &= expect(
      dedode_path.ok() && *dedode_path == user_models / dedode,
      "non-redistributable DeDoDe must use the user cache even when packaged models exist");
  ::setenv("HM_DEDODE_LIGHTGLUE_ONNX_MODEL", (user_models / dedode).c_str(), 1);
  ok &= expect(
      hm::stitching::feature_matcher_model_override_configured(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue),
      "a matcher-specific DeDoDe path must count as an explicit override");
  const auto overridden_dedode_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue);
  ok &= expect(
      overridden_dedode_asset.ok() && overridden_dedode_asset->empty(),
      "an existing explicit DeDoDe model must bypass automatic asset fetching");
  ::unsetenv("HM_DEDODE_LIGHTGLUE_ONNX_MODEL");
  const auto loftr_path = hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kLoFTR);
  ok &= expect(
      loftr_path.ok() && *loftr_path == package_models / loftr,
      "redistributable LoFTR must use the package model directory");
  const auto packaged_loftr_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kLoFTR);
  ok &= expect(
      packaged_loftr_asset.ok() && *packaged_loftr_asset == "efficient-loftr-outdoor",
      "a stock packaged LoFTR graph must still select its asset declaration for SHA-256 verification");
  const auto packaged_loftr_target =
      hm::stitching::feature_matcher_model_target_path(hm::stitching::ControlPointMatcher::kLoFTR);
  ok &= expect(
      packaged_loftr_target.ok() && *packaged_loftr_target == package_models / loftr,
      "asset provisioning must resolve the same packaged LoFTR target that runtime will consume");
  ok &= expect(
      hm::stitching::bind_feature_matcher_model_path(hm::stitching::ControlPointMatcher::kLoFTR, package_models / loftr)
          .ok(),
      "a verified matcher model path must be bindable for later construction");
  ::setenv("HM_PACKAGED_NATIVE_MODEL_DIR", (root / "different-package-models").c_str(), 1);
  const auto bound_loftr_path = hm::stitching::feature_matcher_model_path(hm::stitching::ControlPointMatcher::kLoFTR);
  ok &= expect(
      bound_loftr_path.ok() && *bound_loftr_path == package_models / loftr,
      "a bound verified matcher path must not drift with later packaged-directory changes");
  ::unsetenv("HM_LOFTR_ONNX_MODEL");
  ::setenv("HM_PACKAGED_NATIVE_MODEL_DIR", package_models.c_str(), 1);

  fs::remove(user_models / dedode);
  const auto missing_dedode_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kDeDoDeLightGlue);
  ok &= expect(
      !missing_dedode_asset.ok(),
      "a missing DeDoDe graph must require local provisioning instead of selecting a download asset");
  fs::remove(user_models / superpoint);
  const auto missing_superpoint_target =
      hm::stitching::feature_matcher_model_target_path(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      missing_superpoint_target.ok() && *missing_superpoint_target == user_models / superpoint,
      "stock model target resolution must work before the asset has been downloaded");
  const auto missing_superpoint_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kSuperPointLightGlue);
  ok &= expect(
      missing_superpoint_asset.ok() && *missing_superpoint_asset == "superpoint-lightglue",
      "a missing stock SuperPoint graph must select its authorized on-demand asset");
  ::setenv("HM_FEATURE_MATCHER_ONNX_MODEL", (root / "missing-superpoint.onnx").c_str(), 1);
  ok &= expect(
      !hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kSuperPointLightGlue).ok(),
      "a missing explicit matcher override must fail instead of silently downloading the stock graph");
  ::unsetenv("HM_FEATURE_MATCHER_ONNX_MODEL");
  const auto akaze_asset =
      hm::stitching::feature_matcher_asset_to_ensure(hm::stitching::ControlPointMatcher::kAkazeHamming);
  ok &= expect(akaze_asset.ok() && akaze_asset->empty(), "AKAZE must never require a model asset");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
