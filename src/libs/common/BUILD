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
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "@gstreamer",
    ],
)
