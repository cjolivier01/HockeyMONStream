load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_library(
    name = "kmeans",
    srcs = glob([
        "*.cpp",
    ]),
    copts = [
        "-Wall",
        "-std=c++11",
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
        "-std=c++11",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-DWITH_CUDA",
        "-g",
    ],
    deps = [
        ":kmeans_lib_headers",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "kmeans_lib_headers",
    hdrs = glob([
        "*.h",
    ]),
    includes=[
      ".",
    ],
    visibility = ["//visibility:public"],
)
