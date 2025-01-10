cc_binary(
    name = "basic-tutorial",
    srcs = [
      "basic-tutorial.c",
    ],
    copts = [
      "-x", "c",
      "-Wno-error=write-strings",
    ],
    deps = [
        "@glib//:glib",
        "@gstreamer",
    ],
    visibility = ["//visibility:public"],
)
