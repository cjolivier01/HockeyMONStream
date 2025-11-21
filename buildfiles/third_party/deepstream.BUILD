INCLUDE_PREFIX="deepstream"

cc_library(
    name = "deepstream_lib",
    hdrs = glob([
    ]) + [
    ],
    linkopts = [
        "-L./lib",
        "-L/opt/nvidia/deepstream/deepstream/lib",
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
    includes = [
        "sources/includes",
        "sources/includes/nvdsinferserver",
    ],
    include_prefix = "deepstream/sources/includes",
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
    deps=[
      # "@deepstream//sources:deepstream_includes",
      ":deepstream_includes",
      "@local_cuda//:cuda_runtime",
    ],
)
