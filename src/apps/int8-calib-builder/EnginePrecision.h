#pragma once

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace hm::inference {

inline bool EngineUsesPrecision(const YAML::Node& inspector, const std::string& precision) {
  const auto layers = inspector["Layers"];
  if (!layers || !layers.IsSequence())
    return false;
  for (const auto& layer : layers) {
    for (const char* direction : {"Inputs", "Outputs"}) {
      const auto tensors = layer[direction];
      if (!tensors || !tensors.IsSequence())
        continue;
      for (const auto& tensor : tensors) {
        // TensorRT 11 separates the datatype from the tensor format.
        const auto format = tensor["Datatype"] ? tensor["Datatype"] : tensor["Format/Datatype"];
        if (!format || !format.IsScalar())
          continue;
        std::string value = format.as<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        if (precision == "int8"
                ? value.find("int8") != std::string::npos
                : value.find("bfloat16") != std::string::npos || value.find("bf16") != std::string::npos)
          return true;
      }
    }
  }
  return false;
}

} // namespace hm::inference
