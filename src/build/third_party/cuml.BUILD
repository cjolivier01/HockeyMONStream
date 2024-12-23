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
      "include/spdlog/**/*.h*",
      "include/spdlog/fmt/bundled/*.h",
  ]),
  includes = [
      "include",
  ],
  copts = [
    "-std=c++17",
    "-DSPDLOG_USE_STD_FORMAT",
  ],
  linkopts = [
    "-Llib",
    "-l:libcuml++.so",
  ],
  visibility = ["//visibility:public"],
)
