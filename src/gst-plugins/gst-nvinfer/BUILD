load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")
# load("@com_extension_dev//build:registry.bzl", "register_extension")
# load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")


gst_cc_plugin(
    name = "nvdsgst_infer",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts=[
      "-fPIC",
      "-Wno-error=deprecated-declarations",
    ],
    linkopts=[
      "-l:/home/colivier/src/hmdeepstream/deepstream/sources/libs/nvdsinfer/libnvds_infer.so",
    ],
    deps = [
    ],
    visibility = ["//visibility:public"],
)
