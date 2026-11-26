#include "hstream/src/apps/apps-common/DetectorPreprocessingCheck.h"

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"

#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <nvds_version.h>

#include <array>
#include <fstream>
#include <string>
#include <thread>

namespace hm::pipeline {
namespace {

absl::Status Probe(unsigned gpu_id) {
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

  // DeepStream and the CLI can load different libcudart majors. All stream,
  // allocation and launch-error operations must use the SDK's runtime.
#define SDK_CUDA(name) reinterpret_cast<decltype(&::name)>(dlsym(sdk, #name))
  const auto set_device = SDK_CUDA(cudaSetDevice);
  const auto create_stream = SDK_CUDA(cudaStreamCreateWithFlags);
  const auto destroy_stream = SDK_CUDA(cudaStreamDestroy);
  const auto allocate = SDK_CUDA(cudaMalloc);
  const auto release = SDK_CUDA(cudaFree);
  const auto copy = SDK_CUDA(cudaMemcpyAsync);
  const auto synchronize = SDK_CUDA(cudaStreamSynchronize);
  const auto launch_error = SDK_CUDA(cudaGetLastError);
  const auto error_name = SDK_CUDA(cudaGetErrorName);
  const auto error_string = SDK_CUDA(cudaGetErrorString);
#undef SDK_CUDA
  if (!set_device || !create_stream || !destroy_stream || !allocate || !release || !copy || !synchronize ||
      !launch_error || !error_name || !error_string)
    return absl::FailedPreconditionError("Cannot resolve the DeepStream CUDA runtime's preprocessing functions");
  const auto cuda_status = [&](cudaError_t error, const char* operation) {
    if (error == cudaSuccess)
      return absl::OkStatus();
    std::string message =
        absl::StrCat(operation, " failed with ", error_name(error), " (", int(error), "): ", error_string(error));
    if (error == cudaErrorUnsupportedPtxVersion || error == cudaErrorNoKernelImageForDevice) {
      if (message.back() != '.')
        message.push_back('.');
      message += " Use a compatible NVIDIA driver or DeepStream build.";
    }
    return absl::FailedPreconditionError(message);
  };

  if (auto status = cuda_status(set_device(gpu_id), "Select GPU"); !status.ok())
    return status;
  cudaStream_t stream{};
  if (auto status = cuda_status(create_stream(&stream, cudaStreamNonBlocking), "Create probe stream"); !status.ok())
    return status;
  absl::Cleanup close_stream = [&] { destroy_stream(stream); };
  struct Sample {
    float planar[6];
    unsigned char rgb[6];
  };
  void* allocation{};
  if (auto status = cuda_status(allocate(&allocation, sizeof(Sample)), "Allocate probe pixels"); !status.ok())
    return status;
  absl::Cleanup free_sample = [&] { release(allocation); };
  auto* device = static_cast<Sample*>(allocation);
  // Two RGB pixels with an exact binary scale. A kernel that silently fails
  // to launch leaves the known zero output intact.
  Sample input{{}, {2, 4, 6, 8, 10, 12}};
  if (auto status =
          cuda_status(copy(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream), "Upload probe pixels");
      !status.ok())
    return status;
  convert(device->planar, device->rgb, 2, 1, 6, 0.5f, nullptr, stream);
  if (auto status = cuda_status(launch_error(), "DeepStream RGB preprocessing kernel"); !status.ok())
    return status;

  std::array<float, 6> result{};
  if (auto status = cuda_status(
          copy(result.data(), device->planar, sizeof(device->planar), cudaMemcpyDeviceToHost, stream),
          "Read probe tensor");
      !status.ok())
    return status;
  if (auto status = cuda_status(synchronize(stream), "Complete probe stream"); !status.ok())
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
