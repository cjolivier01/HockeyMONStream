load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsosd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts=[
      "-fPIC",
      # "-Wno-error=deprecated-declarations",
    ],
    linkopts=[
    ],
    deps = [
    ],
    visibility = ["//visibility:public"],
)
