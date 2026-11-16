// This target runs without a GPU. Its dependencies must all choose CUDA even
// when the surrounding build requests HIP through either legacy backend flag.
#include "hstream/src/libs/gpu/Runtime.h"

#include "cupano/gpu/gpu_runtime.h"
#include "cudaUtility.h"

#if defined(HSTREAM_USE_HIP) || defined(JETSON_USE_HIP) || defined(JUT_INCLUDE_HIP_HEADERS)
#error HIP definitions crossed a CUDA-only dependency boundary
#endif

#if !GPU_BACKEND_CUDA || GPU_BACKEND_HIP
#error hm-cupano did not select CUDA inside the dependency boundary
#endif

int main() {
  return 0;
}
