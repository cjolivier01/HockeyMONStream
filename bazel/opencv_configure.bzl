def _path_exists(ctx, p):
    return ctx.path(p).exists

def _detect_opencv(ctx, root):
    # Prefer newer OpenCV first, if present.
    for version in ["opencv5", "opencv4"]:
        include_dir = root + "/include/" + version
        core_hpp = include_dir + "/opencv2/core.hpp"
        if _path_exists(ctx, core_hpp):
            return {
                "root": root,
                "version": version,
                "include_dir": include_dir,
                "has_cudawarping": _path_exists(ctx, include_dir + "/opencv2/cudawarping.hpp"),
                "has_cudawarping_lib": (
                    _path_exists(ctx, root + "/lib/libopencv_cudawarping.so") or
                    _path_exists(ctx, root + "/lib/libopencv_cudawarping.so.500") or
                    _path_exists(ctx, root + "/lib/x86_64-linux-gnu/libopencv_cudawarping.so") or
                    _path_exists(ctx, root + "/lib/aarch64-linux-gnu/libopencv_cudawarping.so")
                ),
            }
    return None

def _build_file_content(opencv_version, use_conda, has_cudawarping, conda_lib_dir = None):
    libs = [
        "libopencv_core.so",
        "libopencv_highgui.so",
        "libopencv_imgcodecs.so",
        "libopencv_imgproc.so",
        "libopencv_video.so",
        "libopencv_videoio.so",
    ]
    if has_cudawarping:
        libs.append("libopencv_cudawarping.so")

    linkopts = []

    # Conda OpenCV must be selected explicitly, otherwise the linker can pick up system OpenCV
    # (e.g. from /usr/lib/x86_64-linux-gnu) and cause ABI mismatches (OpenCV4 vs OpenCV5).
    if use_conda:
        if not conda_lib_dir:
            fail("conda_lib_dir is required when use_conda=True")
        linkopts = [conda_lib_dir + "/" + lib for lib in libs]
    else:
        linkopts = ["-l:" + lib for lib in libs]

    # Note: the repo rule symlinks the OpenCV headers to `<opencv_version>/...`,
    # so includes just needs to add that directory.
    return """\
cc_library(
    name = "opencv",
    hdrs = glob([
        "{v}/opencv2/*.h*",
        "{v}/opencv2/**/*.h*",
    ]),
    includes = [
        "{v}",
    ],
    linkopts = {linkopts},
    visibility = ["//visibility:public"],
)
""".format(v = opencv_version, linkopts = repr(linkopts))

def _opencv_configure_impl(ctx):
    conda_prefix = ctx.os.environ.get("CONDA_PREFIX")

    # Probe candidates in priority order.
    candidates = []
    if conda_prefix:
        candidates.append(("conda", conda_prefix))
    candidates.append(("system", "/usr"))

    chosen = None
    chosen_kind = None
    for (kind, root) in candidates:
        det = _detect_opencv(ctx, root)
        if det:
            chosen = det
            chosen_kind = kind
            break

    if not chosen:
        fail("OpenCV headers not found under CONDA_PREFIX or /usr (expected include/opencv4|opencv5/opencv2/core.hpp)")

    opencv_version = chosen["version"]
    use_conda = (chosen_kind == "conda")

    # Only advertise cudawarping if both the header and a shared library exist.
    has_cudawarping = chosen["has_cudawarping"] and chosen["has_cudawarping_lib"]

    # Keep the repository small: only expose the OpenCV include tree and lib dir.
    ctx.symlink(chosen["root"] + "/include/" + opencv_version, opencv_version)

    # For conda OpenCV, libs live in `<prefix>/lib`.
    # For system OpenCV, we still create `lib` so downstream users can add `-L` if desired.
    if _path_exists(ctx, chosen["root"] + "/lib"):
        ctx.symlink(chosen["root"] + "/lib", "lib")

    # Write the repository BUILD/WORKSPACE files.
    ctx.file("WORKSPACE", "workspace(name = \"{name}\")\n".format(name = ctx.name))
    ctx.file(
        "BUILD.bazel",
        _build_file_content(
            opencv_version,
            use_conda,
            has_cudawarping,
            conda_lib_dir = (chosen["root"] + "/lib") if use_conda else None,
        ),
    )

opencv_configure = repository_rule(
    implementation = _opencv_configure_impl,
    environ = ["CONDA_PREFIX"],
    doc = "Autoconfigures OpenCV headers/libs, preferring CONDA_PREFIX OpenCV5 when available.",
)
