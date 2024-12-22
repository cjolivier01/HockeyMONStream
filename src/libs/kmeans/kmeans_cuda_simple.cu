#include "kmeans_cuda_simple.h"

#include <cuda_runtime.h>
#include <cfloat>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <random>
#include <vector>

// A small data structure to do RAII for a dataset of 2-dimensional points.
struct Data {
  explicit Data(int size) : size(size), bytes(size * sizeof(float)) {
    cudaMalloc(&x, bytes);
    cudaMalloc(&y, bytes);
  }

  Data(int size, std::vector<float>& h_x, std::vector<float>& h_y) : size(size), bytes(size * sizeof(float)) {
    cudaMalloc(&x, bytes);
    cudaMalloc(&y, bytes);
    cudaMemcpy(x, h_x.data(), bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(y, h_y.data(), bytes, cudaMemcpyHostToDevice);
  }

  ~Data() {
    cudaFree(x);
    cudaFree(y);
  }

  void clear() {
    cudaMemset(x, 0, bytes);
    cudaMemset(y, 0, bytes);
  }

  float* x{nullptr};
  float* y{nullptr};
  int size{0};
  int bytes{0};
};

__device__ float squared_l2_distance(float x_1, float y_1, float x_2, float y_2) {
  return (x_1 - x_2) * (x_1 - x_2) + (y_1 - y_2) * (y_1 - y_2);
}

// In the assignment step, each point (thread) computes its distance to each
// cluster centroid and adds its x and y values to the sum of its closest
// centroid, as well as incrementing that centroid's count of assigned points.
__global__ void assign_clusters(
    const float* __restrict__ data_x,
    const float* __restrict__ data_y,
    int data_size,
    const float* __restrict__ means_x,
    const float* __restrict__ means_y,
    float* __restrict__ new_sums_x,
    float* __restrict__ new_sums_y,
    int k,
    int* __restrict__ counts) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= data_size)
    return;

  // Make global loads once.
  const float x = data_x[index];
  const float y = data_y[index];

  float best_distance = FLT_MAX;
  int best_cluster = 0;
  for (int cluster = 0; cluster < k; ++cluster) {
    const float distance = squared_l2_distance(x, y, means_x[cluster], means_y[cluster]);
    if (distance < best_distance) {
      best_distance = distance;
      best_cluster = cluster;
    }
  }

  // Slow but simple.
  atomicAdd(&new_sums_x[best_cluster], x);
  atomicAdd(&new_sums_y[best_cluster], y);
  atomicAdd(&counts[best_cluster], 1);
}

// Each thread is one cluster, which just recomputes its coordinates as the mean
// of all points assigned to it.
__global__ void compute_new_means(
    float* __restrict__ means_x,
    float* __restrict__ means_y,
    const float* __restrict__ new_sum_x,
    const float* __restrict__ new_sum_y,
    const int* __restrict__ counts) {
  const int cluster = threadIdx.x;
  // Threshold count to turn 0/0 into 0/1.
  const int count = max(1, counts[cluster]);
  means_x[cluster] = new_sum_x[cluster] / count;
  means_y[cluster] = new_sum_y[cluster] / count;
}

namespace hm {
namespace cuda {
// CPU Code for K-means clustering using CUDA
// `void kmeansCuda(const std::vector<float>& points, int numClusters, int dim, int numIterations) {
//   int numPoints = points.size() / dim;

//   // Allocate memory on the GPU
//   float *d_points, *d_centroids;
//   int *d_labels, *d_clusterSizes;
//   cudaMalloc(&d_points, points.size() * sizeof(float));
//   cudaMalloc(&d_centroids, numClusters * dim * sizeof(float));
//   cudaMalloc(&d_labels, numPoints * sizeof(int));
//   cudaMalloc(&d_clusterSizes, numClusters * sizeof(int));

//   // Copy points to the GPU
//   cudaMemcpy(d_points, points.data(), points.size() * sizeof(float), cudaMemcpyHostToDevice);

//   // Initialize centroids randomly
//   std::vector<float> centroids(numClusters * dim);
//   for (int i = 0; i < numClusters * dim; ++i) {
//     centroids[i] = points[rand() % points.size()];
//   }
//   cudaMemcpy(d_centroids, centroids.data(), centroids.size() * sizeof(float), cudaMemcpyHostToDevice);

//   // K-means iterations
//   for (int iter = 0; iter < numIterations; ++iter) {
//     cudaMemset(d_clusterSizes, 0, numClusters * sizeof(int));

//     // Assign points to clusters
//     assignClusters<<<(numPoints + 255) / 256, 256>>>(d_points, d_centroids, d_labels, numPoints, numClusters, dim);

//     // Update centroids
//     updateCentroids<<<numClusters, 256, 256 * dim * sizeof(float)>>>(
//         d_points, d_labels, d_centroids, d_clusterSizes, numPoints, numClusters, dim);
//   }

//   // Copy results back to the CPU
//   std::vector<int> cluster_sizes(numClusters, -1);
//   std::vector<int> labels(numPoints, -1);

//   cudaMemcpy(cluster_sizes.data(), d_clusterSizes, cluster_sizes.size() * sizeof(int), cudaMemcpyDeviceToHost);
//   cudaMemcpy(labels.data(), d_labels, labels.size() * sizeof(int), cudaMemcpyDeviceToHost);

//   // Print final centroids
//   std::cout << "Final centroids:\n";
//   for (int c = 0; c < numClusters; ++c) {
//     std::cout << "Cluster " << c << ": ";
//     for (int d = 0; d < dim; ++d) {
//       std::cout << centroids[c * dim + d] << " ";
//     }
//     int cluster_size = cluster_sizes.at(c);
//     std::cout << "cluster size=" << cluster_size;
//     std::cout << std::endl;
//   }

//   std::cout << "point labels: [ ";
//   for (int l = 0; l < numPoints; l++) {
//     std::cout << labels.at(l) << " ";
//   }
//   std::cout << "]";
//   std::cout << std::endl;

//   // Free GPU memory
//   cudaFree(d_points);
//   cudaFree(d_centroids);
//   cudaFree(d_labels);
//   cudaFree(d_clusterSizes);
// }

void kmeansCuda(const std::vector<float>& points, int numClusters, int dim, int numIterations) {
  std::vector<float> h_x;
  std::vector<float> h_y;

  const size_t number_of_elements = h_x.size();
  h_x.reserve(number_of_elements);
  h_y.reserve(number_of_elements);

  for (std::size_t i = 0; i < number_of_elements; ++i) {
    std::size_t pos = i << 1;
    h_x.push_back(points[pos]);
    h_y.push_back(points[pos + 1]);
  }

  // Load x and y into host vectors ... (omitted)

  int k = numClusters;

  Data d_data(number_of_elements, h_x, h_y);

  // Random shuffle the data and pick the first
  // k points (i.e. k random points).

  std::random_device seed;
  std::mt19937 rng(seed());
  std::shuffle(h_x.begin(), h_x.end(), rng);
  std::shuffle(h_y.begin(), h_y.end(), rng);
  Data d_means(k, h_x, h_y);

  Data d_sums(k);

  int* d_counts;
  cudaMalloc(&d_counts, k * sizeof(int));
  cudaMemset(d_counts, 0, k * sizeof(int));

  const int threads = 1024;
  const int blocks = (number_of_elements + threads - 1) / threads;

  for (size_t iteration = 0; iteration < numIterations; ++iteration) {
    cudaMemset(d_counts, 0, k * sizeof(int));
    d_sums.clear();

    assign_clusters<<<blocks, threads>>>(
        d_data.x, d_data.y, d_data.size, d_means.x, d_means.y, d_sums.x, d_sums.y, k, d_counts);
    cudaDeviceSynchronize();

    compute_new_means<<<1, k>>>(d_means.x, d_means.y, d_sums.x, d_sums.y, d_counts);
    cudaDeviceSynchronize();
  }
}
} // namespace cuda
} // namespace hm
