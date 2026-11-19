config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

# Do not add the entire sysroot include directory as an early -isystem path:
# on a native build it moves /usr/include ahead of libstdc++, breaking its
# include_next <stdlib.h>. Qt headers only need the versioned directory.
qt_includes = select({
    ":aarch64-linux-gnu": ["include/aarch64-linux-gnu/qt6"],
    ":x86_64-linux-gnu": ["include/x86_64-linux-gnu/qt6"],
    "//conditions:default": ["include/x86_64-linux-gnu/qt6"],
})

qt_lib_dirs = select({
    ":aarch64-linux-gnu": ["-Llib/aarch64-linux-gnu"],
    ":x86_64-linux-gnu": ["-Llib/x86_64-linux-gnu"],
    "//conditions:default": [],
})

# The same two directories as literals, because a glob cannot take a select.
# Keep the leading segments. This repository's root is /usr, or a sysroot, so
# "include/**" makes Bazel walk every directory under /usr/include, and a
# single symlink loop anywhere in that tree then fails the build with an error
# that names Qt and points at a path with no Qt in it. Any repository rooted
# at /usr or /usr/include needs the same care; @misc is rooted there and has
# no globs today.
qt_hdr_roots = [
    "include/x86_64-linux-gnu/qt6",
    "include/aarch64-linux-gnu/qt6",
]

cc_library(
    name = "qt_core",
    hdrs = glob(
        [root + "/QtCore/**" for root in qt_hdr_roots],
        allow_empty = True,
    ),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Core"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "qt_gui",
    hdrs = glob(
        [root + "/QtGui/**" for root in qt_hdr_roots],
        allow_empty = True,
    ),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Gui"],
    visibility = ["//visibility:public"],
    deps = [":qt_core"],
)

cc_library(
    name = "qt_widgets",
    hdrs = glob(
        [root + "/QtWidgets/**" for root in qt_hdr_roots],
        allow_empty = True,
    ),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Widgets"],
    visibility = ["//visibility:public"],
    deps = [
        ":qt_core",
        ":qt_gui",
    ],
)

cc_library(
    name = "qt_network",
    hdrs = glob(
        [root + "/QtNetwork/**" for root in qt_hdr_roots],
        allow_empty = True,
    ),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Network"],
    visibility = ["//visibility:public"],
    deps = [":qt_core"],
)

cc_library(
    name = "qt_test",
    hdrs = glob(
        [root + "/QtTest/**" for root in qt_hdr_roots],
        allow_empty = True,
    ),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Test"],
    visibility = ["//visibility:public"],
    deps = [
        ":qt_core",
        ":qt_gui",
        ":qt_widgets",
    ],
)

filegroup(
    name = "qt_moc",
    srcs = ["lib/qt6/libexec/moc"],
    visibility = ["//visibility:public"],
)
