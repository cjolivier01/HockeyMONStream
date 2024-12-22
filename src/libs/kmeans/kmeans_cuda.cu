#include <cuda_runtime.h>
#include <cfloat>
#include <cstdlib>
#include <iostream>
#include <vector>

// CUDA Kernel for K-means (assign points to clusters)
__global__ void assignClusters(
    const float* points,
    const float* centroids,
    int* labels,
    int numPoints,
    int numClusters,
    int dim) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= numPoints)
    return;

  float minDist = FLT_MAX;
  int bestCluster = 0;

  for (int c = 0; c < numClusters; ++c) {
    float dist = 0.0;
    for (int d = 0; d < dim; ++d) {
      float diff = points[idx * dim + d] - centroids[c * dim + d];
      dist += diff * diff;
    }
    if (dist < minDist) {
      minDist = dist;
      bestCluster = c;
    }
  }

  labels[idx] = bestCluster;
}

// CUDA Kernel for K-means (update centroids)
__global__ void updateCentroids(
    const float* points,
    const int* labels,
    float* centroids,
    int* clusterSizes,
    int numPoints,
    int numClusters,
    int dim) {
  extern __shared__ float sharedCentroids[];

  int idx = threadIdx.x;
  int cluster = blockIdx.x;

  for (int d = 0; d < dim; ++d) {
    sharedCentroids[idx * dim + d] = 0.0;
  }

  int clusterSize = 0;

  for (int i = idx; i < numPoints; i += blockDim.x) {
    if (labels[i] == cluster) {
      clusterSize++;
      for (int d = 0; d < dim; ++d) {
        sharedCentroids[idx * dim + d] += points[i * dim + d];
      }
    }
  }

  atomicAdd(&clusterSizes[cluster], clusterSize);

  for (int d = 0; d < dim; ++d) {
    atomicAdd(&centroids[cluster * dim + d], sharedCentroids[idx * dim + d]);
  }
}

// CPU Code for K-means clustering using CUDA
void kmeansCuda(const std::vector<float>& points, int numClusters, int dim, int numIterations) {
  int numPoints = points.size() / dim;

  // Allocate memory on the GPU
  float *d_points, *d_centroids;
  int *d_labels, *d_clusterSizes;
  cudaMalloc(&d_points, points.size() * sizeof(float));
  cudaMalloc(&d_centroids, numClusters * dim * sizeof(float));
  cudaMalloc(&d_labels, numPoints * sizeof(int));
  cudaMalloc(&d_clusterSizes, numClusters * sizeof(int));

  // Copy points to the GPU
  cudaMemcpy(d_points, points.data(), points.size() * sizeof(float), cudaMemcpyHostToDevice);

  // Initialize centroids randomly
  std::vector<float> centroids(numClusters * dim);
  for (int i = 0; i < numClusters * dim; ++i) {
    centroids[i] = points[rand() % points.size()];
  }
  cudaMemcpy(d_centroids, centroids.data(), centroids.size() * sizeof(float), cudaMemcpyHostToDevice);

  // K-means iterations
  for (int iter = 0; iter < numIterations; ++iter) {
    cudaMemset(d_clusterSizes, 0, numClusters * sizeof(int));

    // Assign points to clusters
    assignClusters<<<(numPoints + 255) / 256, 256>>>(d_points, d_centroids, d_labels, numPoints, numClusters, dim);

    // Update centroids
    updateCentroids<<<numClusters, 256, 256 * dim * sizeof(float)>>>(
        d_points, d_labels, d_centroids, d_clusterSizes, numPoints, numClusters, dim);
  }

  // Copy results back to the CPU
  cudaMemcpy(centroids.data(), d_centroids, centroids.size() * sizeof(float), cudaMemcpyDeviceToHost);

  // Print final centroids
  std::cout << "Final centroids:\n";
  for (int c = 0; c < numClusters; ++c) {
    std::cout << "Cluster " << c << ": ";
    for (int d = 0; d < dim; ++d) {
      std::cout << centroids[c * dim + d] << " ";
    }
    std::cout << std::endl;
  }

  // Free GPU memory
  cudaFree(d_points);
  cudaFree(d_centroids);
  cudaFree(d_labels);
  cudaFree(d_clusterSizes);
}

int main() {
  // Example points (flattened 2D points)
  std::vector<float> points = {1.0, 2.0, 1.5, 1.8, 5.0, 8.0, 1.1, 2.2, 5.5, 8.2, 1.2, 1.5};
  int numClusters = 2;
  int dim = 2;
  int numIterations = 10;

  kmeansCuda(points, numClusters, dim, numIterations);

  return 0;
}
