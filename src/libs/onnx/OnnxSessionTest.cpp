#include "hstream/src/libs/onnx/OnnxSession.h"

#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {

constexpr unsigned char kIdentityModel[] = {
    0x08, 0x09, 0x3a, 0x58, 0x0a, 0x19, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x06, 0x6f, 0x75,
    0x74, 0x70, 0x75, 0x74, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x12, 0x08, 0x69,
    0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x5a, 0x17, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12,
    0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x18,
    0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a,
    0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

// A two-input Add graph with float32 [1, 2] inputs. Keeping this tiny fixture
// in the test exercises the same multi-input Session path used by LightGlue
// without checking a trained model into the repository.
constexpr unsigned char kAddModel[] = {
    0x08, 0x09, 0x3a, 0x71, 0x0a, 0x1f, 0x0a, 0x04, 0x6c, 0x65, 0x66, 0x74, 0x0a, 0x05, 0x72, 0x69, 0x67, 0x68,
    0x74, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x1a, 0x03, 0x61, 0x64, 0x64, 0x22, 0x03, 0x41, 0x64,
    0x64, 0x12, 0x03, 0x61, 0x64, 0x64, 0x5a, 0x16, 0x0a, 0x04, 0x6c, 0x65, 0x66, 0x74, 0x12, 0x0e, 0x0a, 0x0c,
    0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x5a, 0x17, 0x0a, 0x05, 0x72, 0x69,
    0x67, 0x68, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08,
    0x02, 0x62, 0x18, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12,
    0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

// An outer product requests a 640 GB CUDA intermediate and returns a [1, 2] slice.
// The allocation cannot fit the test GPU; the CPU fallback uses the tiny Add
// graph above, so this exercises a real ORT OOM without exhausting host RAM.
constexpr unsigned char kCudaOomModel[] = {
    0x08, 0x09, 0x3a, 0xf1, 0x02, 0x0a, 0x17, 0x0a, 0x04, 0x6c, 0x65, 0x66, 0x74, 0x0a, 0x05, 0x72, 0x69, 0x67, 0x68,
    0x74, 0x12, 0x03, 0x73, 0x75, 0x6d, 0x22, 0x03, 0x41, 0x64, 0x64, 0x0a, 0x21, 0x0a, 0x03, 0x73, 0x75, 0x6d, 0x0a,
    0x06, 0x73, 0x74, 0x61, 0x72, 0x74, 0x73, 0x0a, 0x03, 0x6f, 0x6e, 0x65, 0x12, 0x06, 0x73, 0x63, 0x61, 0x6c, 0x61,
    0x72, 0x22, 0x05, 0x53, 0x6c, 0x69, 0x63, 0x65, 0x0a, 0x26, 0x0a, 0x06, 0x73, 0x63, 0x61, 0x6c, 0x61, 0x72, 0x0a,
    0x0c, 0x63, 0x6f, 0x6c, 0x75, 0x6d, 0x6e, 0x5f, 0x73, 0x68, 0x61, 0x70, 0x65, 0x12, 0x06, 0x63, 0x6f, 0x6c, 0x75,
    0x6d, 0x6e, 0x22, 0x06, 0x45, 0x78, 0x70, 0x61, 0x6e, 0x64, 0x0a, 0x27, 0x0a, 0x06, 0x63, 0x6f, 0x6c, 0x75, 0x6d,
    0x6e, 0x12, 0x03, 0x72, 0x6f, 0x77, 0x22, 0x09, 0x54, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x6f, 0x73, 0x65, 0x2a, 0x0d,
    0x0a, 0x04, 0x70, 0x65, 0x72, 0x6d, 0x40, 0x01, 0x40, 0x00, 0xa0, 0x01, 0x07, 0x0a, 0x1c, 0x0a, 0x06, 0x63, 0x6f,
    0x6c, 0x75, 0x6d, 0x6e, 0x0a, 0x03, 0x72, 0x6f, 0x77, 0x12, 0x05, 0x6c, 0x61, 0x72, 0x67, 0x65, 0x22, 0x06, 0x4d,
    0x61, 0x74, 0x4d, 0x75, 0x6c, 0x0a, 0x24, 0x0a, 0x05, 0x6c, 0x61, 0x72, 0x67, 0x65, 0x0a, 0x06, 0x73, 0x74, 0x61,
    0x72, 0x74, 0x73, 0x0a, 0x04, 0x65, 0x6e, 0x64, 0x73, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x05,
    0x53, 0x6c, 0x69, 0x63, 0x65, 0x12, 0x08, 0x63, 0x75, 0x64, 0x61, 0x5f, 0x6f, 0x6f, 0x6d, 0x2a, 0x18, 0x08, 0x02,
    0x10, 0x07, 0x3a, 0x04, 0x80, 0xb5, 0x18, 0x01, 0x42, 0x0c, 0x63, 0x6f, 0x6c, 0x75, 0x6d, 0x6e, 0x5f, 0x73, 0x68,
    0x61, 0x70, 0x65, 0x2a, 0x10, 0x08, 0x02, 0x10, 0x07, 0x3a, 0x02, 0x00, 0x00, 0x42, 0x06, 0x73, 0x74, 0x61, 0x72,
    0x74, 0x73, 0x2a, 0x0d, 0x08, 0x02, 0x10, 0x07, 0x3a, 0x02, 0x01, 0x01, 0x42, 0x03, 0x6f, 0x6e, 0x65, 0x2a, 0x0e,
    0x08, 0x02, 0x10, 0x07, 0x3a, 0x02, 0x01, 0x02, 0x42, 0x04, 0x65, 0x6e, 0x64, 0x73, 0x5a, 0x16, 0x0a, 0x04, 0x6c,
    0x65, 0x66, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02,
    0x5a, 0x17, 0x0a, 0x05, 0x72, 0x69, 0x67, 0x68, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02,
    0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x18, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a,
    0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

hm::onnx::TensorContract input_contract() {
  return {"input", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}};
}

hm::onnx::TensorContract output_contract() {
  return {"output", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}};
}

} // namespace

int main() {
  bool ok = true;
  // This must precede every CPU session: loading a CPU model first hides a
  // missing ORT environment when CUDA registers its provider/logger.
  if (const char* require_cuda = std::getenv("HM_REQUIRE_CUDA_TESTS");
      require_cuda && std::string(require_cuda) == "1") {
    std::string path = (std::filesystem::temp_directory_path() / "hstream-cuda-first-XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    if (fd < 0)
      return 1;
    ::close(fd);
    {
      std::ofstream model(path, std::ios::binary);
      model.write(reinterpret_cast<const char*>(kAddModel), sizeof(kAddModel));
    }
    auto first = hm::onnx::Session::Create(
        path,
        {{"left", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}}, {"right", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}}},
        {output_contract()},
        false,
        hm::onnx::ExecutionProvider::kCuda);
    std::filesystem::remove(path);
    if (!first.ok()) {
      std::cerr << "FAIL: CUDA must initialize before any CPU session: " << first.status() << '\n';
      return 1;
    }
    const float left[] = {1.25f, -2.0f};
    const float right[] = {3.0f, 0.5f};
    auto result = (*first)->RunFloatInputs({{"left", {1, 2}, left, 2}, {"right", {1, 2}, right, 2}});
    if (!result.ok()) {
      std::cerr << result.status() << '\n';
      return 1;
    }
    auto values = result->front().float_data();
    ok &= expect(
        values.ok() && (*values)[0] == 4.25f && (*values)[1] == -1.5f,
        "first-session CUDA inference must use both inputs");
  }

  using hm::onnx::ExecutionProvider;
  using hm::onnx::IsCudaOutOfMemory;
  const std::string arena_oom =
      "onnxruntime::BFCArena::AllocateRawInternal Failed to allocate memory for requested buffer of size 265420800";
  for (const std::string& diagnostic :
       {arena_oom,
        std::string("CUDA failure 2: cudaErrorMemoryAllocation"),
        std::string("CUDA failure 2: out of memory; GPU=0; expr=cudaMalloc"),
        std::string("CUDA failure: 2 ; GPU=0 ; expr=cudaMalloc"),
        std::string("CUDA failure: status=2"),
        std::string("CUDA_ERROR_OUT_OF_MEMORY"),
        std::string("CUDNN_STATUS_ALLOC_FAILED")}) {
    ok &= expect(
        IsCudaOutOfMemory(diagnostic, ExecutionProvider::kCuda, false),
        "CUDA memory exhaustion must qualify for fallback");
    ok &= expect(
        !IsCudaOutOfMemory(diagnostic, ExecutionProvider::kCpu, false),
        "CPU failures must never retry or switch providers");
  }
  ok &= expect(
      !IsCudaOutOfMemory(arena_oom, ExecutionProvider::kCuda, true),
      "an ambiguous CPU/GPU arena error must not be treated as GPU OOM");
  for (const char* diagnostic :
       {"CUDA failure: 200",
        "CUDA failure: status=200",
        "CUDA failure: 700: illegal memory access",
        "CUDNN_STATUS_NOT_SUPPORTED",
        "Failed to load CUDA provider",
        "bad_alloc",
        "CPU allocator failed to allocate memory",
        "Invalid model input shape"}) {
    ok &= expect(
        !IsCudaOutOfMemory(diagnostic, ExecutionProvider::kCuda, false),
        "non-OOM CUDA, model, and host-memory failures must not qualify for fallback");
  }
  if (const char* require_cuda = std::getenv("HM_REQUIRE_CUDA_TESTS");
      require_cuda && std::string(require_cuda) == "1") {
    const auto dir =
        std::filesystem::temp_directory_path() / ("hstream-cpu-fallback-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto gpu_path = dir / "cuda.onnx";
    const auto cpu_path = dir / "cpu.onnx";
    std::ofstream(gpu_path, std::ios::binary)
        .write(reinterpret_cast<const char*>(kCudaOomModel), sizeof(kCudaOomModel));
    std::ofstream(cpu_path, std::ios::binary).write(reinterpret_cast<const char*>(kAddModel), sizeof(kAddModel));
    int fallbacks = 0;
    bool cancel = false;
    auto create = [&](const std::filesystem::path& gpu,
                      const std::filesystem::path& cpu,
                      const std::function<void()>& notification) {
      return hm::onnx::Session::Create(
          gpu.string(),
          {{"left", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}},
           {"right", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}}},
          {output_contract()},
          false,
          ExecutionProvider::kCuda,
          {},
          {cpu.string(), notification});
    };
    const float left[] = {1.25f, -2.0f};
    const float right[] = {3.0f, 0.5f};
    const std::vector<hm::onnx::FloatInput> inputs{{"left", {1, 2}, left, 2}, {"right", {1, 2}, right, 2}};
    auto fallback = create(gpu_path, cpu_path, [&] { ++fallbacks; });
    ok &= expect(fallback.ok(), "CUDA OOM fixture must load successfully before inference");
    if (fallback.ok()) {
      ok &= expect((*fallback)->execution_provider() == ExecutionProvider::kCuda, "GPU must be attempted first");
      auto cancelled = (*fallback)->RunFloatInputs(inputs, [] { return true; });
      ok &= expect(absl::IsCancelled(cancelled.status()) && fallbacks == 0, "cancellation must not trigger fallback");
      for (int attempt = 0; attempt < 2; ++attempt) {
        auto result = (*fallback)->RunFloatInputs(inputs);
        if (!result.ok())
          std::cerr << result.status() << '\n';
        ok &= expect(result.ok(), "GPU OOM must retry the current input on CPU and allow later inputs");
        if (result.ok()) {
          auto values = result->front().float_data();
          ok &= expect(
              values.ok() && (*values)[0] == 4.25f && (*values)[1] == -1.5f,
              "CPU fallback must use the same inputs and the CPU-specific graph");
        }
        ok &= expect(
            fallbacks == 1 && (*fallback)->execution_provider() == ExecutionProvider::kCpu,
            "fallback must happen once and retain the CPU session");
      }
    }
    auto invalid = create(dir / "missing.onnx", cpu_path, [&] { ++fallbacks; });
    ok &= expect(!invalid.ok() && fallbacks == 1, "missing CUDA model must not be hidden by CPU fallback");
    auto cpu_failure = create(gpu_path, dir / "missing-cpu.onnx", [&] { ++fallbacks; });
    ok &= expect(cpu_failure.ok(), "missing CPU fallback graph must not prevent GPU session creation");
    if (cpu_failure.ok()) {
      const auto first = (*cpu_failure)->RunFloatInputs(inputs);
      const auto second = (*cpu_failure)->RunFloatInputs(inputs);
      ok &= expect(
          !first.ok() && second.status() == first.status() && fallbacks == 2,
          "CPU loading failures must remain terminal without repeated GPU/CPU attempts");
    }
    auto cancelled_retry = create(gpu_path, cpu_path, [&] { cancel = true; });
    ok &= expect(cancelled_retry.ok(), "cancellable fallback fixture must load");
    if (cancelled_retry.ok()) {
      auto result = (*cancelled_retry)->RunFloatInputs(inputs, [&] { return cancel; });
      ok &= expect(absl::IsCancelled(result.status()), "cancellation during fallback must prevent CPU inference");
    }
    std::filesystem::remove_all(dir);
  }

  auto empty_count = hm::onnx::checked_element_count({2, 0});
  ok &= expect(empty_count.ok() && *empty_count == 0, "zero-sized output tensors must be representable");
  ok &= expect(
      !hm::onnx::checked_element_count({std::numeric_limits<int64_t>::max(), 3}).ok(),
      "overflowing element count must fail");
  ok &= expect(hm::onnx::validate_shape({2, 3}, {-1, 3}).ok(), "dynamic shape must accept a positive axis");
  ok &= expect(!hm::onnx::validate_shape({2, 4}, {-1, 3}).ok(), "fixed shape mismatch must fail");

  auto session = hm::onnx::Session::CreateFromBytes(
      kIdentityModel, sizeof(kIdentityModel), {input_contract()}, {output_contract()});
  if (!session.ok())
    std::cerr << session.status() << '\n';
  ok &= expect(session.ok(), "tiny identity model must load");
  if (session.ok()) {
    const float input[] = {3.25f, -7.5f};
    auto outputs = (*session)->RunFloat("input", {1, 2}, input, 2);
    ok &= expect(outputs.ok() && outputs->size() == 1, "identity inference must return one output");
    if (outputs.ok() && outputs->size() == 1) {
      auto data = outputs->at(0).float_data();
      ok &= expect(data.ok() && (*data)[0] == input[0] && (*data)[1] == input[1], "identity output must match input");
    }
    ok &= expect(!(*session)->RunFloat("wrong", {1, 2}, input, 2).ok(), "wrong input name must fail");
    ok &= expect(!(*session)->RunFloat("input", {1, 2}, input, 1).ok(), "wrong data length must fail");
    const auto cancelled = (*session)->RunFloat("input", {1, 2}, input, 2, [] { return true; });
    ok &= expect(
        !cancelled.ok() && cancelled.status().code() == absl::StatusCode::kCancelled,
        "an already-cancelled inference must stop before entering ONNX Runtime");
  }

  auto wrong_contract = output_contract();
  wrong_contract.name = "wrong";
  ok &= expect(
      !hm::onnx::Session::CreateFromBytes(kIdentityModel, sizeof(kIdentityModel), {input_contract()}, {wrong_contract})
           .ok(),
      "model contract mismatch must fail during load");
  ok &= expect(
      !hm::onnx::Session::CreateFromBytes(nullptr, 0, {input_contract()}, {output_contract()}).ok(),
      "empty model must fail");

  auto add_session = hm::onnx::Session::CreateFromBytes(
      kAddModel,
      sizeof(kAddModel),
      {{"left", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}}, {"right", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 2}}},
      {output_contract()});
  if (!add_session.ok())
    std::cerr << add_session.status() << '\n';
  ok &= expect(add_session.ok(), "tiny multi-input model must load");
  if (add_session.ok()) {
    const float left[] = {1.25f, -2.0f};
    const float right[] = {3.0f, 0.5f};
    auto outputs = (*add_session)
                       ->RunFloatInputs({
                           {"left", {1, 2}, left, 2},
                           {"right", {1, 2}, right, 2},
                       });
    ok &= expect(outputs.ok() && outputs->size() == 1, "multi-input inference must return one output");
    if (outputs.ok() && outputs->size() == 1) {
      auto data = outputs->at(0).float_data();
      ok &= expect(data.ok() && (*data)[0] == 4.25f && (*data)[1] == -1.5f, "Add output must use both inputs");
    }
    ok &= expect(
        !(*add_session)->RunFloatInputs({{"right", {1, 2}, right, 2}, {"left", {1, 2}, left, 2}}).ok(),
        "multi-input order and names must match the frozen contract");
  }
  return ok ? 0 : 1;
}
