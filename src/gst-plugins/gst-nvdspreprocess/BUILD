load("@com_extension_dev//build:graph_extension.bzl", "graph_cc_extension")
load("@com_extension_dev//build:registry.bzl", "register_extension")
load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")

exports_files(["LICENSE"])

cc_binary(
    name = "nvdsgst_preprocess",
    srcs = ["gstnvdspreprocess.cpp"],
    copts=[
      "-fPIC",
      "-DWITH_CUDA",
    ],
    linkopts=[
      "-shared",
    ],
    deps = [
        ":nvdsgst_preprocess_lib",
        "@deepstream_lib",
        "@NvDsBaseExt",
        "@NvDsInterfaceExt",
        "@StandardExtension",
        "@glib",
        "@gstreamer",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
    linkshared = True,
)

cc_library(
    name = "nvdsgst_preprocess_lib",
    srcs = [
      "gstnvdspreprocess_allocator.cpp", "nvdspreprocess_property_parser.cpp",
    ],
    hdrs = [
      "gstnvdspreprocess_allocator.h", "nvdspreprocess_property_parser.h", "gstnvdspreprocess.h",
    ],
    copts=[
      "-fPIC",
      "-DWITH_CUDA",
    ],
    includes = [
        ".",
    ],
    deps = [
        "//gst-plugins/gst-nvdspreprocess/nvdspreprocess_lib:nvdspreprocess_lib",
        "@deepstream_lib",
        "@glib",
        "@gstreamer",
        "@opencv_linux//:opencv",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)

# cc_library(
#     name = "nvdspreprocess_lib",
#     srcs = [
#       "nvdspreprocess_impl.cpp", "nvdspreprocess_lib.cpp",
#     ],
#     hdrs = [
#       "nvdspreprocess_impl.h", "nvdspreprocess_lib.h",
#     ],
#     copts=[
#       "-fPIC",
#     ],
#     deps = [
#         ":nvdspreprocess_cuda_lib",
#         "@deepstream_lib",
#         "@glib",
#         "@gstreamer",
#         "@opencv_linux//:opencv",
#     ],
#     visibility = ["//visibility:public"],
# )
