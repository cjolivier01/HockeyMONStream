"""Assemble native ONNX Runtime GPU SDKs from pinned upstream packages.

Only the C API and CUDA provider libraries are exposed; no managed/Python
bindings or TensorRT provider enter the runtime.
"""

def _arm64_impl(ctx):
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
    # to packaging.
    for notice in ["LICENSE", "ThirdPartyNotices.txt"]:
        ctx.delete(notice)
        ctx.symlink("wheel/onnxruntime/" + notice, notice)
    ctx.file("VERSION_NUMBER", ctx.attr.version + "\n")
    ctx.symlink(ctx.attr.build_file, "BUILD.bazel")

onnxruntime_gpu_arm64_repository = repository_rule(
    implementation = _arm64_impl,
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

def _x86_64_impl(ctx):
    ctx.download_and_extract(
        url = ctx.attr.package_url,
        integrity = ctx.attr.package_integrity,
        type = "zip",
        output = "package",
    )
    ctx.symlink("package/buildTransitive/native/include", "include")
    native = "package/runtimes/linux-x64/native/"
    runtime = "libonnxruntime.so." + ctx.attr.version
    for library in ["libonnxruntime_providers_cuda.so", "libonnxruntime_providers_shared.so"]:
        ctx.symlink(native + library, "lib/" + library)
    copy = ctx.execute(["cp", native + "libonnxruntime.so", "lib/" + runtime])
    if copy.return_code:
        fail("Failed to materialize the versioned ONNX Runtime library: " + copy.stderr)
    ctx.symlink("lib/" + runtime, "lib/libonnxruntime.so.1")
    ctx.symlink("lib/" + runtime, "lib/libonnxruntime.so")
    for notice in ["LICENSE", "ThirdPartyNotices.txt"]:
        ctx.symlink("package/" + notice, notice)
    ctx.file("VERSION_NUMBER", ctx.attr.version + "\n")
    ctx.symlink(ctx.attr.build_file, "BUILD.bazel")

onnxruntime_gpu_x86_64_repository = repository_rule(
    implementation = _x86_64_impl,
    attrs = {
        "version": attr.string(mandatory = True),
        "package_url": attr.string(mandatory = True),
        "package_integrity": attr.string(mandatory = True),
        "build_file": attr.label(mandatory = True, allow_single_file = True),
    },
)
