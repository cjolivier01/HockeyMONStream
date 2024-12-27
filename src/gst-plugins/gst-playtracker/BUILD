load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "gstplaytracker",
    srcs = [
        "PlayTrackerCtx.cpp",
        "PlayTrackerCtx.h",
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
        "@hm//hockeymom/csrc/play_tracker:play_tracker",
    ],
)
