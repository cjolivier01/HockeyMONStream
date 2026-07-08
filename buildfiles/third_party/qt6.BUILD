config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

qt_includes = [
    "include",
] + select({
    ":aarch64-linux-gnu": ["include/aarch64-linux-gnu/qt6"],
    ":x86_64-linux-gnu": ["include/x86_64-linux-gnu/qt6"],
    "//conditions:default": ["include/x86_64-linux-gnu/qt6"],
})

qt_lib_dirs = select({
    ":aarch64-linux-gnu": ["-Llib/aarch64-linux-gnu"],
    ":x86_64-linux-gnu": ["-Llib/x86_64-linux-gnu"],
    "//conditions:default": [],
})

cc_library(
    name = "qt_core",
    hdrs = glob([
        "include/**/qt6/QtCore/**",
    ]),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Core"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "qt_gui",
    hdrs = glob(["include/**/qt6/QtGui/**"]),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Gui"],
    visibility = ["//visibility:public"],
    deps = [":qt_core"],
)

cc_library(
    name = "qt_widgets",
    hdrs = glob(["include/**/qt6/QtWidgets/**"]),
    includes = qt_includes,
    linkopts = qt_lib_dirs + ["-lQt6Widgets"],
    visibility = ["//visibility:public"],
    deps = [
        ":qt_core",
        ":qt_gui",
    ],
)

cc_library(
    name = "qt_test",
    hdrs = glob(["include/**/qt6/QtTest/**"]),
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
