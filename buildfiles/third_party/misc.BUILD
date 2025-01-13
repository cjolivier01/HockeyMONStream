# Bazel is only available for amd64 and arm64.

config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

cc_library(
    name = "misc",
    hdrs = [
        # I hate Bazel...
        "stdc-predef.h",
    ],
    visibility = ["//visibility:public"],
)
