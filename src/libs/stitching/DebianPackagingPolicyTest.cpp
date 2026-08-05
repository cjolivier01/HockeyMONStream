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
  if (argc != 7) {
    std::cerr << "FAIL: expected .bazelrc, Dockerfile, builder, packager, Docker runner, and installer paths\n";
    return 1;
  }
  const std::string bazelrc = read(argv[1]);
  const std::string dockerfile = read(argv[2]);
  const std::string builder = read(argv[3]);
  const std::string packager = read(argv[4]);
  const std::string docker_runner = read(argv[5]);
  const std::string installer = read(argv[6]);
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
          contains(packager, "CUDA 12 dependency entered the CUDA 13.2 HMStream package") &&
          contains(packager, "pretrained/native-calibration") && contains(packager, "model_cache_root"),
      "package must carry OS provenance, reject CUDA 12 ELFs, and stage verified native models");
  ok &= expect(
      contains(docker_runner, ":/root/.cache/hmstream/models:ro") &&
          contains(docker_runner, "HMSTREAM_MODEL_CACHE_DIR"),
      "immutable Docker build must expose the content-addressed native model cache read-only");
  ok &= expect(
      contains(installer, "X-HMStream-Target-Ubuntu") && !contains(installer, "libc6 (>= 2.43)") &&
          !contains(installer, "Pin-Priority") && contains(installer, "old_deepstream_packages") &&
          contains(installer, "^deepstream-[0-9]+([.][0-9]+)*$") &&
          contains(installer, "deepstream-9.1-transition.deb") && contains(installer, "Conflicts") &&
          contains(installer, "Replaces") && !contains(installer, "apt-get remove -y --no-install-recommends") &&
          !contains(installer, "nccl") && !contains(installer, "NCCL"),
      "installer must validate OS provenance, replace older DeepStream atomically, and leave NCCL policy untouched");
  ok &= expect(
      contains(installer, "hmstream-cuda-ubuntu2404-compat.gpg") &&
          contains(installer, "disable_cuda_compat_sources") &&
          contains(installer, "disable_installer_managed_cuda_sources") &&
          contains(installer, "CUDA_LEGACY_COMPAT_SOURCE") && contains(installer, "publish_cuda_compat_source") &&
          contains(installer, "restore_compat_source_transition") && contains(installer, "sync -d") &&
          contains(installer, "sync -f") && contains(installer, "Enabled: no") &&
          !contains(installer, "rollback_transaction") &&
          !contains(installer, "install -m 0644 \"${combined_keyring}\" /usr/share/keyrings/cuda-archive-keyring.gpg"),
      "Ubuntu 26 installer must own its key and interruption-safely replace duplicate compatibility sources");
  return ok ? 0 : 1;
}
