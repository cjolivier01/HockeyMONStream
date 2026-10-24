"""Discover the optional native HIP SDK without changing CUDA toolchains."""

def _impl(ctx):
    root = ctx.os.environ.get("ROCM_PATH", "/opt/rocm")
    if not ctx.path(root + "/bin/hipcc").exists:
        fail("HIP targets require ROCm with bin/hipcc; set --repo_env=ROCM_PATH=/path/to/rocm")
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
    ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])
exports_files(["hipcc-wrapper"])
filegroup(name = "hipcc", srcs = ["hipcc-wrapper"])
alias(name = "rocm_sdk_core", actual = ":runtime")
cc_library(
    name = "runtime",
    hdrs = glob(["include/hip/**", "include/hiprand/**"]),
    includes = ["include"],
    defines = ["__HIP_PLATFORM_AMD__", "HSTREAM_USE_HIP"],
    deps = [":amdhip64"],
)
cc_import(name = "amdhip64", shared_library = "lib/libamdhip64.so")
""")

rocm_repository = repository_rule(
    implementation = _impl,
    environ = ["ROCM_PATH", "HSTREAM_AMDGPU_TARGETS"],
    local = True,
)
