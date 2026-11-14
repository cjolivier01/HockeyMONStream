"""TensorRT SDK discovery shared by the two TensorRT repository rules.

Both builders use the installed DeepStream runtime, matching development headers
and the same narrowly verified managed-header fallback. Explicit SDK overrides
and Jetson sysroots remain authoritative.
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

# Development headers only: the already installed DeepStream runtime supplies
# the libraries. Never install packages or change /usr from a repository rule.
_TRT_10_16_HEADERS = [
    ("libnvinfer-headers-dev_10.16.1.11-1+cuda13.2_amd64.deb", "8447e3fc65f46bf28907c7d03c39f3fa86ef2bf6ec46afb848ea6533fb7fcd65"),
    ("libnvonnxparsers-dev_10.16.1.11-1+cuda13.2_amd64.deb", "9904cbbc32d4b0f8eb665ffd2758ee10b74a3a3afb19c8e164b2c4a83a98bcd1"),
]
_NVIDIA_PACKAGES = "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/"

def _libraries(ctx, root, major, machine, override_name):
    for suffix in ["lib", "lib64", "lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + suffix)
        libraries = {}
        for name in ["nvinfer", "nvonnxparser"]:
            # /usr's unversioned development link may name a newer SDK than
            # DeepStream. Prefer its required runtime SONAME when available.
            versioned = candidate.get_child("lib" + name + ".so." + major)
            libraries[name] = versioned if versioned.exists else candidate.get_child("lib" + name + ".so")
        coherent = all([library.exists for library in libraries.values()])
        if coherent:
            for name, library in libraries.items():
                expected = "[lib" + name + ".so." + major + "]"
                soname = [line for line in readelf(ctx, library, "-d").splitlines() if "(SONAME)" in line]
                coherent = coherent and bool(soname) and expected in soname[0]
                coherent = coherent and (not machine or elf_machine(ctx, library) == machine)
        if coherent:
            return libraries
    fail("No complete TensorRT {} runtime at {}. Set {} to a compatible SDK.".format(major, root, override_name))

def _library_release(library):
    version = library.realpath.basename.split(".so.")[-1].split(".")
    if len(version) == 3 and all([field.isdigit() for field in version]):
        return version
    fail("Cannot identify TensorRT library release; use the SDK's versioned libraries: {}".format(library))

def _runtime_version(ctx, library, cross):
    if cross:
        # Do not execute target code. The actual target performs the full
        # major/minor/patch/build check in QueryRuntimeIdentity at runtime.
        return _library_release(library)
    python = ctx.which("python3")
    if not python:
        fail("python3 is required to query the native TensorRT runtime version.")
    # Version functions do not create a CUDA context or load an engine. No
    # TensorRT Python package is imported; -S excludes site customization.
    result = ctx.execute([
        python,
        "-S",
        "-c",
        "import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); print(lib.getInferLibVersion(), lib.getInferLibBuildVersion())",
        library,
    ])
    fields = result.stdout.strip().split(" ")
    if result.return_code or len(fields) != 2 or not all([field.isdigit() for field in fields]):
        fail("Cannot query the native TensorRT runtime version: {} {}".format(library, result.stderr))
    version = int(fields[0])
    return [str(version // 10000), str((version % 10000) // 100), str(version % 100), fields[1]]

def _managed_headers(ctx, libraries, ds_machine, version, cross, override_name):
    # This fallback is deliberately narrow. Other SDK versions/architectures
    # require their own verified headers; never use host headers for a sysroot.
    host = ctx.execute(["uname", "-m"])
    compatible = not cross and ctx.os.name == "linux" and host.return_code == 0 and host.stdout.strip() == "x86_64"
    compatible = compatible and ds_machine == "Advanced Micro Devices X86-64" and version == ["10", "16", "1", "11"]
    for name in ["nvinfer", "nvonnxparser"]:
        compatible = compatible and libraries[name].realpath.basename == "lib" + name + ".so.10.16.1"
    if not compatible:
        fail("Installed headers do not match DeepStream's TensorRT runtime. Install matching development headers or set {}. Automatic headers are available only for native Linux x86_64 TensorRT 10.16.1.".format(override_name))
    dpkg = ctx.which("dpkg-deb")
    if not dpkg:
        fail("dpkg-deb is required to extract managed TensorRT headers; alternatively set {}.".format(override_name))
    for filename, digest in _TRT_10_16_HEADERS:
        archive = "downloads/" + filename
        ctx.download(url = _NVIDIA_PACKAGES + filename, output = archive, sha256 = digest)
        result = ctx.execute([dpkg, "--extract", ctx.path(archive), ctx.path("development")])
        if result.return_code:
            fail("Cannot extract TensorRT development headers: {}".format(result.stderr))
    # The parser development package also contains a static archive and linker
    # symlink. Neither participates in this SDK; link only the verified runtime.
    ctx.delete("development/usr/lib")
    return include_dir(ctx, str(ctx.path("development/usr")))

def select_sdk(ctx, override_name):
    """Return matching headers and runtime libraries for a TensorRT repository.

    Args:
      ctx: the repository context.
      override_name: the environment variable selecting an explicit SDK root.

    Returns:
      The include directory and a map of nvinfer/nvonnxparser library paths.
      Only the verified native 10.16.1 runtime supports managed headers;
      explicit SDK roots and cross-compiles never fall back to them.
    """
    override = ctx.os.environ.get(override_name)
    cross = ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1"
    root = override or ("/opt/jetson-sysroot/usr" if cross else "/usr")
    ds_infer = deepstream_infer(ctx, cross)
    include = include_dir(ctx, root)
    header_version = version_from_header(ctx.read(include.get_child("NvInferVersion.h"))) if include else [""] * 4
    major = needed_major(ctx, ds_infer) if ds_infer else header_version[0]
    if not major or not major.isdigit():
        fail("Cannot determine a compatible TensorRT SDK. Set {}.".format(override_name))
    if override and header_version[0] != major:
        fail("Explicit TensorRT SDK at {} has header major {} but DeepStream requires {}. Select a coherent SDK; explicit overrides never fall back.".format(root, header_version[0] or "missing", major))
    machine = elf_machine(ctx, ds_infer) if ds_infer else None
    libraries = _libraries(ctx, root, major, machine, override_name)
    machine = machine or elf_machine(ctx, libraries["nvinfer"])
    runtime_version = _runtime_version(ctx, libraries["nvinfer"], cross)
    parser_release = _library_release(libraries["nvonnxparser"])
    if parser_release != runtime_version[:3]:
        fail("TensorRT parser release {} disagrees with runtime {}. Select a coherent SDK.".format(".".join(parser_release), ".".join(runtime_version)))
    if header_version[:len(runtime_version)] != runtime_version:
        if override:
            fail("Explicit TensorRT SDK headers {} disagree with runtime {} at {}. Explicit overrides never fall back.".format(".".join(header_version), ".".join(runtime_version), root))
        include = _managed_headers(ctx, libraries, machine, runtime_version, cross, override_name)
    if not include or version_from_header(ctx.read(include.get_child("NvInferVersion.h")))[:len(runtime_version)] != runtime_version:
        fail("No compatible TensorRT headers were found.")
    return include, libraries
