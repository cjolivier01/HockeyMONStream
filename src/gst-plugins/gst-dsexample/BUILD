load("@com_extension_dev//build:registry.bzl", "register_extension")
load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")
load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")

# gst_cc_extension

exports_files(["LICENSE"])

# cc_binary(
#   name = "dsexample",
#   srcs=[":dsexample_lib"],
#   linkopts=[
#       "-shared",
#   ],
#   linkshared = 1,
# )

#graph_cc_extension(
#cc_library(
#cc_binary(
gst_cc_plugin(
    name = "nvdsgst_dsexample",
    srcs = [
        "dsexample_lib.cpp",
        "dsexample_lib.h",
        "gstdsexample_optimized.cpp",
        "gstdsexample_optimized.h",
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
        "//libs/common:utils",
        "@opencv_linux//:opencv",
    ],
)

# graph_cc_extension(
#     name = "dsexample_optimized",
#     srcs = [
#       "gstdsexample_optimized.cpp", "dsexample_lib.c",
#     ],
#     hdrs = [
#       "gstdsexample_optimized.h", "dsexample_lib.h",
#     ],
#     copts=[
#     ],
#     deps = [
#         #"//extensions/includes:deepstream_headers",
#         "@deepstream_lib",
#         # "@NvDsBaseExt",
#         # "@NvDsInterfaceExt",
#         # "@StandardExtension",
#         "@glib",
#         "@gstreamer",
#         "@local_cuda//:cuda_runtime",
#         "@opencv_linux//:opencv",
#     ],
#     visibility = ["//visibility:public"],
# )

# register_extension(
#     name = "register_nvtracker_ext",
#     badges = [""],
#     extension = "dsexample",
#     labels = [
#         "nvidia",
#         "gpu",
#         "deepstream",
#     ],
#     ngc_dependencies = {
#         "NvDsBaseExt": "1.6.0",
#     },
#     priority = "1",
#     git_repository = "",
#     compute_dependencies = ["deepstream"],
#     url = "www.example.com",
#     uuid = "1ca0f521-371e-3c65-80fd-0b6a7cac4082",
#     version = "0.0.1",
#     visibility = ["//visibility:public"],
# )
