"""Select one coherent TensorRT SDK for the offline detector engine builder."""

load(
    "//bazel:tensorrt_sdk_common.bzl",
    "deepstream_infer",
    "include_dir",
    "needed_major",
    "readelf",
    "version_from_header",
)

def _libraries(ctx, root, major):
    # The unversioned lib*.so in /usr is a development symlink owned by
    # whichever -dev package happens to be installed, which need not be the SDK
    # DeepStream loads. Prefer the versioned SONAME, then confirm it really is
    # the one asked for rather than trusting the filename.
    for relative in ["lib", "lib64", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + relative)
        libraries = {}
        for name in ["nvinfer", "nvonnxparser"]:
            versioned = candidate.get_child("lib" + name + ".so." + major)
            libraries[name] = versioned if versioned.exists else candidate.get_child("lib" + name + ".so")
        coherent = all([library.exists for library in libraries.values()])
        for name, library in libraries.items():
            if not coherent:
                break
            expected = "[lib" + name + ".so." + major + "]"
            soname = [line for line in readelf(ctx, library, "-d").splitlines() if "(SONAME)" in line]
            coherent = bool(soname) and expected in soname[0]
        if coherent:
            return libraries
    return None

def _tensorrt_sdk_impl(ctx):
    cross = ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1"
    root = ctx.os.environ.get("HSTREAM_TENSORRT_SDK_ROOT")
    if not root:
        root = "/opt/jetson-sysroot/usr" if cross else "/usr"

    include = include_dir(ctx, root)
    if include == None:
        fail("No TensorRT headers at {}. Set HSTREAM_TENSORRT_SDK_ROOT to the SDK used by DeepStream.".format(root))
    header_version = version_from_header(ctx.read(include.get_child("NvInferVersion.h")))

    # DeepStream's nvinfer is what deserializes the engines this builder writes,
    # so its TensorRT major is the one that matters, not whatever /usr defaults
    # to. Fall back to the headers only when DeepStream is not installed.
    ds_infer = deepstream_infer(ctx, cross)
    major = needed_major(ctx, ds_infer) if ds_infer else header_version[0]
    if not major or not major.isdigit():
        fail("Cannot determine a compatible TensorRT SDK. Set HSTREAM_TENSORRT_SDK_ROOT.")
    if header_version[0] != major:
        fail(("TensorRT headers at {} are major {}, but DeepStream loads libnvinfer.so.{}. " +
              "Engines built against these headers will fail to deserialize at inference " +
              "time. Install the matching development headers or set " +
              "HSTREAM_TENSORRT_SDK_ROOT to the SDK DeepStream uses.").format(
            root,
            header_version[0] or "missing",
            major,
        ))

    libraries = _libraries(ctx, root, major)
    if libraries == None:
        fail("No complete TensorRT {} SDK at {}. Set HSTREAM_TENSORRT_SDK_ROOT to the SDK used by DeepStream.".format(major, root))
    ctx.symlink(include, "include")
    ctx.symlink(libraries["nvinfer"], "lib/libnvinfer.so")
    ctx.symlink(libraries["nvonnxparser"], "lib/libnvonnxparser.so")
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
    environ = ["HSTREAM_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH", "DEEPSTREAM_ROOT"],
    local = True,
)
