
INCLUDE_PREFIX = "src/libs/common"

cc_library(
    name = "utils",
    srcs = glob([
        "*.cpp",
    ]),
    hdrs = glob([
        "*.h",
    ]),
    copts = [
        "-fPIC",
        "-std=c++20",
    ],
    includes = ["."], # TODO: can this be removed?
    include_prefix = INCLUDE_PREFIX,
    visibility = ["//visibility:public"],
    deps = [
        "@deepstream//:deepstream_lib",
        "@gstreamer",
        "@hm//hockeymom/csrc/play_tracker",
        "@opencv_linux//:opencv",
        "@yaml-cpp",
        "@local_cuda//:cuda_runtime",
        "@local_cuda//:cuda",
    ],
)
