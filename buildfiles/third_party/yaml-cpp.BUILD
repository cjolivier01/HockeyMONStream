
cc_library(
    name = "yaml-cpp",
    hdrs = glob(["include/yaml-cpp/**/*.h"]),
    srcs = glob(["src/**/*.h", "src/**/*.cpp"]),
    # Some yaml-cpp versions assume uint16_t is already available; force the include for robustness.
    copts = ["-g", "-include", "cstdint"],
    includes = ["include"],
    visibility = ["//visibility:public"],
)
