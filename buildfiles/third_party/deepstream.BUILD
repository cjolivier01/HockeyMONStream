INCLUDE_PREFIX = "deepstream"

config_setting(
    name = "jetson",
    values = {"define": "target_platform=jetson"},
)

config_setting(
    name = "arm64-sbsa",
    values = {"define": "target_platform=arm64"},
)

config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

cc_library(
    name = "deepstream_lib",
    hdrs = glob([
    ]) + [
    ],
    linkopts = [
        "-L./lib",
    ] + select({
        ":jetson": ["-L/opt/jetson-sysroot/opt/nvidia/deepstream/deepstream/lib"],
        ":arm64-sbsa": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        ":x86_64-linux-gnu": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        "//conditions:default": [],
    }) + [
        "-l:libnvdsgst_meta.so",
        "-l:libnvbufsurface.so",
        "-l:libnvdsgst_inferbase.so",
        "-l:libnvdsgst_helper.so",
        "-l:libnvds_meta.so",
        "-l:libnvbufsurftransform.so",
        "-l:libnvdsgst_customhelper.so",
        "-l:libnvds_nvtxhelper.so",
        "-l:libnvdsgst_smartrecord.so",
        "-l:libnvds_utils.so",
        "-l:libnvds_logger.so",
        "-l:libnvds_msgbroker.so",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":deepstream_includes",
    ],
)

cc_library(
    name = "deepstream_includes",
    srcs = [],
    hdrs = glob([
        "sources/includes/**/*.h*",
        "sources/includes/nvdsinferserver/*.h",
    ]),
    include_prefix = "deepstream/sources/includes",
    includes = [
        "sources/includes",
        "sources/includes/nvdsinferserver",
    ],
    visibility = ["//visibility:public"],
    deps = [
    ],
)

cc_library(
    name = "nvdsinfer",
    hdrs = glob([
        "sources/libs/nvdsinfer/*.h",
    ]) + [
        # "nvdsinfer_context_impl.h",
    ],
    copts = [
    ],
    includes = [
        "sources/libs/nvdsinfer",
    ],
    linkopts = [
        "-lnvinfer_plugin",
        "-lnvinfer",
        "-lnvonnxparser",
    ],
    visibility = ["//visibility:public"],
    deps = [
        # "@deepstream//sources:deepstream_includes",
        ":deepstream_includes",
        "@gst_plugin_dev//toolchains/jetson:cuda_runtime",
    ],
)
