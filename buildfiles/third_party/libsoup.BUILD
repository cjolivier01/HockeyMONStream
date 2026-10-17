load(":version.bzl", "SOUP_SONAME")

cc_library(
    name = "libsoup",
    hdrs = glob([
        "include/**/*.h*",
        "nlohmann/**/*.h*",
    ]),
    includes = [
        "include",
        "nlohmann",
    ],
    # No explicit -L: the multiarch directory is already a default linker
    # search path (and the Jetson toolchain supplies its sysroot equivalent).
    # Naming it here only hoisted /usr/lib/<triple> above every later -L,
    # including DeepStream's, which let stray copies of libnvbufsurface.so
    # and libnvbufsurftransform.so in /usr win over the installed SDK.
    linkopts = [
        "-l:" + SOUP_SONAME,
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@glib",
        "@json_glib",
    ],
)
