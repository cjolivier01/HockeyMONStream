# Bazel is only available for amd64 and arm64.

cc_library(
  name = "deepstream_lib",
  hdrs = glob([
      "sources/includes/**/*.h*",
      "sources/libs/nvdsinfer/nvdsinfer_func_utils.h",
  ]),
  includes = [
      "sources/includes",
      "sources/libs/nvdsinfer",
  ],
  # copts=[
  #   "-Isources/includes",
  # ],
  linkopts = [
    "-L/home/colivier/src/hmdeepstream/deepstream/lib",
    "-L/opt/nvidia/deepstream/deepstream-7.1/lib",
    "-l:libnvdsgst_meta.so",
    "-l:libnvbufsurface.so",
    "-l:libnvdsgst_inferbase.so",
    "-l:libnvdsgst_helper.so",
    "-l:libnvds_meta.so", 
    "-l:libnvbufsurftransform.so",
    "-l:libnvdsgst_customhelper.so",
    "-l:libnvds_nvtxhelper.so",
  ],
  visibility = ["//visibility:public"],
)
