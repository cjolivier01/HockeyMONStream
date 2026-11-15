#include "Bf16Engine.h"

#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
  const char* onnx = std::getenv("HSTREAM_BF16_TEST_ONNX");
  if (!onnx) {
    std::cout << "Set HSTREAM_BF16_TEST_ONNX to a fixed-spatial FP32 detector ONNX to run the GPU test\n";
    return 0;
  }
  auto temporary = (std::filesystem::temp_directory_path() / "hstream-bf16-test-XXXXXX").string();
  if (!::mkdtemp(temporary.data()))
    return 1;
  const auto engine = std::filesystem::path(temporary) / "detector.engine";
  hm::inference::Bf16EngineOptions options{};
  options.batch = 2;
  auto status = hm::inference::BuildBf16Engine(onnx, engine, options);
  if (status.ok())
    status = hm::inference::ValidateBf16Engine(engine, options);
  std::filesystem::remove_all(temporary);
  std::cout << status << '\n';
  return status.ok() ? 0 : 1;
}
