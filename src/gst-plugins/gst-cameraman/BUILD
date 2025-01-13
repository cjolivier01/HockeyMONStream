load("@gst_plugin_dev//buildfiles:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dscameraman",
    srcs = [
        "dscameraman_lib.cpp",
        "dscameraman_lib.h",
        "gstdscameraman_optimized.cpp",
        "gstdscameraman_optimized.h",
    ],
    copts = [
        # "-DWITH_OPENCV=1",
        "-Wno-error=unused-function",
        "-Wno-error=unused-variable",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/libs/common:utils",
        "@opencv_linux//:opencv",
    ],
)
