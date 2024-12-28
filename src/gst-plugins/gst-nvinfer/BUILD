load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")


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
      "@deepstream//sources/libs/nvdsinfer:nvdsinfer",
    ],
    visibility = ["//visibility:public"],
)
