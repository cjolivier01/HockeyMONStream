load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cuda_library(
    name = "kernels_cuda_lib",
    srcs=glob([
        "*.cu",
    ]),
    hdrs=[
    ],
    copts=[
        "-std=c++11",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-g",
    ],
    deps = [
        "//libs/k-means:headers",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)

