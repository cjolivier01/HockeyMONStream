cc_library(
    name = "mesa_interop",
    hdrs = glob(["include/EGL/*.h", "include/GLES2/*.h", "include/KHR/*.h", "include/gbm.h"]),
    includes = ["include"],
    linkopts = ["-lEGL", "-lGLESv2", "-lgbm", "-lva-drm"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gstreamer_gl",
    hdrs = glob(["lib/x86_64-linux-gnu/gstreamer-1.0/include/gst/gl/*.h"]),
    includes = ["lib/x86_64-linux-gnu/gstreamer-1.0/include"],
    linkopts = ["-lgstgl-1.0"],
    visibility = ["//visibility:public"],
    deps = ["@gstreamer"],
)
