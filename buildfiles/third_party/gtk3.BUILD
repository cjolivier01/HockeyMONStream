
cc_library(
    name = "gtk3",
    hdrs = glob(["**/*.h"]),
    copts = [
    ],
    linkopts = [
        "-lgtk-3",
        "-lgdk-3",
    ],    
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = ["@pango", "@cairo", "@gdk-pixbuf", "@atk", "@gstreamer"],
)


cc_library(
    name = "pango",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    linkopts = [
      "-lpango-1.0",
      "-lpangocairo-1.0",
    ],
    visibility = ["//visibility:public"],
    deps = [
      "@harfbuzz",
      "@cairo",
    ]
)

cc_library(
    name = "harfbuzz",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    linkopts = [
      "-lharfbuzz",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "cairo",
    hdrs = glob(["**/*.h"]),
    linkopts = [
      "-lcairo-gobject",
      "-lcairo ",
    ],
    includes = ["."],
    deps = [
      "@glib",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gdk-pixbuf",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    linkopts = [
      "-lgdk_pixbuf-2.0",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "atk",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    linkopts = [
      "-latk-1.0",
    ],
    visibility = ["//visibility:public"],
)
