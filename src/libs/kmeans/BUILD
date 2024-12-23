load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_library(
    name = "kmeans",
    srcs = glob([
        "*.cpp",
    ]) + [
      "omp_kmeans.c",
      "seq_kmeans.c",
      "wtime.c",
      "file_io.c",
    ],
    copts = [
        "-fopenmp",
        "-Wall",
        "-std=c++17",
        "-fPIC",
        "-Wno-error=deprecated-declarations",
        "-DWITH_CUDA",
    ],
    linkopts = [
    ],
    deps=[
      ":kmeans_cuda_lib",
      ":kmeans_lib_headers",
    ],
    visibility = ["//visibility:public"],
)

cuda_library(
    name = "kmeans_cuda_lib",
    srcs=glob([
        "*.cu",
    ]),
    copts=[
        "-std=c++14",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-DWITH_CUDA",
        "-g",
    ],
    deps = [
        ":kmeans_lib_headers",
        # "@cuml//:cuml",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "kmeans_lib_headers",
    hdrs = glob([
        "*.h",
    ]),
    copts=[
      "-fopenmp",
      #"-Wno-error=write-strings",
    ],
    includes=[
      ".",
    ],
    visibility = ["//visibility:public"],
)
