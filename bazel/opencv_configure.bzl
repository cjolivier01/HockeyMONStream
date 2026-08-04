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

def _has_shared_lib(ctx, dirs, lib_stem):
    # Accept unversioned (.so) and versioned (.so.*) names.
    prefix = "lib" + lib_stem + ".so"
    for d in dirs:
        if not _path_exists(ctx, d):
            continue
        for entry in ctx.path(d).readdir():
            name = _basename(entry)
            if name == prefix or name.startswith(prefix + "."):
                return True
    return False

def _pick_opencv_lib_dir(ctx, root):
    # NVIDIA's JetPack OpenCV package installs its matching libraries directly
    # in /usr/lib while Ubuntu's older multiarch OpenCV can remain installed in
    # /usr/lib/aarch64-linux-gnu. Prefer the prefix's direct lib directory so
    # headers and libraries come from the same package; normal distro installs
    # fall through to their multiarch directory.
    for rel in ["lib", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        lib_dir = root + "/" + rel
        if _has_shared_lib(ctx, [lib_dir], "opencv_core"):
            return rel
    return None

def _detect_opencv_soname_suffix(ctx, lib_dir):
    if not _path_exists(ctx, lib_dir):
        return None

    # Conda packages typically provide:
    # - libopencv_core.so      -> libopencv_core.so.<ABI>
    # - libopencv_core.so.<ABI> -> libopencv_core.so.<MAJOR>.<MINOR>.<PATCH>
    #
    # We want the `<ABI>` file name because the binary's DT_NEEDED entry uses it.
    prefix = "libopencv_core.so."
    best = None
    for entry in ctx.path(lib_dir).readdir():
        name = _basename(entry)
        if name.startswith(prefix):
            suffix = name[len(prefix):]
            if _looks_like_soname_suffix(suffix):
                if best == None:
                    best = suffix
                elif len(suffix) > len(best) or (len(suffix) == len(best) and suffix > best):
                    best = suffix
    return best

def _detect_opencv(ctx, root):
    # Prefer newer OpenCV first, if present.
    for version in ["opencv5", "opencv4"]:
        include_dir = root + "/include/" + version
        core_hpp = include_dir + "/opencv2/core.hpp"
        if _path_exists(ctx, core_hpp):
            lib_dir_rel = _pick_opencv_lib_dir(ctx, root)
            if lib_dir_rel == None:
                return None
            return {
                "root": root,
                "version": version,
                "include_dir": include_dir,
                "lib_dir_rel": lib_dir_rel,
                "has_cudawarping": _path_exists(ctx, include_dir + "/opencv2/cudawarping.hpp"),
                "has_cudawarping_lib": _has_shared_lib(
                    ctx,
                    [root + "/" + lib_dir_rel],
                    "opencv_cudawarping",
                ),
            }
    return None

def _build_file_content(
        opencv_version,
        use_conda,
        has_cudawarping,
        lib_dir = "lib",
        soname_suffix = None):
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

    # Use cc_import for both conda and system OpenCV so the final link uses the
    # exact library directory detected above. Raw `-Llib -l:...` linkopts are
    # interpreted relative to the consuming workspace/sandbox and can pick up a
    # different OpenCV from /usr/local/lib before the configured one.
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
        if soname_suffix:
            lib_file = lib + "." + soname_suffix
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

def _opencv_configure_impl(ctx):
    conda_prefix = ctx.os.environ.get("CONDA_PREFIX")
    opencv_root = ctx.os.environ.get("OPENCV_ROOT")

    # Probe candidates in priority order.
    candidates = []
    if opencv_root:
        candidates.append(("override", opencv_root))
    if conda_prefix:
        candidates.append(("conda", conda_prefix))
    # JetPack hosts commonly install their CUDA-enabled OpenCV build under
    # /usr/local while retaining Ubuntu's older runtime libraries under /usr.
    # Keep headers and shared libraries from one prefix instead of mixing the
    # two installations.
    candidates.append(("system-local", "/usr/local"))
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
        fail("OpenCV headers not found under OPENCV_ROOT, CONDA_PREFIX, or /usr (expected include/opencv4|opencv5/opencv2/core.hpp)")

    opencv_version = chosen["version"]
    use_conda = (chosen_kind == "conda")

    # Only advertise cudawarping if both the header and a shared library exist.
    has_cudawarping = chosen["has_cudawarping"] and chosen["has_cudawarping_lib"]

    # Keep the repository small: only expose the OpenCV include tree and lib dir.
    ctx.symlink(chosen["root"] + "/include/" + opencv_version, opencv_version)

    if chosen["lib_dir_rel"] and _path_exists(ctx, chosen["root"] + "/" + chosen["lib_dir_rel"]):
        ctx.symlink(chosen["root"] + "/" + chosen["lib_dir_rel"], "lib")

    # Write the repository BUILD/WORKSPACE files.
    ctx.file("WORKSPACE", "workspace(name = \"{name}\")\n".format(name = ctx.name))
    ctx.file(
        "BUILD.bazel",
        _build_file_content(
            opencv_version,
            use_conda,
            has_cudawarping,
            lib_dir = "lib",
            # System installs can carry multiple OpenCV ABIs side-by-side.
            # Prefer the unversioned linker symlinks there so headers and libs stay aligned.
            soname_suffix = _detect_opencv_soname_suffix(ctx, chosen["root"] + "/" + chosen["lib_dir_rel"]) if use_conda and chosen["lib_dir_rel"] else None,
        ),
    )

opencv_configure = repository_rule(
    implementation = _opencv_configure_impl,
    environ = ["CONDA_PREFIX", "OPENCV_ROOT"],
    doc = "Autoconfigures OpenCV headers/libs, preferring CONDA_PREFIX OpenCV5 when available.",
)
