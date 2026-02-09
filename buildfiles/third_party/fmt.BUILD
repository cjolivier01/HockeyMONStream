load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "fmt",
    hdrs = glob([
        "include/fmt/*.h",
        "include/fmt/**/*.h",
    ]),
    includes = [
        "include",
    ],
    copts = [
        "-DFMT_HEADER_ONLY",
    ],
    visibility = ["//visibility:public"],
)

