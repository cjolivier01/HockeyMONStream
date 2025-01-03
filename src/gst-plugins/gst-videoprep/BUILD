load("@gst_plugin_dev//build:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_dsvideoprep",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = [
        "-Wno-error=unused-function",
        "-Wno-error=unused-variable",
        "-Wno-error=write-strings",
    ],
    linkopts = [
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
    ],
)
