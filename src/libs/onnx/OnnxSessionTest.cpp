#include "hstream/src/libs/onnx/OnnxSession.h"

#include <cstdint>
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
