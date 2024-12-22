load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsplaytracker",
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
        # "@deepstream_lib",
        # "@deepstream_apps_common",
        # "@NvDsBaseExt",
        # "@NvDsInterfaceExt",
        # "@StandardExtension",
        "//libs/common:utils",
        "@opencv_linux//:opencv",
    ],
)

