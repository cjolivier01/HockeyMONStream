#include "hstream/src/apps/apps-common/DetectorPreprocessingCheck.h"

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"

#include <cuda_runtime.h>
#include <dlfcn.h>
#include <nvds_version.h>

#include <array>
#include <fstream>
#include <string>
#include <thread>

namespace hm::pipeline {
namespace {

absl::Status CudaStatus(cudaError_t error, const char* operation) {
  if (error == cudaSuccess)
    return absl::OkStatus();
  std::string message = absl::StrCat(
      operation, " failed with ", cudaGetErrorName(error), " (", int(error), "): ", cudaGetErrorString(error));
  if (error == cudaErrorUnsupportedPtxVersion || error == cudaErrorNoKernelImageForDevice) {
    if (message.back() != '.')
      message.push_back('.');
    message += " Use a compatible NVIDIA driver or DeepStream build.";
  }
  return absl::FailedPreconditionError(message);
}

absl::Status Probe(unsigned gpu_id) {
  if (auto status = CudaStatus(cudaSetDevice(gpu_id), "Select GPU"); !status.ok())
    return status;
  // nvinfer has already loaded the SDK. Keep its TensorRT dependencies local,
  // and honor interposition exactly as a normal SDK kernel call does.
  void* sdk = dlopen("libnvds_infer.so", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  if (!sdk)
    return absl::FailedPreconditionError(absl::StrCat("Cannot inspect the loaded DeepStream library: ", dlerror()));
  absl::Cleanup close_sdk = [&] { dlclose(sdk); };
  // C++ ABI of NvDsInferConvert_C3ToP3Float in NVIDIA's nvdsinfer_conversion.h.
  constexpr char symbol[] = "_Z28NvDsInferConvert_C3ToP3FloatPfPhjjjfS_P11CUstream_st";
  using Convert = void (*)(float*, unsigned char*, unsigned, unsigned, unsigned, float, float*, cudaStream_t);
  auto convert = reinterpret_cast<Convert>(dlsym(RTLD_DEFAULT, symbol));
  if (!convert)
    convert = reinterpret_cast<Convert>(dlsym(sdk, symbol));
  if (!convert)
    return absl::FailedPreconditionError("Loaded DeepStream library lacks NvDsInferConvert_C3ToP3Float");

  // The CLI and DeepStream can use different libcudart majors. Check the launch
  // on the same host thread, using the SDK's own CUDA last-error slot.
  auto launch_error = reinterpret_cast<cudaError_t (*)()>(dlsym(sdk, "cudaGetLastError"));
  if (!launch_error)
    return absl::FailedPreconditionError("Cannot inspect the DeepStream CUDA runtime's launch status");

  cudaStream_t stream{};
  if (auto status = CudaStatus(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "Create probe stream");
      !status.ok())
    return status;
  absl::Cleanup close_stream = [&] { cudaStreamDestroy(stream); };
  struct Sample {
    float planar[6];
    unsigned char rgb[6];
  };
  Sample* device{};
  if (auto status = CudaStatus(cudaMalloc(&device, sizeof(Sample)), "Allocate probe pixels"); !status.ok())
    return status;
  absl::Cleanup free_sample = [&] { cudaFree(device); };
  // Two RGB pixels with an exact binary scale. A kernel that silently fails
  // to launch leaves the known zero output intact.
  Sample input{{}, {2, 4, 6, 8, 10, 12}};
  if (auto status = CudaStatus(
          cudaMemcpyAsync(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream), "Upload probe pixels");
      !status.ok())
    return status;
  convert(device->planar, device->rgb, 2, 1, 6, 0.5f, nullptr, stream);
  if (auto status = CudaStatus(launch_error(), "DeepStream RGB preprocessing kernel"); !status.ok())
    return status;

  std::array<float, 6> result{};
  if (auto status = CudaStatus(
          cudaMemcpyAsync(result.data(), device->planar, sizeof(device->planar), cudaMemcpyDeviceToHost, stream),
          "Read probe tensor");
      !status.ok())
    return status;
  if (auto status = CudaStatus(cudaStreamSynchronize(stream), "Complete probe stream"); !status.ok())
    return status;
  if (result == std::array<float, 6>{})
    return absl::FailedPreconditionError("DeepStream preprocessing returned all zeros for a nonzero RGB test image");
  if (result != std::array<float, 6>{1, 4, 2, 5, 3, 6})
    return absl::FailedPreconditionError("DeepStream preprocessing returned incorrect values for the RGB test image");
  return absl::OkStatus();
}
} // namespace

absl::Status CheckDetectorPreprocessing(unsigned gpu_id) {
  // Isolate device selection and the per-thread CUDA error slot from playback.
  absl::Status status;
  std::thread worker([&] {
    status = Probe(gpu_id);
    if (status.ok())
      return;
    unsigned major{}, minor{};
    nvds_version(&major, &minor);
    cudaDeviceProp gpu{};
    std::string driver;
    std::ifstream("/sys/module/nvidia/version") >> driver;
    std::string context = absl::StrCat("Player detection could not start on GPU ", gpu_id);
    if (cudaGetDeviceProperties(&gpu, gpu_id) == cudaSuccess)
      absl::StrAppend(&context, " (", gpu.name, ")");
    if (!driver.empty())
      absl::StrAppend(&context, ", NVIDIA driver ", driver);
    status = absl::FailedPreconditionError(
        absl::StrCat(context, ", DeepStream ", major, ".", minor, ": ", status.message()));
  });
  worker.join();
  return status;
}
} // namespace hm::pipeline
