"""Import ONNX Runtime with adjacent dlopen-only providers in Bazel runfiles."""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

def _impl(ctx):
    toolchain = find_cpp_toolchain(ctx)
    features = cc_common.configure_features(ctx = ctx, cc_toolchain = toolchain)
    libraries = []
    for src in [ctx.file.library] + ctx.files.providers:
        libraries.append(cc_common.create_library_to_link(
            actions = ctx.actions,
            feature_configuration = features,
            cc_toolchain = toolchain,
            dynamic_library = src,
            dynamic_library_symlink_path = "hstream_onnxruntime/" + src.basename,
        ))
    # CUDA provider static initialization requires ORT to set its host first.
    # Only link the core; the providers must be loaded by ORT, not the ELF loader.
    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        libraries = depset([libraries[0]]),
    )
    runtime = [library.dynamic_library for library in libraries] + ctx.files.providers
    return [
        CcInfo(linking_context = cc_common.create_linking_context(linker_inputs = depset([linker_input]))),
        DefaultInfo(files = depset(runtime), runfiles = ctx.runfiles(files = runtime)),
    ]

onnxruntime_import = rule(
    implementation = _impl,
    attrs = {
        "library": attr.label(allow_single_file = True, mandatory = True),
        "providers": attr.label_list(allow_files = True),
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
    },
    fragments = ["cpp"],
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
)
