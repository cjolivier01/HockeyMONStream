
cc_library(
    name = "yaml-cpp",
    hdrs = glob(["include/yaml-cpp/**/*.h"]),
    srcs = glob(["src/**/*.h", "src/**/*.cpp"]),
    copts = [
        "-g",
        "-include",
        "stdint.h",
    ],
    includes = ["include"],
    visibility = ["//visibility:public"],
)
