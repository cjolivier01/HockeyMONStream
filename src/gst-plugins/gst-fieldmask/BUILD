load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsfieldmask",
    srcs = [
        "dsfieldmask_lib.cpp",
        "gstdsfieldmask.cpp",
    ],
    hdrs = [
        "dsfieldmask_lib.h",
        "gstdsfieldmask.h",
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
