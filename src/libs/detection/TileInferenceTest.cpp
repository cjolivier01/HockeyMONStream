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
      auto tiles = MakeDetectionTiles(size.first, size.second, {.2, .5, 2});
      Check(tiles.ok(), "Real panorama rejected");
      const auto& a = (*tiles)[0];
      const auto& b = (*tiles)[1];
      Check(a.left == 0 && a.top == 0 && b.top == 0 && a.width == b.width, "Asymmetric tiles");
      Check(b.left + b.width == (size.first & ~1) && a.height == (size.second & ~1), "Lost canvas coverage");
      Check(!(a.width % 2) && !(b.left % 2) && !(a.height % 2), "Unaligned native crop");
      Check(a.width > b.left && std::abs(double(a.width - b.left) / a.width - .2) < .01, "Wrong overlap");
    }
    auto halves = MakeDetectionTiles(1000, 400, {0, .5, 2});
    Check(halves.ok() && (*halves)[0].width == 500 && (*halves)[1].left == 500, "Zero overlap");
    for (double overlap : {-1., 1., std::numeric_limits<double>::quiet_NaN()})
      Check(!MakeDetectionTiles(1000, 400, {overlap, .5, 2}).ok(), "Invalid overlap accepted");
    Check(!MakeDetectionTiles(2, 400, {.2, .5, 2}).ok(), "Tiny canvas accepted");
    for (const auto& size : {std::pair{12407, 4711}, std::pair{12370, 3113}, std::pair{1000, 800}}) {
      for (double overlap : {0., .2, .8}) {
        const auto tiles = MakeDetectionTiles(size.first, size.second, {overlap, .5, 4});
        Check(tiles.ok() && tiles->size() == 4, "Quadrant plan failed");
        const int width = size.first & ~1, height = size.second & ~1;
        const auto& first = tiles->front();
        for (unsigned index = 0; index < 4; ++index) {
          const auto& tile = (*tiles)[index];
          Check(tile.width == first.width && tile.height == first.height, "Unequal quadrants");
          Check(
              tile.left == (index % 2 ? width - tile.width : 0) && tile.top == (index / 2 ? height - tile.height : 0),
              "Quadrant coverage/origin");
          Check(
              !(tile.left % 2) && !(tile.top % 2) && !(tile.width % 2) && !(tile.height % 2),
              "Quadrants must be even aligned");
        }
        Check(
            std::abs(double(2 * first.width - width) / first.width - overlap) < .01 &&
                std::abs(double(2 * first.height - height) / first.height - overlap) < .01,
            "Both axes must use the configured overlap");
      }
    }
    Check(!MakeDetectionTiles(1000, 2, {.2, .5, 4}).ok(), "Tiny quadrant height accepted");
    for (int count : {-1, 0, 1, 3, 5})
      Check(!MakeDetectionTiles(1000, 800, {.2, .5, count}).ok(), "Unsupported tile count accepted");
    std::vector<TileProposal> input{
        {{450, 10, 500, 100, .7, 0, 0}, 0},
        {{451, 11, 501, 101, .9, 0, 1}, 1},
        {{451, 11, 501, 101, .8, 2, 2}, 0},
        {{451, 11, 501, 101, .6, 0, 3}, 1},
        {{700, 10, 750, 100, .5, 0, 4}, 1}};
    auto keep = MergeTileProposals(input, {.2, .5, 2});
    Check(keep.ok() && *keep == std::vector<size_t>({1, 2, 3, 4}), "Seam NMS changed classes or per-tile results");
    input[0].box.confidence = .9;
    keep = MergeTileProposals(input, {.2, .5, 2});
    Check(keep.ok() && *keep == std::vector<size_t>({0, 2, 4}), "Ties must be deterministic");
    Check(MergeTileProposals({}, {.2, .5, 2})->empty(), "Empty frame failed");
    input[0].box.right = std::numeric_limits<float>::infinity();
    Check(!MergeTileProposals(input, {.2, .5, 2}).ok(), "Invalid output accepted");
    // All four quadrants see the centre. Keep the best box, distinct roles,
    // and a separate lower-right player without changing their coordinates.
    input = {
        {{480, 380, 510, 410, .6, 0, 0}, 0},
        {{481, 381, 511, 411, .7, 0, 1}, 1},
        {{482, 382, 512, 412, .8, 0, 2}, 2},
        {{483, 383, 513, 413, .9, 0, 3}, 3},
        {{480, 380, 510, 410, .85, 2, 4}, 0},
        {{800, 700, 830, 730, .5, 0, 5}, 3}};
    keep = MergeTileProposals(input, {.2, .5, 4});
    Check(keep.ok() && *keep == std::vector<size_t>({3, 4, 5}), "Four-way merge changed roles/coordinates");
    Check(!MergeTileProposals(input, {.2, .5, 2}).ok(), "Quadrant IDs accepted for halves");
    input[0].tile = 4;
    Check(!MergeTileProposals(input, {.2, .5, 4}).ok(), "Out-of-range quadrant accepted");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
