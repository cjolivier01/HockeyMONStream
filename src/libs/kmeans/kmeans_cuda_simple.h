#pragma once

#include <vector>

namespace hm {
namespace cuda {
void kmeansCuda(
    const std::vector<float>& points,
    int numClusters,
    int dim,
    int numIterations,
    std::vector<size_t>& assignments);
}
} // namespace hm
