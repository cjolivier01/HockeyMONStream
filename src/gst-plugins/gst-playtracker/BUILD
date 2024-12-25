load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "gstplaytracker",
    srcs = [
        "PlayTracker.cpp",
        "PlayTracker.h",
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
        "//libs/k-means/kmcuda:kmcuda_lib",
        "@hm//hockeymom/csrc/play_tracker:play_tracker",
        "@opencv_linux//:opencv",
    ],
)
