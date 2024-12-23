# Bazel is only available for amd64 and arm64.

config_setting(
  name = "aarch64-linux-gnu",
  define_values = {"multiarch": "aarch64-linux-gnu"},
)

config_setting(
  name = "x86_64-linux-gnu",
  define_values = {"multiarch": "x86_64-linux-gnu"},
)

cc_library(
  name = "cuml",
  hdrs = glob([
      "include/cuml/**/*.h*",
  ]),
  includes = [
      "include",
  ],
  linkopts = [
    "-Llib",
    # "-l:libopencv_core.so",
  ],
  visibility = ["//visibility:public"],
)
