#include "hstream/src/libs/detection/HipDetector.h"

#include <migraphx/migraphx.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> failed_enqueue{false};
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  bool released{false};
  static void Wait(void* data) {
    auto& gate = *static_cast<Gate*>(data);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.released; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }
};
struct Resources {
  hipStream_t stream{};
  void* image{};
  std::string path;
  ~Resources() {
    if (stream)
      (void)hipStreamSynchronize(stream);
    if (image)
      (void)hipFree(image);
    if (stream)
      (void)hipStreamDestroy(stream);
    if (!path.empty())
      std::filesystem::remove(path);
  }
};
constexpr unsigned char kModel[] = {
    0x08, 0x09, 0x3a, 0x7b, 0x0a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x0a, 0x05, 0x73, 0x68, 0x61, 0x70,
    0x65, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x07, 0x52, 0x65, 0x73, 0x68, 0x61, 0x70, 0x65, 0x12,
    0x07, 0x72, 0x65, 0x73, 0x68, 0x61, 0x70, 0x65, 0x2a, 0x10, 0x08, 0x03, 0x10, 0x07, 0x3a, 0x03, 0x01, 0x02, 0x06,
    0x42, 0x05, 0x73, 0x68, 0x61, 0x70, 0x65, 0x5a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x16, 0x0a,
    0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x03, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02,
    0x08, 0x02, 0x62, 0x1c, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x12, 0x0a, 0x10, 0x08, 0x01, 0x12,
    0x0c, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x06, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11};
} // namespace

// Inject a runtime failure after preprocessing has been submitted, without
// corrupting the HIP context. This interposes the actual SDK call boundary.
extern "C" migraphx_status __wrap_migraphx_program_run_async(
    migraphx_arguments_t*,
    migraphx_program_t,
    migraphx_program_parameters_t,
    void*,
    const char*) {
  failed_enqueue = true;
  return migraphx_status_unknown_error;
}
int main() {
  try {
    Resources resources;
    resources.path = (std::filesystem::temp_directory_path() / "hstream-inference-failure-XXXXXX").string();
    const int fd = mkstemp(resources.path.data());
    Check(fd >= 0, "Create model fixture");
    close(fd);
    {
      std::ofstream out(resources.path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(kModel), sizeof(kModel));
    }
    auto detector = hm::detection::HipDetector::Create(
        {resources.path,
         "input",
         2,
         2,
         1,
         0,
         2,
         1.f / 255,
         .25,
         .45,
         true,
         true,
         hm::gpu::MigraphxSession::Precision::kFloat32,
         4096});
    Check(detector.ok(), detector.status().ToString());
    Hip(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking));
    Hip(hipMalloc(&resources.image, 16));
    Hip(hipMemset(resources.image, 0, 16));
    hm::detection::Image image{
        resources.image, nullptr, 8, 0, 2, 2, hm::detection::PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
    // Load the preprocessing module before blocking the stream. HIP may
    // synchronize while first loading device code into a context.
    float* warmup = nullptr;
    Hip(hipMalloc(reinterpret_cast<void**>(&warmup), 12 * sizeof(float)));
    Hip(hm::detection::PreprocessDetector(image, {2, 2, 2, 2, 0, 0, 1.f / 255}, warmup, resources.stream));
    Hip(hipStreamSynchronize(resources.stream));
    Hip(hipFree(warmup));
    Gate gate;
    Hip(hipLaunchHostFunc(resources.stream, Gate::Wait, &gate));
    auto future = std::async(std::launch::async, [&] { return (*detector)->Detect(image, resources.stream); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!failed_enqueue && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool reached = failed_enqueue;
    const bool retained = reached && future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    gate.Release();
    const auto result = future.get();
    Check(reached, "The fault injector did not reach the inference call");
    Check(retained, "Detect returned an error while borrowed-frame preprocessing was still queued");
    Check(!result.ok(), "The original inference error was not preserved");
    Hip(hipStreamQuery(resources.stream));
    Check(
        absl::IsFailedPrecondition((*detector)->Detect(image, resources.stream).status()),
        "Failed detector must retire");
    std::cout << "Inference failure drains borrowed frame reads before returning\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
