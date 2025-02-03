load("@gst_plugin_dev//buildfiles:gst_plugin.bzl", "gst_cc_plugin")

gst_cc_plugin(
    name = "nvdsgst_osd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts=[
      "-fPIC",
      # "-Wno-error=deprecated-declarations",
    ],
    linkopts=[
      "-lnvdsgst_helper",
      "-lnvdsgst_meta",
      "-lnvds_meta",
      "-lnvds_osd",
      "-lnvbufsurface",
      "-lnvbufsurftransform",
      "-lnvdsgst_customhelper",
      "-ldl",
      "-lpthread",
    ],
    deps = [
      
    ],
    visibility = ["//visibility:public"],
)
