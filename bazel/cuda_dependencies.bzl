"""Keep NVIDIA-only consumers and their dependency graph on the CUDA backend.

The native C++/CUDA rule keeps its public label and output paths. Only its
dependency configuration changes, so shared libraries can also be built with
HIP elsewhere in the same invocation without leaking HIP headers or defines
into a DeepStream/TensorRT translation unit.
"""

def _cuda_configuration_impl(settings, attr):
    return {"@kstream//:cuda_only": settings["@kstream//:cuda_only"] or attr.force_cuda}

_cuda_configuration = transition(
    implementation = _cuda_configuration_impl,
    inputs = ["@kstream//:cuda_only"],
    outputs = ["@kstream//:cuda_only"],
)

def _cuda_dependencies_impl(ctx):
    runfiles = ctx.runfiles()
    for dep in ctx.attr.deps:
        runfiles = runfiles.merge(dep[DefaultInfo].default_runfiles)
    return [
        cc_common.merge_cc_infos(cc_infos = [dep[CcInfo] for dep in ctx.attr.deps]),
        DefaultInfo(
            files = depset(transitive = [dep[DefaultInfo].files for dep in ctx.attr.deps]),
            runfiles = runfiles,
        ),
        coverage_common.instrumented_files_info(ctx, dependency_attributes = ["deps"]),
    ]

_cuda_dependencies = rule(
    implementation = _cuda_dependencies_impl,
    attrs = {
        "deps": attr.label_list(cfg = _cuda_configuration, providers = [CcInfo]),
        "force_cuda": attr.bool(),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def cuda_dependencies(name, deps, testonly = False):
    """Return CUDA-configured dependencies for an NVIDIA-only native rule."""
    _cuda_dependencies(
        name = name + "_cuda_dependencies",
        deps = deps,
        force_cuda = select({
            "@kstream//:use_hip": True,
            "@kstream//:use_jetson_hip": True,
            "//conditions:default": False,
        }),
        testonly = testonly,
        tags = ["manual"],
        visibility = ["//visibility:private"],
    )
    return [":" + name + "_cuda_dependencies"]
