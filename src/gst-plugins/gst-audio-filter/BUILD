load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_audio_filter",
    srcs = glob(["*.c"]),
    hdrs = glob(["*.h"]),
    copts = [
        "-x",
        "c",
        "-Wno-error=write-strings",
    ],
    visibility = ["//visibility:public"],
    deps = [],
)
