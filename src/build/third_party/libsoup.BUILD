config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

cc_library(
    name = "libsoup",
    hdrs = glob([
        "include/libsoup-2.4/**/*.h*",
        "include/nlohmann/**/*.h*",
    ]),
    includes = [
        "include/libsoup-2.4",
        "include/nlohmann",
    ],
    linkopts =
        select({
            ":aarch64-linux-gnu": ["-Llib/aarch64-linux-gnu"],
            ":x86_64-linux-gnu": ["-Llib/x86_64-linux-gnu"],
            "//conditions:default": [],
        }) + [
            "-llibsoup-2.4.so",
        ],
    visibility = ["//visibility:public"],
    deps = [
        "@glib",
        "@json_glib",
    ],
)
