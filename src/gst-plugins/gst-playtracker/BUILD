load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "gstplaytracker",
    srcs = [
        "playtracker_lib.cpp",
        "playtracker_lib.h",
        "gstplaytracker.cpp",
        "gstplaytracker.h",
    ],
    copts = [
        # "-Wno-error=unused-function",
        # "-Wno-error=unused-variable",
    ],
    linkopts = [
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//libs/common:utils",
        "//libs/kmeans:kmeans",
        "@opencv_linux//:opencv",
    ],
)

