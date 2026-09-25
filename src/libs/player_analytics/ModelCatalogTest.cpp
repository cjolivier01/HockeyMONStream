#include "hstream/src/libs/player_analytics/ModelCatalog.h"
#include "hstream/src/libs/player_analytics/ModelContract.h"

#include <fstream>
#include <iostream>
#include <string>

namespace pa = hm::player_analytics;

int main(int argc, char** argv) {
  if (argc != 5)
    return 1;
  const char* features[] = {"pose", "jersey", "action", "reid"};
  for (int index = 0; index < 4; ++index) {
    const std::string id =
        index == 3 ? "reidentificationnet-deployable-v1.2" : std::string(pa::DefaultModelId(features[index]));
    const auto* model = pa::FindModel(features[index], id);
    if (!model)
      return 2;
    std::ifstream input(argv[index + 1]);
    const std::string document{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input || document != model->contract_json) {
      std::cerr << "Embedded catalog differs from the distributed contract for " << id << '\n';
      return 3;
    }
    auto manifest = YAML::Load(document);
    if (manifest["onnx"]["sha256"].as<std::string>() != model->onnx_sha256 ||
        manifest["model_id"].as<std::string>() != id)
      return 4;
    if (index == 3) {
      if (manifest["tracker"]["ReID"]["batchSize"].as<int>() != model->max_batch ||
          manifest["tracker"]["ReID"]["keepAspc"].as<int>() != 0)
        return 5;
      continue;
    }
    manifest["engine"] = YAML::Load(
        "{file: model.engine, tensorrt_version: '10.3.0', tensorrt_build_version: 30, "
        "cuda_runtime_version: 12060, gpu_name: Test GPU, compute_capability: '8.7', precision: fp16}");
    manifest["engine"]["sha256"] = std::string(64, 'a');
    manifest["engine"]["max_batch"] = model->max_batch;
    const auto parsed = pa::ParseModelManifest(manifest);
    if (!parsed.ok()) {
      std::cerr << id << ": " << parsed.status() << '\n';
      return 6;
    }
  }
  if (pa::DefaultModelId("reid") != "deepstream" || !pa::FindModel("reid", "deepstream") ||
      pa::FindModel("pose", "reidentificationnet-deployable-v1.2"))
    return 7;
  return 0;
}
