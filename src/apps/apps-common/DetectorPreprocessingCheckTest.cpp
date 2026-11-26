#include "hstream/src/apps/apps-common/DetectorPreprocessingCheck.h"

#include <cuda_runtime_api.h>
#include <dlfcn.h>

#include <cstdlib>
#include <iostream>

namespace {
bool leave_tensor_zero;
}

// Stand in for a conversion routine which returns normally after a failed
// launch. One mode leaves the original zeros, the other corrupts one float.
void NvDsInferConvert_C3ToP3Float(
    float* output,
    unsigned char*,
    unsigned,
    unsigned,
    unsigned,
    float,
    float*,
    cudaStream_t stream) {
  if (!leave_tensor_zero && cudaMemsetAsync(output, 1, sizeof(float), stream) != cudaSuccess)
    std::abort();
}

int main() {
  // Match the production lifetime: the nvinfer plugin loads its SDK locally.
  void* sdk = dlopen("libnvds_infer.so", RTLD_NOW | RTLD_LOCAL);
  if (!sdk) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  for (bool zero : {true, false}) {
    leave_tensor_zero = zero;
    const auto status = hm::pipeline::CheckDetectorPreprocessing(0);
    if (status.ok() || status.message().find("Player detection could not start on GPU 0") == absl::string_view::npos ||
        status.message().find("DeepStream") == absl::string_view::npos ||
        status.message().find(zero ? "all zeros" : "incorrect values") == absl::string_view::npos) {
      std::cerr << "Unexpected preprocessing result: " << status << '\n';
      return 1;
    }
  }
  dlclose(sdk);
}
