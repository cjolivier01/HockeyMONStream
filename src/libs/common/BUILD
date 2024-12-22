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
        "@yaml-cpp",
        "@gstreamer",
    ],
)
