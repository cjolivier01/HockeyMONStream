#include "hstream/src/libs/detection/DetectionPostprocess.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  using namespace hm::detection;
  try {
    const auto box = MakeLetterbox(100, 50, 100, 100, true, true, 1.f / 255);
    Check(box.ok() && box->top == 25 && box->resized_height == 50, "symmetric padding");
    const auto stretch = MakeLetterbox(100, 50, 100, 100, false, false, 1.f / 255);
    Check(stretch.ok() && stretch->top == 0 && stretch->resized_height == 100, "stretch geometry");
    Check(!MakeLetterbox(0, 50, 100, 100, true, false, 1).ok(), "empty source");
    Check(!MakeLetterbox(100, 50, 100, 100, true, false, std::numeric_limits<float>::infinity()).ok(), "invalid scale");
    std::vector<Proposal> input{
        {0, 0, 10, 10, 1, 0, 0}, {20, 30, 40, 50, .9, 0, 1}, {21, 31, 41, 51, .9, 0, 2}, {20, 30, 40, 50, .8, 1, 3}};
    const auto result = SuppressProposals(input, .45, 2, *box, 100, 50);
    Check(result.ok() && result->size() == 2, "padding predictions do not consume limit");
    Check((*result)[0].source_index == 1 && (*result)[1].source_index == 3, "NMS keeps different class");
    Check((*result)[0].top == 5 && (*result)[0].bottom == 25, "source coordinates");
    std::reverse(input.begin(), input.end());
    const auto reversed = SuppressProposals(input, .45, 2, *box, 100, 50);
    Check(reversed.ok() && (*reversed)[0].source_index == 1, "order-independent ties");
    Check(!SuppressProposals(input, .45, 0, *box, 100, 50).ok(), "invalid output bound");
    Check(!SuppressProposals(input, -1, 2, *box, 100, 50).ok(), "invalid overlap");
    input[0].confidence = std::numeric_limits<float>::quiet_NaN();
    Check(!SuppressProposals(input, .45, 2, *box, 100, 50).ok(), "reject nonfinite score before sorting");
    std::cout << "Detector geometry, class-aware NMS, deterministic ties and validation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
