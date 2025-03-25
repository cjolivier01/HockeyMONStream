cc_library(
    name = "gstreamer",
    hdrs = glob([
        "include/gstreamer-1.0/**/*.h",
        "include/glib-2.0/**/*.h",
        "lib/x86_64-linux-gnu/glib-2.0/include/**/*.h",
    ]),
    includes = [
        "include/gstreamer-1.0",
        "include/glib-2.0",
        "lib/x86_64-linux-gnu/glib-2.0/include",
    ],
    linkopts = [
        "-lgstreamer-1.0",
        "-lgobject-2.0",
        "-lglib-2.0",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gstreamer_app",
    hdrs = glob(["include/gstreamer-1.0/gst/app/**/*.h"]),
    includes = ["include/gstreamer-1.0"],
    deps = [":gstreamer"],
    linkopts = ["-lgstapp-1.0"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gstreamer_video",
    hdrs = glob(["include/gstreamer-1.0/gst/video/**/*.h"]),
    includes = ["include/gstreamer-1.0"],
    deps = [":gstreamer"],
    linkopts = ["-lgstvideo-1.0"],
    visibility = ["//visibility:public"],
)
