load("@rules_cc//cc:defs.bzl", "cc_library")

config_setting(
    name = "aarch64",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64",
    constraint_values = ["@platforms//cpu:x86_64"],
)

cc_library(
    name = "dbus",
    hdrs = glob([
        # Expose dbus headers; actual headers are under include/dbus-1.0
        "include/dbus-1.0/dbus/**/*.h",
        "include/dbus-1.0/dbus/*.h",
    ]) + select({
        ":x86_64": glob([
            "lib/x86_64-linux-gnu/dbus-1.0/include/dbus/**/*.h",
            "lib/x86_64-linux-gnu/dbus-1.0/include/dbus/*.h",
        ]),
        ":aarch64": glob([
            "lib/aarch64-linux-gnu/dbus-1.0/include/dbus/**/*.h",
            "lib/aarch64-linux-gnu/dbus-1.0/include/dbus/*.h",
        ]),
        "//conditions:default": [],
    }),
    includes = [
        "include/dbus-1.0",
    ] + select({
        ":x86_64": ["lib/x86_64-linux-gnu/dbus-1.0/include"],
        ":aarch64": ["lib/aarch64-linux-gnu/dbus-1.0/include"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
