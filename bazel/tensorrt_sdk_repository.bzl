"""Select one coherent TensorRT SDK for the offline detector engine builder."""

load("//bazel:tensorrt_sdk_common.bzl", "select_sdk")

def _tensorrt_sdk_impl(ctx):
    include, libraries = select_sdk(ctx, "HSTREAM_TENSORRT_SDK_ROOT")
    ctx.symlink(include, "include")
    ctx.symlink(libraries["nvinfer"], "lib/libnvinfer.so")
    ctx.symlink(libraries["nvonnxparser"], "lib/libnvonnxparser.so")
    ctx.file("BUILD.bazel", """
cc_import(name = "infer", shared_library = "lib/libnvinfer.so")
cc_import(name = "onnx_parser", shared_library = "lib/libnvonnxparser.so")
cc_library(
    name = "sdk",
    # A private header tree avoids adding an alias of /usr/include ahead of
    # libstdc++, which breaks its #include_next <stdlib.h> lookup.
    hdrs = glob(["include/Nv*.h"]),
    strip_include_prefix = "include",
    deps = [":infer", ":onnx_parser"],
    visibility = ["//visibility:public"],
)
""")

tensorrt_sdk_repository = repository_rule(
    implementation = _tensorrt_sdk_impl,
    environ = ["HSTREAM_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH", "DEEPSTREAM_ROOT"],
    local = True,
)
