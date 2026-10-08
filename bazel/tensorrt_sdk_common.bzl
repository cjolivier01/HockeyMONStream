"""TensorRT SDK discovery shared by the two TensorRT repository rules.

Both rules have to answer the same question before they can pick an SDK: which
TensorRT major does the installed DeepStream actually load? Keeping that answer
in one place is what stops the two from drifting apart.
"""

def readelf(ctx, path, flag):
    """Run readelf over a library and return its stdout.

    Args:
      ctx: the repository context.
      path: the library to inspect.
      flag: the readelf flag to pass, for example "-d" or "-h".

    Returns:
      The readelf output, with LC_ALL=C so the fields are parseable.
    """
    tool = ctx.which("readelf")
    if not tool:
        fail("readelf is required to verify the TensorRT SDK.")
    result = ctx.execute([tool, flag, path], environment = {"LC_ALL": "C"})
    if result.return_code:
        fail("Cannot inspect TensorRT/DeepStream library {}: {}".format(path, result.stderr))
    return result.stdout

def elf_machine(ctx, path):
    """Return a library's ELF architecture, as readelf names it.

    Args:
      ctx: the repository context.
      path: the library to inspect.

    Returns:
      The architecture string, for example "Advanced Micro Devices X86-64".
    """
    for line in readelf(ctx, path, "-h").splitlines():
        if "Machine:" in line:
            return line.split("Machine:")[1].strip()
    fail("Cannot determine ELF architecture for {}".format(path))

def version_from_header(text):
    """Parse the TensorRT release out of NvInferVersion.h.

    Args:
      text: the contents of NvInferVersion.h.

    Returns:
      [major, minor, patch, build] as strings; a field absent from the header
      comes back empty rather than failing, so callers can report what is
      missing.
    """
    values = {}
    for line in text.splitlines():
        parts = [word for word in line.replace("\t", " ").split(" ") if word]
        if len(parts) >= 3 and parts[0] == "#define":
            values[parts[1]] = parts[2]
    version = []
    for field in ["MAJOR", "MINOR", "PATCH", "BUILD"]:
        value = values.get("NV_TENSORRT_" + field, "")
        version.append(values.get(value, value))
    return version

def include_dir(ctx, root):
    """Find the directory under root holding a complete set of TensorRT headers.

    Args:
      ctx: the repository context.
      root: the SDK prefix to search, for example "/usr".

    Returns:
      The include directory, or None when no candidate holds all three headers.
    """
    for suffix in ["include", "include/aarch64-linux-gnu", "include/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + suffix)
        if all([candidate.get_child(name).exists for name in ["NvInfer.h", "NvInferVersion.h", "NvOnnxParser.h"]]):
            return candidate
    return None

def needed_major(ctx, ds_infer):
    """Return the TensorRT major that DeepStream's nvinfer links against.

    This is the authority on which SDK matters. It comes from DT_NEEDED on the
    installed runtime, not from whatever /usr currently defaults to.

    Args:
      ctx: the repository context.
      ds_infer: path to DeepStream's libnvds_infer.so.

    Returns:
      The major version as a string, for example "10".
    """
    for line in readelf(ctx, ds_infer, "-d").splitlines():
        if "(NEEDED)" in line and "[libnvinfer.so." in line:
            major = line.split("[libnvinfer.so.")[1].split("]")[0]
            if major.isdigit():
                return major
    fail("Cannot determine the TensorRT runtime required by {}".format(ds_infer))

def deepstream_infer(ctx, cross):
    """Locate libnvds_infer.so in the DeepStream install these rules target.

    Args:
      ctx: the repository context.
      cross: True when building against the Jetson sysroot.

    Returns:
      The path to libnvds_infer.so, or None when DeepStream is not installed.
    """
    override = ctx.os.environ.get("DEEPSTREAM_ROOT")
    roots = [override] if override else [
        "/opt/jetson-sysroot/opt/nvidia/deepstream/deepstream",
        "/opt/nvidia/deepstream/deepstream",
    ]
    if not override and not cross:
        roots = roots[::-1]

    # Match conditional_local_repository's first existing root, including its
    # explicit override and native/sysroot preference.
    for root in roots:
        path = ctx.path(root)
        if path.exists:
            infer = path.get_child("lib/libnvds_infer.so")
            if not infer.exists:
                fail("DeepStream at {} has no lib/libnvds_infer.so".format(root))
            return infer
    if override:
        fail("DEEPSTREAM_ROOT does not exist: {}".format(override))
    return None
