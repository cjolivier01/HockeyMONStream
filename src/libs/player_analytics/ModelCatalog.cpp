#include "hstream/src/libs/player_analytics/ModelCatalog.h"

namespace hm::player_analytics {
namespace {
#include "ModelCatalogData.inc"

constexpr CatalogModel kModels[] = {
    {"reid", "deepstream", "DeepStream tracker default", "", "", "", 32},
    {"pose",
     "rtmpose-m-coco17-256x192",
     "RTMPose-M (COCO 17 joints)",
     "https://github.com/cjolivier01/HockeyMONStream/releases/download/pretrained-assets-v1/"
     "rtmpose-m-coco17-5449a830cd121954.onnx",
     "5449a830cd12195430c2ac66d93854b47ae8be0cfea0f6ffbe2422d9d28983a0",
     kPoseContract,
     8},
    {"jersey",
     "parseq-hockey-cvprw2024",
     "Hockey PARSeq (CC BY-NC 3.0; noncommercial)",
     "https://github.com/cjolivier01/HockeyMONStream/releases/download/pretrained-assets-v1/"
     "parseq-hockey-cvprw2024-df48ad7463aa89a5.onnx",
     "df48ad7463aa89a5e852f9c4cf2616f0cb9a4e7490ef380b0c6ab67f110228b1",
     kJerseyContract,
     8},
    {"action",
     "stgcnpp-coco2d-joint-ntu60",
     "STGCN++ (NTU60 generic activities)",
     "https://github.com/cjolivier01/HockeyMONStream/releases/download/pretrained-assets-v1/"
     "stgcnpp-coco2d-ntu60-a69d882572452c6c.onnx",
     "a69d882572452c6c046d998bd587c39cf614b45844e50d32ec24803ca6a3e9bd",
     kActionContract,
     8},
    {"reid",
     "reidentificationnet-deployable-v1.2",
     "NVIDIA ReIdentificationNet",
     "https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/"
     "files?redirect=true&path=resnet50_market1501_aicity156.onnx",
     "0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b",
     kReidContract,
     32},
};
} // namespace

std::string_view DefaultModelId(std::string_view feature) {
  for (const auto& model : kModels)
    if (feature == model.feature)
      return model.id;
  return {};
}

const CatalogModel* FindModel(std::string_view feature, std::string_view id) {
  for (const auto& model : kModels)
    if (feature == model.feature && id == model.id)
      return &model;
  return nullptr;
}
} // namespace hm::player_analytics
