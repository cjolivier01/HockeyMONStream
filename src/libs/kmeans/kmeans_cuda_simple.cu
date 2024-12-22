#include "kmeans_cuda_simple.h"

#include <iostream>
#include <vector>

#include <cuda_runtime.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

// Error checking macro
#define CHECK_CUDA_ERROR(call)                                                                   \
  {                                                                                              \
    cudaError_t err = call;                                                                      \
    if (err != cudaSuccess) {                                                                    \
      fprintf(stderr, "CUDA error in %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
      exit(EXIT_FAILURE);                                                                        \
    }                                                                                            \
  }

// Kernel to compute distances and assign points to nearest centroid
__global__ void assignClusters(
    float* points,
    float* centroids,
    int* assignments,
    int n_points,
    int n_clusters,
    int dim) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;

  if (idx < n_points) {
    float min_dist = FLT_MAX;
    int closest_centroid = 0;

    // Find closest centroid for this point
    for (int c = 0; c < n_clusters; c++) {
      float dist = 0.0f;

      // Compute Euclidean distance
      for (int d = 0; d < dim; d++) {
        float diff = points[idx * dim + d] - centroids[c * dim + d];
        dist += diff * diff;
      }

      if (dist < min_dist) {
        min_dist = dist;
        closest_centroid = c;
      }
    }

    assignments[idx] = closest_centroid;
  }
}

// Kernel to update centroids
__global__ void updateCentroids(
    float* points,
    float* centroids,
    int* assignments,
    int* cluster_sizes,
    int n_points,
    int n_clusters,
    int dim) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;

  if (idx < (n_clusters * dim)) {
    int centroid_idx = idx / dim;
    int dim_idx = idx % dim;

    float sum = 0.0f;
    int count = 0;

    // Sum up all points assigned to this centroid
    for (int p = 0; p < n_points; p++) {
      if (assignments[p] == centroid_idx) {
        sum += points[p * dim + dim_idx];
        if (dim_idx == 0) {
          count++;
        }
      }
    }

    // Update centroid coordinate
    if (count > 0) {
      centroids[idx] = sum / count;
      if (dim_idx == 0) {
        cluster_sizes[centroid_idx] = count;
      }
    }
  }
}

// Host function to perform K-means clustering
void kmeansGPU(
    const float* h_points,
    float* h_centroids,
    int* h_assignments,
    int n_points,
    int n_clusters,
    int dim,
    int max_iters,
    float tolerance) {
  // Allocate device memory
  float *d_points, *d_centroids, *d_old_centroids;
  int *d_assignments, *d_cluster_sizes;

  CHECK_CUDA_ERROR(cudaMalloc(&d_points, n_points * dim * sizeof(float)));
  CHECK_CUDA_ERROR(cudaMalloc(&d_centroids, n_clusters * dim * sizeof(float)));
  CHECK_CUDA_ERROR(cudaMalloc(&d_old_centroids, n_clusters * dim * sizeof(float)));
  CHECK_CUDA_ERROR(cudaMalloc(&d_assignments, n_points * sizeof(int)));
  CHECK_CUDA_ERROR(cudaMalloc(&d_cluster_sizes, n_clusters * sizeof(int)));

  // Copy input data to device
  CHECK_CUDA_ERROR(cudaMemcpy(d_points, h_points, n_points * dim * sizeof(float), cudaMemcpyHostToDevice));
  CHECK_CUDA_ERROR(cudaMemcpy(d_centroids, h_centroids, n_clusters * dim * sizeof(float), cudaMemcpyHostToDevice));

  // Calculate grid and block dimensions
  int block_size = 256;
  int grid_size_points = (n_points + block_size - 1) / block_size;
  int grid_size_centroids = (n_clusters * dim + block_size - 1) / block_size;

  bool converged = false;
  int iter = 0;

  while (!converged && iter < max_iters) {
    // Save old centroids
    CHECK_CUDA_ERROR(
        cudaMemcpy(d_old_centroids, d_centroids, n_clusters * dim * sizeof(float), cudaMemcpyDeviceToDevice));

    // Assign points to nearest centroids
    assignClusters<<<grid_size_points, block_size>>>(d_points, d_centroids, d_assignments, n_points, n_clusters, dim);
    CHECK_CUDA_ERROR(cudaGetLastError());

    // Reset centroids and cluster sizes
    CHECK_CUDA_ERROR(cudaMemset(d_centroids, 0, n_clusters * dim * sizeof(float)));
    CHECK_CUDA_ERROR(cudaMemset(d_cluster_sizes, 0, n_clusters * sizeof(int)));

    // Update centroids
    updateCentroids<<<grid_size_centroids, block_size>>>(
        d_points, d_centroids, d_assignments, d_cluster_sizes, n_points, n_clusters, dim);
    CHECK_CUDA_ERROR(cudaGetLastError());

    // Check convergence
    float max_change = 0.0f;
    float* h_new_centroids = new float[n_clusters * dim];
    float* h_old_centroids = new float[n_clusters * dim];

    CHECK_CUDA_ERROR(
        cudaMemcpy(h_new_centroids, d_centroids, n_clusters * dim * sizeof(float), cudaMemcpyDeviceToHost));
    CHECK_CUDA_ERROR(
        cudaMemcpy(h_old_centroids, d_old_centroids, n_clusters * dim * sizeof(float), cudaMemcpyDeviceToHost));

    for (int i = 0; i < n_clusters * dim; i++) {
      float change = fabs(h_new_centroids[i] - h_old_centroids[i]);
      max_change = max_change > change ? max_change : change;
    }

    converged = max_change < tolerance;

    delete[] h_new_centroids;
    delete[] h_old_centroids;

    iter++;
  }

  // Copy results back to host
  CHECK_CUDA_ERROR(cudaMemcpy(h_assignments, d_assignments, n_points * sizeof(int), cudaMemcpyDeviceToHost));
  CHECK_CUDA_ERROR(cudaMemcpy(h_centroids, d_centroids, n_clusters * dim * sizeof(float), cudaMemcpyDeviceToHost));

  // Free device memory
  cudaFree(d_points);
  cudaFree(d_centroids);
  cudaFree(d_old_centroids);
  cudaFree(d_assignments);
  cudaFree(d_cluster_sizes);
}

void kmeansCuda(
    const std::vector<float>& points,
    int numClusters,
    int dim,
    int numIterations,
    std::vector<int>& assignments) {
  const float tolerance = 1e-4;
  std::vector<float> centroids(numClusters);
  const size_t n_points = points.size();
  assignments.resize(n_points);

  for (int i = 0; i < numClusters * dim; i++) {
    centroids[i] = points[rand() % n_points * dim + (i % dim)];
  }

  // Run K-means
  kmeansGPU(points.data(), centroids.data(), assignments.data(), n_points, numClusters, dim, numIterations, tolerance);

  for (int i = 0; i < n_points; ++i) {
    std::cout << assignments[i] << " ";
  }
  std::cout << std::endl;
}

#ifdef BUILD_MAIN
// Example usage function
int main() {
  // Example parameters
  const int n_points = 100;
  const int n_clusters = 3;
  const int dim = 2;
  const int max_iters = 100;
  const float tolerance = 1e-4;

  // Allocate and initialize host memory
  float* points = new float[n_points * dim];
  float* centroids = new float[n_clusters * dim];
  int* assignments = new int[n_points];

  // Initialize points and centroids with random values
  for (int i = 0; i < n_points * dim; i++) {
    points[i] = static_cast<float>(rand()) / RAND_MAX;
  }
  for (int i = 0; i < n_clusters * dim; i++) {
    centroids[i] = points[rand() % n_points * dim + (i % dim)];
  }

  // Run K-means
  kmeansGPU(points, centroids, assignments, n_points, n_clusters, dim, max_iters, tolerance);

  for (int i = 0; i < n_points; ++i) {
    std::cout << assignments[i] << " ";
  }
  std::cout << std::endl;

  // Clean up
  delete[] points;
  delete[] centroids;
  delete[] assignments;

  return 0;
}
#endif // BUILD_MAIN
