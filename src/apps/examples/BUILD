cc_binary(
    name = "basic-tutorial",
    srcs = [
      "basic-tutorial.cpp",
    ],
    linkopts=[
      # "-Wl,--unresolved-symbols=ignore-all",
      # "-Wl,-t",
      # "-Wl,-L/usr/local/lib/x86_64-linux-gnu",
      # "-Wl,-L/usr/local/lib",
    ],
    deps = [
        "@glib",
        "@gstreamer",
    ],
    visibility = ["//visibility:public"],
)
