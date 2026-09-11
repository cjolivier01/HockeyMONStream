load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "bluez",
    hdrs = glob([
        "include/bluetooth/**/*.h",
        "include/bluetooth/*.h",
    ]),
    # This repository points at /usr. An early -isystem of its include directory
    # breaks libstdc++'s include_next lookup of libc headers. Stage only Bluetooth.
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
)
