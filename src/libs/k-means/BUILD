load("@rules_cc//cc:defs.bzl", "cc_library")

cc_binary(
    name = "k-means",
    srcs = [
        "k-means.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":k-means-lib",
    ],
)

cc_library(
    name = "k-means-lib",
    srcs = [
        "kmcuda_adapter.cpp",
    ],
    hdrs = [],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "//libs/k-means/kmcuda:kmcuda_lib",
        ":headers",
    ],
)

cc_library(
    name = "headers",
    hdrs = glob([
        "headers/**/*.hpp",
        "headers/*.cuh",
    ]),
    includes = ["headers"],
    visibility = ["//visibility:public"],
    deps = [
        "//libs/k-means/kmcuda:headers",
    ],
)
