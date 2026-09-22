#include "EnginePrecision.h"

#include <iostream>

int main() {
  using hm::inference::TensorRtVersionMatches;
  if (!TensorRtVersionMatches(8601, 8, 6) || !TensorRtVersionMatches(9300, 9, 3) ||
      !TensorRtVersionMatches(101601, 10, 16) || !TensorRtVersionMatches(110300, 11, 3) ||
      TensorRtVersionMatches(8601, 9, 6) || TensorRtVersionMatches(9300, 9, 2) ||
      TensorRtVersionMatches(101601, 10, 15) || TensorRtVersionMatches(110300, 10, 3)) {
    std::cerr << "SDK matching must handle both TensorRT version encodings\n";
    return 1;
  }
  using hm::inference::EngineUsesPrecision;
  const auto trt10 = YAML::Load(R"({"Layers":[{"Inputs":[{"Format/Datatype":"BFloat16"}],"Outputs":[]}]})");
  const auto trt11 = YAML::Load(R"({"Layers":[{"Inputs":[],"Outputs":[{"Datatype":"Int8"}]}]})");
  const auto fallback = YAML::Load(
      R"({"Layers":[{"Name":"int8_bf16_conv","Inputs":[{"Datatype":"Float"}],"Outputs":[{"Datatype":"Float"}]}]})");
  if (!EngineUsesPrecision(trt10, "bf16") || EngineUsesPrecision(trt10, "int8") ||
      !EngineUsesPrecision(trt11, "int8") || EngineUsesPrecision(trt11, "bf16") ||
      EngineUsesPrecision(fallback, "int8") || EngineUsesPrecision(fallback, "bf16") ||
      EngineUsesPrecision(YAML::Load("{}"), "int8")) {
    std::cerr << "Engine precision inspection must support TensorRT 10/11 and reject fallback/name-only claims\n";
    return 1;
  }
  return 0;
}
