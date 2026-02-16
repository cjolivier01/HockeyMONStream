def _path_exists(ctx, p):
    return ctx.path(p).exists

def _looks_like_soname_suffix(s):
    # Starlark strings aren't iterable in Bazel, so keep this simple.
    # We want to prefer the ABI symlink (e.g. `.so.500`) over the full version file (e.g. `.so.5.0.0`).
    return bool(s) and s.find(".") == -1

def _basename(p):
    s = str(p)
    i = s.rfind("/")
    return s[i + 1:] if i != -1 else s

def _detect_opencv_soname_suffix(ctx, root):
    lib_dir = root + "/lib"
    if not _path_exists(ctx, lib_dir):
        return None

    # Conda packages typically provide:
    # - libopencv_core.so      -> libopencv_core.so.<ABI>
    # - libopencv_core.so.<ABI> -> libopencv_core.so.<MAJOR>.<MINOR>.<PATCH>
    #
    # We want the `<ABI>` file name because the binary's DT_NEEDED entry uses it.
    prefix = "libopencv_core.so."
    for entry in ctx.path(lib_dir).readdir():
        name = _basename(entry)
        if name.startswith(prefix):
            suffix = name[len(prefix):]
            if _looks_like_soname_suffix(suffix):
                return suffix
    return None

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

def _build_file_content(
        opencv_version,
        use_conda,
        has_cudawarping,
        conda_lib_dir = None,
        conda_soname_suffix = None):
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

    if use_conda:
        # For conda OpenCV, use cc_import so Bazel sets up rpaths/runfiles correctly.
        # This avoids binaries failing at runtime with `libopencv_*.so.* => not found`.
        imports = []
        deps = []
        for lib in libs:
            # e.g. libopencv_imgproc.so -> opencv_imgproc
            rule_name = lib
            if rule_name.startswith("lib"):
                rule_name = rule_name[len("lib"):]
            if rule_name.endswith(".so"):
                rule_name = rule_name[:-len(".so")]

            lib_file = lib
            if conda_soname_suffix:
                lib_file = lib + "." + conda_soname_suffix
            imports.append("""\
cc_import(
    name = "{name}",
    shared_library = "lib/{lib_file}",
    visibility = ["//visibility:private"],
)
""".format(name = rule_name, lib_file = lib_file))
            deps.append(":" + rule_name)

        return """\
{imports}
cc_library(
    name = "opencv",
    hdrs = glob([
        "{v}/opencv2/*.h*",
        "{v}/opencv2/**/*.h*",
    ]),
    includes = [
        "{v}",
    ],
    deps = {deps},
    visibility = ["//visibility:public"],
)
""".format(imports = "\n".join(imports), v = opencv_version, deps = repr(deps))

    linkopts = ["-l:" + lib for lib in libs]

    # System OpenCV: rely on the system linker search path.
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
            conda_soname_suffix = _detect_opencv_soname_suffix(ctx, chosen["root"]) if use_conda else None,
        ),
    )

opencv_configure = repository_rule(
    implementation = _opencv_configure_impl,
    environ = ["CONDA_PREFIX"],
    doc = "Autoconfigures OpenCV headers/libs, preferring CONDA_PREFIX OpenCV5 when available.",
)
