load("@gst_plugin_dev//buildfiles:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsexample",
    srcs = [
        "dsexample_lib.cpp",
        "dsexample_lib.h",
        "gstdsexample.cpp",
        "gstdsexample.h",
        # "gstdsexample_optimized.cpp",
        # "gstdsexample_optimized.h",
    ],
    # hdrs = [
    #   "gstdsexample.h", "dsexample_lib.h",
    # ],
    copts = [
        # "-DWITH_OPENCV=1",
        "-Wno-error=unused-function",
        "-Wno-error=unused-variable",
    ],
    linkopts = [
        #"-shared",
        #"-Wl,--unresolved-symbols=ignore-all",
    ],
    visibility = ["//visibility:public"],
    deps = [
        #"//extensions/includes:deepstream_headers",
        # "@deepstream_lib",
        # "@deepstream_apps_common",
        # "@NvDsBaseExt",
        # "@NvDsInterfaceExt",
        # "@StandardExtension",
        "//src/libs/common:utils",
        "@opencv_linux//:opencv",
    ],
)
