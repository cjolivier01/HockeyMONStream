load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_library(
    name = "kmcuda_lib",
    srcs = glob([
        "*.cc",
    ]),
    copts = [
        "-fPIC",
        "-std=c++14",
        "-Wno-error=unknown-pragmas",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "kmcuda_cuda_lib",
    ],
)

cuda_library(
    name = "kmcuda_cuda_lib",
    srcs = glob([
        "*.cu",
    ]),
    hdrs = [
    ],
    copts = [
        "-std=c++11",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-g",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":headers",
        "//src/libs/k-means/kernels:kernels_cuda_lib",
        "@local_cuda//:cuda_runtime",
    ],
)

cc_library(
    name = "headers",
    hdrs = glob([
        "*.h",
        "*.cuh",
    ]),
    includes = [
        ".",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@local_cuda//:cuda_runtime",
    ],
)
