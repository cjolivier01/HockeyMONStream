#include "hstream/src/libs/player_analytics/ModelContract.h"

#include <iostream>
#include <string>

namespace pa = hm::player_analytics;

YAML::Node PoseManifest() {
  auto node = YAML::Load(R"(
schema_version: 1
feature: pose
model_family: rtmpose
model_id: reference-pose
source: {url: 'https://example.invalid/model.pth', license: Apache-2.0}
onnx: {file: model.onnx, opset: 17}
engine: {file: model.engine, tensorrt_version: '10.3.0', tensorrt_build_version: 1,
         cuda_runtime_version: 12060, gpu_name: Example GPU, compute_capability: '8.7', precision: fp16, max_batch: 8}
inputs: [{name: image, dtype: float32, shape: [-1,3,256,192]}]
outputs: [{name: simcc_x, dtype: float32, shape: [-1,17,384]},
          {name: simcc_y, dtype: float32, shape: [-1,17,512]}]
topology_id: coco17
preprocessing: {id: rtmpose-coco17-affine-v1, color: rgb,
                mean: [123.675,116.28,103.53], std: [58.395,57.12,57.375],
                padding: 1.25, simcc_split_ratio: 2, flip_test: false}
)");
  for (const char* section : {"source", "onnx", "engine"})
    node[section]["sha256"] = std::string(64, 'a');
  node["source"]["config_sha256"] = std::string(64, 'b');
  return node;
}

int main(int argc, char** argv) {
  auto node = PoseManifest();
  auto parsed = pa::ParseModelManifest(node);
  if (!parsed.ok()) {
    std::cerr << parsed.status() << '\n';
    return 1;
  }
  if (!pa::ValidateRuntimeIdentity(*parsed, parsed->engine_identity).ok())
    return 2;
  auto other_runtime = parsed->engine_identity;
  other_runtime.cuda_runtime_version++;
  if (pa::ValidateRuntimeIdentity(*parsed, other_runtime).ok())
    return 3;
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto invalid = YAML::Clone(node);
    switch (mutation) {
      case 0:
        invalid["outputs"][0]["shape"][1] = 18;
        break;
      case 1:
        invalid["engine"]["max_batch"] = 9;
        break;
      case 2:
        invalid["preprocessing"]["padding"] = 1;
        break;
      case 3:
        invalid["engine"]["file"] = "../other.engine";
        break;
      case 4:
        invalid["onnx"]["sha256"] = "not-a-hash";
        break;
      case 5:
        invalid["preprocessing"]["flip_test"] = true;
        break;
      case 6:
        invalid["outputs"][1]["name"] = "simcc_x";
        break;
      case 7:
        invalid["preprocessing"]["typo"] = true;
        break;
    }
    if (pa::ParseModelManifest(invalid).ok()) {
      std::cerr << "invalid manifest mutation accepted: " << mutation << '\n';
      return 4;
    }
  }
  auto jersey = YAML::Clone(node);
  jersey["feature"] = "jersey";
  jersey["model_family"] = "parseq";
  jersey.remove("topology_id");
  jersey["inputs"] = YAML::Load("[{name: image, dtype: float32, shape: [-1,3,32,128]}]");
  jersey["outputs"] = YAML::Load("[{name: logits, dtype: float32, shape: [-1,3,95]}]");
  jersey["preprocessing"] = YAML::Load(R"({id: parseq-rgb-bicubic-v1, color: rgb, input_range: [-1,1],
      interpolation: bicubic, align_corners: false, antialias: true, cubic_coefficient: -0.5,
      resize_coordinate_mode: half_pixel, quantize_uint8_after_resize: true, max_length: 2, eos_index: 0})");
  auto charset = jersey["preprocessing"]["charset"];
  charset.push_back("[E]");
  for (char c = '!'; c <= '~'; ++c)
    charset.push_back(std::string(1, c)); // Deliberately not digit-first: use actual mapping.
  if (!pa::ParseModelManifest(jersey).ok())
    return 5;
  jersey["preprocessing"]["eos_index"] = 1;
  if (pa::ParseModelManifest(jersey).ok())
    return 6;

  auto action = YAML::Clone(node);
  action["feature"] = "action";
  action["model_family"] = "stgcnpp";
  action["inputs"] = YAML::Load("[{name: skeleton, dtype: float32, shape: [-1,2,100,17,3]}]");
  action["outputs"] = YAML::Load("[{name: logits, dtype: float32, shape: [-1,60]}]");
  action["preprocessing"] = YAML::Load(R"({id: stgcnpp-coco17-causal-v1,
      coordinate_normalization: full_canvas_minus1_plus1, sample_period_ns: 100000000,
      max_gap_ns: 150000000, samples: 100, second_person: zeros})");
  for (int i = 0; i < 60; ++i)
    action["labels"].push_back("label " + std::to_string(i));
  if (!pa::ParseModelManifest(action).ok())
    return 7;
  action["preprocessing"]["max_gap_ns"] = 1000000000;
  if (pa::ParseModelManifest(action).ok())
    return 8;
  // Optional real prepared-bundle fixture, supplied only by the offline parity
  // workflow. No model file or runtime is required for ordinary CPU tests.
  for (int i = 1; i < argc; ++i) {
    const auto actual = pa::ParseModelManifest(YAML::LoadFile(argv[i]));
    if (!actual.ok()) {
      std::cerr << argv[i] << ": " << actual.status() << '\n';
      return 9;
    }
  }
  return 0;
}
