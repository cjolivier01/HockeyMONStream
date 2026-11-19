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
#
# Keep the leading segments. The rule for every repository whose root can be
# /usr, /usr/include or a sysroot: anchor the first wildcard below a literal
# subdirectory, and never let "**" sit directly under the root. Otherwise
# Bazel walks the whole system include tree, and one symlink loop anywhere in
# it fails the build with an error naming this repository and a path with no
# Qt in it.
#
# allow_empty is needed because one of the two roots is always empty on any
# single architecture, and --incompatible_disallow_empty_glob checks per
# pattern. That flag is off on the pinned Bazel 6.5, so this changes nothing
# today, but it does mean a missing Qt install surfaces as a compile error
# rather than a load-time one; "includes" below names the same two paths.
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
