cc_library(
    name = "qt_core",
    hdrs = glob(["include/x86_64-linux-gnu/qt6/QtCore/**"]),
    includes = ["include/x86_64-linux-gnu/qt6"],
    linkopts = ["-lQt6Core"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "qt_widgets",
    hdrs = glob([
        "include/x86_64-linux-gnu/qt6/QtWidgets/**",
        "include/x86_64-linux-gnu/qt6/QtGui/**",
    ]),
    includes = ["include/x86_64-linux-gnu/qt6"],
    deps = [":qt_core"],
    linkopts = [
        "-lQt6Widgets",
        "-lQt6Gui",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "qt_gui",
    hdrs = glob(["include/x86_64-linux-gnu/qt6/QtGui/**"]),
    includes = ["include/x86_64-linux-gnu/qt6"],
    deps = [":qt_core"],
    linkopts = ["-lQt6Gui"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "qt_rcc",
    srcs = ["bin/rcc"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "qt_moc",
    srcs = ["bin/moc"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "qt_uic",
    srcs = ["bin/uic"],
    visibility = ["//visibility:public"],
)
