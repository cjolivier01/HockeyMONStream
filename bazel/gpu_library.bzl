"""Compile shared HStream kernels with NVCC or native HIP.

HIP is opt-in. Headers and defines come from the same CcInfo dependencies as
host translation units; no generated copies of source or include trees.
"""

load("@rules_cuda//cuda:defs.bzl", "cuda_library")

def _hip_objects_impl(ctx):
    contexts = [dep[CcInfo].compilation_context for dep in ctx.attr.deps]
    merged = cc_common.merge_compilation_contexts(compilation_contexts = contexts)
    objects = []
    for src in ctx.files.srcs:
        obj = ctx.actions.declare_file(ctx.label.name + "/" + src.basename + ".o")
        args = ctx.actions.args()
        args.add_all(["-x", "hip", "-std=c++17", "-fPIC", "-c"])
        args.add_all(["-O0", "-g"] if ctx.var["COMPILATION_MODE"] == "dbg" else ["-O2"])
        args.add_all(ctx.attr.copts)
        args.add_all(merged.defines, format_each = "-D%s")
        args.add_all(merged.includes, format_each = "-I%s")
        args.add_all(merged.quote_includes, before_each = "-iquote")
        args.add_all(merged.system_includes, before_each = "-isystem")
        args.add_all(["-I.", "-I" + ctx.bin_dir.path])
        args.add(src)
        args.add("-o", obj)
        ctx.actions.run(
            executable = ctx.executable._hipcc,
            arguments = [args],
            inputs = depset([src], transitive = [merged.headers]),
            outputs = [obj],
            mnemonic = "HipCompile",
            # ROCm's compiler invokes its sibling tools and reads device libraries.
            # This is a local installed toolchain, like the detected CUDA SDK.
            execution_requirements = {"no-sandbox": "1", "local": "1"},
            use_default_shell_env = True,
        )
        objects.append(obj)
    return [DefaultInfo(files = depset(objects))]

_hip_objects = rule(
    implementation = _hip_objects_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".cu"]),
        "deps": attr.label_list(providers = [CcInfo]),
        "copts": attr.string_list(),
        "_hipcc": attr.label(
            default = "@hstream_rocm//:hipcc-wrapper",
            executable = True,
            cfg = "exec",
            allow_single_file = True,
        ),
    },
)

def gpu_library(name, srcs, deps = [], visibility = None):
    """A GPU library whose source uses hstream/src/libs/gpu/Runtime.h."""
    cuda_library(
        name = name + "_nvcc",
        srcs = srcs,
        deps = deps,
        tags = ["manual"],
        target_compatible_with = select({"//:use_hip": ["@platforms//:incompatible"], "//conditions:default": []}),
    )
    _hip_objects(
        name = name + "_hip_objects",
        srcs = srcs,
        deps = deps,
        tags = ["manual"],
        target_compatible_with = select({"//:use_hip": [], "//conditions:default": ["@platforms//:incompatible"]}),
    )
    native.cc_library(
        name = name + "_hip",
        srcs = [":" + name + "_hip_objects"],
        deps = deps,
        tags = ["manual"],
        target_compatible_with = select({"//:use_hip": [], "//conditions:default": ["@platforms//:incompatible"]}),
    )
    native.alias(
        name = name,
        actual = select({"//:use_hip": ":" + name + "_hip", "//conditions:default": ":" + name + "_nvcc"}),
        visibility = visibility,
    )
