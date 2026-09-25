"""Select a coherent TensorRT SDK matching the installed DeepStream runtime."""

def _major_from_header(text):
    values = {}
    for line in text.splitlines():
        parts = [word for word in line.replace("\t", " ").split(" ") if word]
        if len(parts) >= 3 and parts[0] == "#define":
            values[parts[1]] = parts[2]
    value = values.get("NV_TENSORRT_MAJOR", "")
    return values.get(value, value)

def _player_tensorrt_sdk_impl(ctx):
    root = ctx.os.environ.get("HSTREAM_PLAYER_TENSORRT_SDK_ROOT")
    cross = ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1"
    if not root:
        root = "/opt/jetson-sysroot/usr" if cross else "/usr"
    include = None
    lib = None
    for suffix in ["include", "include/aarch64-linux-gnu", "include/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + suffix)
        if candidate.get_child("NvInfer.h").exists:
            include = candidate
            break
    for suffix in ["lib", "lib64", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + suffix)
        if candidate.get_child("libnvinfer.so").exists and candidate.get_child("libnvonnxparser.so").exists:
            lib = candidate
            break
    if not include or not lib:
        fail("No complete player TensorRT SDK at {}. Set HSTREAM_PLAYER_TENSORRT_SDK_ROOT.".format(root))
    major = _major_from_header(ctx.read(include.get_child("NvInferVersion.h")))
    if not major or not major.isdigit():
        fail("Cannot determine player TensorRT header major at {}".format(include))
    readelf = ctx.which("readelf")
    if not readelf:
        fail("readelf is required to verify the player TensorRT SDK.")
    for name in ["nvinfer", "nvonnxparser"]:
        result = ctx.execute([readelf, "-d", lib.get_child("lib" + name + ".so")])
        expected = "lib" + name + ".so." + major + "]"
        if result.return_code or expected not in result.stdout:
            fail("Player TensorRT {} headers disagree with {} library. Select a coherent SDK.".format(major, name))
    ds_root = "/opt/jetson-sysroot/opt/nvidia/deepstream/deepstream" if cross else "/opt/nvidia/deepstream/deepstream"
    ds_infer = ctx.path(ds_root + "/lib/libnvds_infer.so")
    if ds_infer.exists:
        result = ctx.execute([readelf, "-d", ds_infer])
        if result.return_code or "libnvinfer.so." + major + "]" not in result.stdout:
            fail("Player TensorRT SDK major {} disagrees with DeepStream. Set HSTREAM_PLAYER_TENSORRT_SDK_ROOT to its SDK.".format(major))
    ctx.symlink(include, "include")
    ctx.symlink(lib.get_child("libnvinfer.so"), "lib/libnvinfer.so")
    ctx.symlink(lib.get_child("libnvonnxparser.so"), "lib/libnvonnxparser.so")
    ctx.file("BUILD.bazel", """
cc_import(name = "infer", shared_library = "lib/libnvinfer.so")
cc_import(name = "onnx_parser", shared_library = "lib/libnvonnxparser.so")
cc_library(
    name = "runtime",
    hdrs = glob(["include/**/*.h"]),
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
    environ = ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH"],
    local = True,
)
