#include "hstream/src/libs/stitching/CalibrationModels.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

absl::StatusOr<std::filesystem::path> model_path(
    const char* override_name,
    const char* content_addressed_name,
    bool may_use_packaged_model = true,
    bool automatic_download_available = true) {
  std::filesystem::path path;
  if (const char* override_path = std::getenv(override_name); override_path && *override_path) {
    path = override_path;
  } else if (const char* model_dir = std::getenv("HM_NATIVE_MODEL_DIR"); model_dir && *model_dir) {
    path = std::filesystem::path(model_dir) / content_addressed_name;
  } else if (const char* model_dir = std::getenv("HM_PACKAGED_NATIVE_MODEL_DIR");
             may_use_packaged_model && model_dir && *model_dir) {
    path = std::filesystem::path(model_dir) / content_addressed_name;
  } else {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
      return absl::NotFoundError(std::string(override_name) + " is unset and HOME is unavailable");
    }
    path = std::filesystem::path(home) / ".cache" / "hstream" / "models" / content_addressed_name;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    const std::string directory_overrides =
        may_use_packaged_model ? "/HM_NATIVE_MODEL_DIR/HM_PACKAGED_NATIVE_MODEL_DIR" : "/HM_NATIVE_MODEL_DIR";
    const std::string remediation = automatic_download_available
        ? "; run hstream-assets on configs/ds_hockey_configure_stitching.yaml or set "
        : "; export or provide the model locally and set ";
    return absl::NotFoundError(
        "Missing native calibration model " + path.string() + remediation + override_name + directory_overrides);
  }
  if (std::filesystem::file_size(path, error) == 0 || error) {
    return absl::FailedPreconditionError("Native calibration model is empty: " + path.string());
  }
  return path;
}

const char* feature_matcher_override_name(ControlPointMatcher matcher) {
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return "HM_FEATURE_MATCHER_ONNX_MODEL";
    case ControlPointMatcher::kDeDoDeLightGlue:
      return "HM_DEDODE_LIGHTGLUE_ONNX_MODEL";
    case ControlPointMatcher::kLoFTR:
      return "HM_LOFTR_ONNX_MODEL";
    case ControlPointMatcher::kAkazeHamming:
      return nullptr;
  }
  return nullptr;
}

} // namespace

absl::StatusOr<std::filesystem::path> rink_model_path() {
  return model_path("HM_RINK_ONNX_MODEL", "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx", false);
}

bool feature_matcher_model_override_configured(ControlPointMatcher matcher) {
  const char* override_name = feature_matcher_override_name(matcher);
  if (override_name == nullptr)
    return false;
  const char* override_path = std::getenv(override_name);
  const char* model_dir = std::getenv("HM_NATIVE_MODEL_DIR");
  return (override_path != nullptr && *override_path != '\0') || (model_dir != nullptr && *model_dir != '\0');
}

absl::StatusOr<std::filesystem::path> feature_matcher_model_path(ControlPointMatcher matcher) {
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "superpoint-lightglue-pipeline-228994cea8c01014.onnx", false);
    case ControlPointMatcher::kDeDoDeLightGlue:
      return model_path(
          "HM_DEDODE_LIGHTGLUE_ONNX_MODEL", "dedode-lightglue-lc4v2-bupright-f8bd053e44d57a77.onnx", false, false);
    case ControlPointMatcher::kLoFTR:
      return model_path("HM_LOFTR_ONNX_MODEL", "efficient-loftr-outdoor-opt-a2cbdcfef0ddb5cd.onnx");
    case ControlPointMatcher::kAkazeHamming:
      return std::filesystem::path();
  }
  return absl::InvalidArgumentError("Unknown native control-point matcher");
}

absl::StatusOr<std::string> feature_matcher_asset_to_ensure(ControlPointMatcher matcher) {
  if (matcher == ControlPointMatcher::kAkazeHamming)
    return std::string();
  auto model = feature_matcher_model_path(matcher);
  if (feature_matcher_model_override_configured(matcher)) {
    if (!model.ok())
      return model.status();
    return std::string();
  }
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      return "superpoint-lightglue";
    case ControlPointMatcher::kLoFTR:
      return "efficient-loftr-outdoor";
    case ControlPointMatcher::kDeDoDeLightGlue:
      if (!model.ok())
        return model.status();
      return std::string();
    case ControlPointMatcher::kAkazeHamming:
      return std::string();
  }
  return absl::InvalidArgumentError("Unknown native control-point matcher");
}

} // namespace hm::stitching
