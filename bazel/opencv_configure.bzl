def _path_exists(ctx, p):
    return ctx.path(p).exists

def _opencv_header_version_prefix(ctx, include_dir):
    major = None
    minor = None
    for line in ctx.read(include_dir + "/opencv2/core/version.hpp").split("\n"):
        if line.startswith("#define CV_VERSION_MAJOR"):
            major = line[len("#define CV_VERSION_MAJOR"):].strip()
        elif line.startswith("#define CV_VERSION_MINOR"):
            minor = line[len("#define CV_VERSION_MINOR"):].strip()
    return major + "." + minor + "." if major and minor else None

def _has_required_shared_libs(ctx, lib_dir, version_prefix):
    for lib_stem in [
        "opencv_core",
        "opencv_calib3d",
        "opencv_features2d",
        "opencv_imgcodecs",
        "opencv_imgproc",
        "opencv_video",
        "opencv_videoio",
    ]:
        filename = "lib" + lib_stem + ".so"
        library = ctx.path(lib_dir + "/" + filename)
        if not library.exists or not library.realpath.basename.startswith(filename + "." + version_prefix):
            return False
    return True

def _pick_opencv_lib_dir(ctx, root, version_prefix):
    # NVIDIA's JetPack OpenCV package installs its matching libraries directly
    # in /usr/lib while Ubuntu's older multiarch OpenCV can remain installed in
    # /usr/lib/aarch64-linux-gnu. Prefer the prefix's direct lib directory so
    # headers and libraries come from the same package; normal distro installs
    # fall through to their multiarch directory.
    for rel in ["lib", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        lib_dir = root + "/" + rel
        if _has_required_shared_libs(ctx, lib_dir, version_prefix):
            return rel
    return None

def _detect_opencv(ctx, root):
    # Prefer newer OpenCV first, if present.
    for version in ["opencv5", "opencv4"]:
        include_dir = root + "/include/" + version
        core_hpp = include_dir + "/opencv2/core.hpp"
        if _path_exists(ctx, core_hpp):
            version_prefix = _opencv_header_version_prefix(ctx, include_dir)
            lib_dir_rel = _pick_opencv_lib_dir(ctx, root, version_prefix) if version_prefix else None
            if lib_dir_rel == None:
                continue
            return {
                "root": root,
                "version": version,
                "include_dir": include_dir,
                "lib_dir_rel": lib_dir_rel,
            }
    return None

def _build_file_content(
        opencv_version):
    # Deliberately no libopencv_highgui: it pulls in GTK/Qt5, and Qt5 loaded
    # alongside our own Qt6 makes Qt's weak inline symbols collide at dynamic
    # link time. Nothing here needs a desktop window from OpenCV.
    libs = [
        "libopencv_core.so",
        "libopencv_calib3d.so",
        "libopencv_features2d.so",
        "libopencv_imgcodecs.so",
        "libopencv_imgproc.so",
        "libopencv_video.so",
        "libopencv_videoio.so",
    ]

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

        imports.append("""\
cc_import(
    name = "{name}",
    shared_library = "lib/{lib}",
    visibility = ["//visibility:private"],
)
""".format(name = rule_name, lib = lib))
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
        candidates.append(opencv_root)
    if conda_prefix:
        candidates.append(conda_prefix)
    # JetPack hosts commonly install their CUDA-enabled OpenCV build under
    # /usr/local while retaining Ubuntu's older runtime libraries under /usr.
    # Keep headers and shared libraries from one prefix instead of mixing the
    # two installations.
    candidates.append("/usr/local")
    candidates.append("/usr")

    chosen = None
    for root in candidates:
        det = _detect_opencv(ctx, root)
        if det:
            chosen = det
            break

    if not chosen:
        fail("OpenCV headers not found under OPENCV_ROOT, CONDA_PREFIX, or /usr (expected include/opencv4|opencv5/opencv2/core.hpp)")

    opencv_version = chosen["version"]

    # Keep the repository small: only expose the OpenCV include tree and lib dir.
    ctx.symlink(chosen["root"] + "/include/" + opencv_version, opencv_version)

    if chosen["lib_dir_rel"] and _path_exists(ctx, chosen["root"] + "/" + chosen["lib_dir_rel"]):
        ctx.symlink(chosen["root"] + "/" + chosen["lib_dir_rel"], "lib")

    # Write the repository BUILD/WORKSPACE files.
    ctx.file("WORKSPACE", "workspace(name = \"{name}\")\n".format(name = ctx.name))
    ctx.file(
        "BUILD.bazel",
        _build_file_content(opencv_version),
    )

opencv_configure = repository_rule(
    implementation = _opencv_configure_impl,
    environ = ["CONDA_PREFIX", "OPENCV_ROOT"],
    doc = "Autoconfigures OpenCV headers/libs, preferring CONDA_PREFIX OpenCV5 when available.",
)
