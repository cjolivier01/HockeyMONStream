load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsfieldmask",
    srcs = [
        "dsfieldmask_lib.cpp",
        "dsfieldmask_lib.h",
        "gstdsfieldmask.cpp",
        "gstdsfieldmask.h",
    ],
    # hdrs = [
    #   "gstdsfieldmask.h", "dsfieldmask_lib.h",
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
        "@opencv_linux//:opencv",
    ],
)
