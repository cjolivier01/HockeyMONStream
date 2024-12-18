load("@com_extension_dev//build:graph_extension.bzl", "graph_cc_extension")

cc_binary(
    name = "basic-tutorial-2",
    srcs = [
      "basic-tutorial-2.cpp",
    ],
    linkopts=[
      "-Wl,--unresolved-symbols=ignore-all",
      "-Wl,-t",
      "-Wl,-L/usr/local/lib/x86_64-linux-gnu",
      "-Wl,-L/usr/local/lib",
      # "-Wl,--start-group",
      # "-lgstreamer-1.0",
      # "-lgstbase-1.0",
      # "-Wl,--end-group",
    ],
    deps = [
        "@glib",
        "@gstreamer",
    ],
    visibility = ["//visibility:public"],
)
