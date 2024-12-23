load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_library(
    name = "kmeans",
    srcs = glob([
        "*.cpp",
    ]) + [
        "file_io.c",
        "omp_kmeans.c",
        "seq_kmeans.c",
        "wtime.c",
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
        "-lgomp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":kmeans_cuda_lib",
        ":kmeans_lib_headers",
    ],
)

cuda_library(
    name = "kmeans_cuda_lib",
    srcs = glob([
        "kmeans_cuda_simple.cu",
        "cuda_io.cu",
        "cuda_kmeans.cu",
        "cuda_wtime.cu",
    ]),
    copts = [
        "-std=c++14",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-DWITH_CUDA",
        "-g",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":kmeans_lib_headers",
        # "@cuml//:cuml",
        "@local_cuda//:cuda_runtime",
    ],
)

cc_library(
    name = "kmeans_lib_headers",
    hdrs = glob([
        "*.h",
    ]),
    copts = [
        "-fopenmp",
        #"-Wno-error=write-strings",
    ],
    includes = [
        ".",
    ],
    visibility = ["//visibility:public"],
)
