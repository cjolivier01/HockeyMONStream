load("@gst_plugin_dev//buildfiles:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsreplaceframe",
    srcs = [
        "dsreplaceframe_lib.cpp",
        "gstdsreplaceframe.cpp",
    ],
    hdrs = [
        "dsreplaceframe_lib.h",
        "gstdsreplaceframe.h",
    ],
    copts = [
        "-Wno-error=unused-function",
        "-Wno-error=unused-variable",
    ],
    linkopts = [
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@opencv_linux//:opencv",
    ],
)
