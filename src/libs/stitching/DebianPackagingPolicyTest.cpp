#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
std::string read(const char* path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, const std::string& token) {
  return text.find(token) != std::string::npos;
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "FAIL: expected .bazelrc, Dockerfile, builder, packager, and installer paths\n";
    return 1;
  }
  const std::string bazelrc = read(argv[1]);
  const std::string dockerfile = read(argv[2]);
  const std::string builder = read(argv[3]);
  const std::string packager = read(argv[4]);
  const std::string installer = read(argv[5]);
  bool ok = true;
  ok &= expect(
      contains(bazelrc, "build:deb_ubuntu24 --repo_env=CUDA_PATH=/usr/local/cuda-13.2") &&
          !contains(bazelrc, "build:deb_ubuntu24 --repo_env=CUDA_PATH=/usr/local/cuda-12"),
      "Ubuntu 24 package config must use the same CUDA 13.2 ABI as DeepStream 9.1");
  ok &= expect(
      contains(dockerfile, "cuda-compiler-13-2") && !contains(dockerfile, "cuda-compiler-12") &&
          contains(dockerfile, "\"libnvinfer-headers-dev=${trt_version}\"") &&
          contains(dockerfile, "\"libnvinfer10=${trt_version}\"") && !contains(dockerfile, "Pin-Priority"),
      "package builder must use CUDA 13.2 and transaction-local TensorRT pins without a broad APT pin");
  ok &= expect(
      contains(builder, "TARGET_CUDA_ROOT=/usr/local/cuda-13.2") &&
          !contains(builder, "TARGET_CUDA_ROOT=/usr/local/cuda-12"),
      "every target-OS build must select CUDA 13.2");
  ok &= expect(
      contains(packager, "X-HMStream-Target-Ubuntu: ${TARGET_UBUNTU}") &&
          contains(packager, "CUDA 12 dependency entered the CUDA 13.2 HMStream package"),
      "package must carry explicit OS provenance and reject CUDA 12 ELFs");
  ok &= expect(
      contains(installer, "X-HMStream-Target-Ubuntu") && !contains(installer, "libc6 (>= 2.43)") &&
          !contains(installer, "Pin-Priority"),
      "installer must validate explicit OS provenance without ABI heuristics or global TensorRT pins");
  return ok ? 0 : 1;
}
