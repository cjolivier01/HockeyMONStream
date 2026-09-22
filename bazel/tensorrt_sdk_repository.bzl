"""Select one coherent TensorRT SDK for the offline detector engine builder."""

def _tensorrt_sdk_impl(ctx):
    root = ctx.os.environ.get("HSTREAM_TENSORRT_SDK_ROOT")
    if not root:
        root = "/opt/jetson-sysroot/usr" if ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1" else "/usr"
    includes = None
    libraries = None
    for relative in ["include", "include/aarch64-linux-gnu", "include/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + relative)
        if candidate.get_child("NvInfer.h").exists:
            includes = candidate
            break
    for relative in ["lib", "lib64", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + relative)
        if candidate.get_child("libnvinfer.so").exists and candidate.get_child("libnvonnxparser.so").exists:
            libraries = candidate
            break
    if includes == None or libraries == None:
        fail("No complete TensorRT SDK at {}. Set HSTREAM_TENSORRT_SDK_ROOT to the SDK used by DeepStream.".format(root))
    ctx.symlink(includes, "include")
    ctx.symlink(libraries.get_child("libnvinfer.so"), "lib/libnvinfer.so")
    ctx.symlink(libraries.get_child("libnvonnxparser.so"), "lib/libnvonnxparser.so")
    ctx.file("BUILD.bazel", """
cc_import(name = "infer", shared_library = "lib/libnvinfer.so")
cc_import(name = "onnx_parser", shared_library = "lib/libnvonnxparser.so")
cc_library(
    name = "sdk",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    deps = [":infer", ":onnx_parser"],
    visibility = ["//visibility:public"],
)
""")

tensorrt_sdk_repository = repository_rule(
    implementation = _tensorrt_sdk_impl,
    environ = ["HSTREAM_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH"],
    local = True,
)
