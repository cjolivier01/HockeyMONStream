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
# Do not widen these to "include/**/qt6/<module>/**": this repository's root is
# /usr, so a leading "include/**" makes Bazel walk every directory under
# /usr/include, and one bad symlink anywhere in there fails the whole build
# with an error that names Qt. A second `ln -sf /usr/include/eigen3/Eigen
# /usr/include/Eigen` leaves exactly that behind, since ln follows the existing
# link and writes eigen3/Eigen/Eigen -> eigen3/Eigen over eigen's own header.
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
