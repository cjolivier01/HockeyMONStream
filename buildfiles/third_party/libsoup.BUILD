cc_library(
    name = "libsoup",
    hdrs = glob([
        "include/libsoup-2.4/**/*.h*",
        "include/nlohmann/**/*.h*",
    ]),
    includes = [
        "include/libsoup-2.4",
        "include/nlohmann",
    ],
    # No explicit -L: the multiarch directory is already a default linker
    # search path (and the Jetson toolchain supplies its sysroot equivalent).
    # Naming it here only hoisted /usr/lib/<triple> above every later -L,
    # including DeepStream's, which let stray copies of libnvbufsurface.so
    # and libnvbufsurftransform.so in /usr win over the installed SDK.
    linkopts = [
        "-l:libsoup-2.4.so",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@glib",
        "@json_glib",
    ],
)
