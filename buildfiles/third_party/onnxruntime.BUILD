package(default_visibility = ["//visibility:public"])

licenses(["notice"])

cc_import(
    name = "onnxruntime_shared",
    # Reference the SONAME symlink so Bazel's runfiles preserve the loader name
    # (libonnxruntime.so.1), not only the fully-versioned backing file.
    shared_library = "lib/libonnxruntime.so.1",
)

cc_library(
    name = "onnxruntime",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    deps = [":onnxruntime_shared"],
)

filegroup(
    name = "runtime_files",
    srcs = [
        "LICENSE",
        "ThirdPartyNotices.txt",
        "lib/libonnxruntime.so.1.23.2",
    ],
)
