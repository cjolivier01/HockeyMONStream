"""Select a coherent TensorRT SDK matching the installed DeepStream runtime."""

load("//bazel:tensorrt_sdk_common.bzl", "select_sdk")

def _player_tensorrt_sdk_impl(ctx):
    include, libraries = select_sdk(ctx, "HSTREAM_PLAYER_TENSORRT_SDK_ROOT")
    ctx.symlink(include, "include")
    ctx.symlink(libraries["nvinfer"], "lib/libnvinfer.so")
    ctx.symlink(libraries["nvonnxparser"], "lib/libnvonnxparser.so")
    ctx.file("BUILD.bazel", """
cc_import(name = "infer", shared_library = "lib/libnvinfer.so")
cc_import(name = "onnx_parser", shared_library = "lib/libnvonnxparser.so")
cc_library(
    name = "runtime",
    # Only TensorRT's own headers. select_sdk() can resolve include/ to a
    # system directory, where "include/**/*.h" both re-exports all of
    # /usr/include under hstream_player_tensorrt/ and makes Bazel walk a tree
    # it has no business in. tensorrt_sdk_repository.bzl pins the same set.
    hdrs = glob(["include/Nv*.h"]),
    strip_include_prefix = "include",
    include_prefix = "hstream_player_tensorrt",
    deps = [":infer"],
    visibility = ["//visibility:public"],
)
cc_library(
    name = "sdk",
    deps = [":runtime", ":onnx_parser"],
    visibility = ["//visibility:public"],
)
""")

player_tensorrt_sdk_repository = repository_rule(
    implementation = _player_tensorrt_sdk_impl,
    environ = ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH", "DEEPSTREAM_ROOT"],
    local = True,
)
