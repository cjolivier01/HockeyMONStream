load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "bluez",
    hdrs = glob([
        "include/bluetooth/**/*.h",
        "include/bluetooth/*.h",
    ]),
    includes = [
        "include",
    ],
    visibility = ["//visibility:public"],
)

