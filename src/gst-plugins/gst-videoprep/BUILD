load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_videoprep",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = [
        "-Wno-error=unused-function",
        "-Wno-error=unused-variable",
        "-Wno-error=write-strings",
    ],
    linkopts = [
      "-L/usr/local/cuda-12.6/targets/x86_64-linux/lib",
      "-l:libnppig.so",
      "-lnppc_static",
      "-lnppif_static",
      "-lnvdsgst_helper",
      "-lnvdsgst_meta",
      "-lnvds_meta",
      "-lnvdsbufferpool",
      "-lnvbufsurface",
      "-lnvbufsurftransform",
      "-lnvds_csvparser",
      "-lnvds_dewarper",
			"-lnvds_nvtxhelper",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//libs/common:utils",
        # "@jetson-utils//cuda:cuda_lib",
        # "@jetson-utils//display:display",
    ],
)
