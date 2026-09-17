"""Assemble an ARM64 native ONNX Runtime SDK from pinned upstream artifacts.

ARM CUDA distributions are wheels. Only their C API and CUDA provider libraries
are used; no Python files, bindings, or TensorRT provider enter the runtime.
"""

def _impl(ctx):
    ctx.download_and_extract(
        url = ctx.attr.headers_url,
        sha256 = ctx.attr.headers_sha256,
        stripPrefix = "onnxruntime-linux-aarch64-" + ctx.attr.headers_version,
    )
    ctx.download_and_extract(
        url = ctx.attr.wheel_url,
        sha256 = ctx.attr.wheel_sha256,
        type = "zip",
        output = "wheel",
    )
    ctx.delete("lib")
    native = "wheel/onnxruntime/capi/"
    runtime = "libonnxruntime.so." + ctx.attr.version
    for library in [runtime, "libonnxruntime_providers_cuda.so", "libonnxruntime_providers_shared.so"]:
        ctx.symlink(native + library, "lib/" + library)
    ctx.symlink("lib/" + runtime, "lib/libonnxruntime.so.1")
    ctx.symlink("lib/" + runtime, "lib/libonnxruntime.so")
    # Use the notices accompanying the binary, and expose its actual version
    # to packaging (Jetson's ABI-24 binary uses the ABI-24 SDK headers).
    for notice in ["LICENSE", "ThirdPartyNotices.txt"]:
        ctx.delete(notice)
        ctx.symlink("wheel/onnxruntime/" + notice, notice)
    ctx.file("VERSION_NUMBER", ctx.attr.version + "\n")
    ctx.symlink(ctx.attr.build_file, "BUILD.bazel")

onnxruntime_gpu_arm64_repository = repository_rule(
    implementation = _impl,
    attrs = {
        "version": attr.string(mandatory = True),
        "headers_version": attr.string(mandatory = True),
        "headers_url": attr.string(mandatory = True),
        "headers_sha256": attr.string(mandatory = True),
        "wheel_url": attr.string(mandatory = True),
        "wheel_sha256": attr.string(mandatory = True),
        "build_file": attr.label(mandatory = True, allow_single_file = True),
    },
)
