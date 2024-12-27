cc_library(
    name = "utils",
    srcs = glob([
      "*.cpp"
    ]),
    hdrs = glob([
        "*.h",
    ]),
    copts = [
        "-fPIC",
        "-std=c++20",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "@deepstream//:deepstream_lib",
        "@yaml-cpp",
        "@gstreamer",
        "@hm//hockeymom/csrc/play_tracker:play_tracker",
    ],
)
