"""Repository rules for local paths with host/sysroot fallbacks."""

def _conditional_local_repository_impl(repository_ctx):
    selected_path = None
    for candidate in repository_ctx.attr.paths:
        candidate_path = repository_ctx.path(candidate)
        if candidate_path.exists:
            selected_path = candidate_path
            break

    if selected_path == None:
        fail(
            "Repository '{}' could not find any existing path from: {}".format(
                repository_ctx.name,
                repository_ctx.attr.paths,
            ),
        )

    for child in selected_path.readdir():
        basename = child.basename
        if basename in ["BUILD", "BUILD.bazel", "WORKSPACE", "WORKSPACE.bazel"]:
            continue
        repository_ctx.symlink(child, basename)

    repository_ctx.symlink(repository_ctx.path(repository_ctx.attr.build_file), "BUILD.bazel")

conditional_local_repository = repository_rule(
    implementation = _conditional_local_repository_impl,
    attrs = {
        "build_file": attr.label(mandatory = True, allow_single_file = True),
        "paths": attr.string_list(mandatory = True),
    },
    local = True,
)
