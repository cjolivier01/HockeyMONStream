"""Discover the optional native HIP SDK without changing CUDA toolchains."""

def _sdk_linkopts(directory):
    lib = directory.replace("'", "'\\''")
    return ["'-Wl,-rpath-link," + lib + "'", "'-Wl,-rpath," + lib + "'"]

def _resolve_migraphx_root(ctx, root):
    configured = ctx.os.environ.get("MIGRAPHX_PATH")
    if configured:
        return configured
    if ctx.path(root + "/lib/libmigraphx_c.so").exists:
        return root

    # AMD packages install core-<major>.<minor> and extras-<major> as siblings.
    # Resolve bin first because /opt/rocm/bin may be managed by alternatives.
    core = ctx.path(root + "/bin").realpath.dirname
    if core.basename.startswith("core-"):
        major = core.basename[len("core-"):].split(".")[0]
        extras = core.dirname.get_child("extras-" + major)
        if extras.get_child("lib").get_child("libmigraphx_c.so").exists:
            return str(extras)
    return root

def _impl(ctx):
    root = ctx.os.environ.get("ROCM_PATH", "/opt/rocm")
    if not ctx.path(root + "/bin/hipcc").exists:
        fail("HIP targets require ROCm with bin/hipcc; set --repo_env=ROCM_PATH=/path/to/rocm")
    if root == "/opt/rocm":
        # Pin the packaged core release instead of embedding movable alternatives.
        core = ctx.path(root + "/bin").realpath.dirname
        if core.basename.startswith("core-") and ctx.path(root + "/lib").realpath == core.get_child("lib"):
            root = str(core)
    version = ctx.execute([root + "/bin/hipcc", "--version"], environment = {"HIP_PLATFORM": "amd"})
    if version.return_code:
        fail("Cannot execute ROCm hipcc: " + version.stderr)
    configured = ctx.os.environ.get("HSTREAM_AMDGPU_TARGETS", "")
    if configured:
        targets = configured.split(",")
    else:
        detected = ctx.execute([root + "/bin/rocm_agent_enumerator", "-name"])
        targets = [
            value.strip()
            for value in detected.stdout.splitlines()
            if value.startswith("gfx") and value != "gfx000" and "-generic" not in value
        ]
    if not targets:
        fail("No AMD GPU target found; set --repo_env=HSTREAM_AMDGPU_TARGETS=gfx1200 (or your GPU architecture)")
    for target in targets:
        if (not target.startswith("gfx") or len(target) < 5 or
            any([c not in "0123456789abcdef" for c in target[3:].elems()])):
            fail("Invalid HSTREAM_AMDGPU_TARGETS architecture: " + target)
    targets = sorted({target: True for target in targets}.keys())
    # Record target and SDK identity in the action's tool input. Depending on
    # hipcc's implicit device detection would incorrectly reuse cached objects
    # after a GPU change. Cross-machine builds can supply explicit architectures.
    ctx.file(
        "hipcc-wrapper",
        "#!/bin/sh\n" + "\n".join(["# " + line for line in version.stdout.splitlines()]) +
        "\nexport HIP_PLATFORM=amd\nexec '" + root.replace("'", "'\\''") + "/bin/hipcc' " +
        " ".join(["--offload-arch=" + target for target in targets]) + ' "$@"\n',
        executable = True,
    )
    for directory in ["include", "lib", "bin", "llvm", "amdgcn"]:
        if ctx.path(root + "/" + directory).exists:
            ctx.symlink(root + "/" + directory, directory)
    migraphx_root = _resolve_migraphx_root(ctx, root)
    for directory in ["include", "lib"]:
        path = ctx.path(migraphx_root + "/" + directory)
        if path.exists:
            ctx.symlink(path, "migraphx/" + directory)
    migraphx_lib = migraphx_root + "/lib"
    migraphx_linkopts = _sdk_linkopts(migraphx_lib)
    # AMD extras ship the C++ libraries used by the C API in a nested directory.
    if ctx.path(migraphx_lib + "/migraphx/lib").exists:
        migraphx_linkopts += _sdk_linkopts(migraphx_lib + "/migraphx/lib")
    ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])
exports_files(["hipcc-wrapper"])
filegroup(name = "hipcc", srcs = ["hipcc-wrapper"])
alias(name = "rocm_sdk_core", actual = ":runtime")
alias(name = "hip_runtime", actual = ":runtime")
cc_library(
    name = "runtime",
    hdrs = glob(["include/hip/**", "include/hiprand/**"]),
    includes = ["include"],
    defines = ["__HIP_PLATFORM_AMD__", "HSTREAM_USE_HIP"],
    linkopts = %r,
    deps = [":amdhip64"],
)
cc_import(name = "amdhip64", shared_library = "lib/libamdhip64.so")
cc_library(
    name = "migraphx",
    hdrs = glob(["migraphx/include/migraphx/**"]),
    includes = ["migraphx/include"],
    linkopts = %r,
    deps = [":migraphx_c", ":runtime"],
)
cc_import(name = "migraphx_c", shared_library = "migraphx/lib/libmigraphx_c.so")
""" % (_sdk_linkopts(root + "/lib"), migraphx_linkopts))

rocm_repository = repository_rule(
    implementation = _impl,
    environ = ["ROCM_PATH", "MIGRAPHX_PATH", "HSTREAM_AMDGPU_TARGETS"],
    local = True,
)
