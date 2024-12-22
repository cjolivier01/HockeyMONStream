load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_library(
  name="kmcuda_lib",
  srcs=[

  ],
  hdrs=[

  ],
  copts=[
    "-fPIC",
  ],
  includes=["."],
  deps=[
    "kmcuda_cuda_lib"
  ],
  visibility = ["//visibility:public"],
)

cuda_library(
    name = "kmcuda_cuda_lib",
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
        ":headers",
        "@local_cuda//:cuda_runtime",
        "//libs/k-means/kernels:kernels_cuda_lib",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "headers",
    hdrs=glob([
        "*.h",
        "*.cuh",
    ]),
    includes=[
      ".",
    ],
    deps = [
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)
