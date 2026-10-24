cc_library(
    name = "mesa_interop",
    hdrs = glob(["include/EGL/*.h", "include/GLES2/*.h", "include/KHR/*.h", "include/gbm.h"]),
    includes = ["include"],
    linkopts = ["-lEGL", "-lGLESv2", "-lgbm", "-lva-drm"],
    visibility = ["//visibility:public"],
)
