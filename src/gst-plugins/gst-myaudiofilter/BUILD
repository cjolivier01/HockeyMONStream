load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_myaudiofilter",
    srcs = glob(["*.c"]),
    hdrs = glob(["*.h"]),
    copts = [
        "-x", "c",
    ],
    visibility = ["//visibility:public"],
    deps = [],
)
