load("@com_extension_dev//build:graph_extension.bzl", "graph_cc_extension")
load("@com_extension_dev//build:registry.bzl", "register_extension")
load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")

exports_files(["LICENSE"])

cc_binary(
    name = "nvdsgst_preprocess",
    srcs = ["gstnvdspreprocess.cpp"],
    deps = [
        ":nvdsgst_preprocess_lib",
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
    includes = [
        ".",
    ],
    deps = [
        "//src/gst-plugins/gst-nvdspreprocess/nvdspreprocess_lib:nvdspreprocess_lib",
        "@deepstream_lib",
        "@glib",
        "@gstreamer",
        "@opencv_linux//:opencv",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)
