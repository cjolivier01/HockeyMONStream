#include "hstream/src/libs/detection/TileInference.h"

#include <cmath>
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
    for (const auto& size : {std::pair{12407, 4710}, std::pair{12370, 3113}, std::pair{1000, 400}}) {
      auto tiles = MakeDetectionTiles(size.first, size.second, .2);
      Check(tiles.ok(), "Real panorama rejected");
      const auto& a = (*tiles)[0];
      const auto& b = (*tiles)[1];
      Check(a.left == 0 && a.top == 0 && b.top == 0 && a.width == b.width, "Asymmetric tiles");
      Check(b.left + b.width == (size.first & ~1) && a.height == (size.second & ~1), "Lost canvas coverage");
      Check(!(a.width % 2) && !(b.left % 2) && !(a.height % 2), "Unaligned native crop");
      Check(a.width > b.left && std::abs(double(a.width - b.left) / a.width - .2) < .01, "Wrong overlap");
    }
    auto halves = MakeDetectionTiles(1000, 400, 0);
    Check(halves.ok() && (*halves)[0].width == 500 && (*halves)[1].left == 500, "Zero overlap");
    for (double overlap : {-1., 1., std::numeric_limits<double>::quiet_NaN()})
      Check(!MakeDetectionTiles(1000, 400, overlap).ok(), "Invalid overlap accepted");
    Check(!MakeDetectionTiles(2, 400, .2).ok(), "Tiny canvas accepted");
    std::vector<TileProposal> input{
        {{450, 10, 500, 100, .7, 0, 0}, 0},
        {{451, 11, 501, 101, .9, 0, 1}, 1},
        {{451, 11, 501, 101, .8, 2, 2}, 0},
        {{451, 11, 501, 101, .6, 0, 3}, 1},
        {{700, 10, 750, 100, .5, 0, 4}, 1}};
    auto keep = MergeTileProposals(input, .5);
    Check(keep.ok() && *keep == std::vector<size_t>({1, 2, 3, 4}), "Seam NMS changed classes or per-tile results");
    input[0].box.confidence = .9;
    keep = MergeTileProposals(input, .5);
    Check(keep.ok() && *keep == std::vector<size_t>({0, 2, 4}), "Ties must be deterministic");
    Check(MergeTileProposals({}, .5)->empty(), "Empty frame failed");
    input[0].box.right = std::numeric_limits<float>::infinity();
    Check(!MergeTileProposals(input, .5).ok(), "Invalid output accepted");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
