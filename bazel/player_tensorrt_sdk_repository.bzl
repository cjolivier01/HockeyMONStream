"""Select a coherent TensorRT SDK matching the installed DeepStream runtime."""

# Development headers only: the already installed DeepStream runtime supplies
# the libraries. Never install packages or change /usr from a repository rule.
_TRT_10_16_HEADERS = [
    ("libnvinfer-headers-dev_10.16.1.11-1+cuda13.2_amd64.deb", "8447e3fc65f46bf28907c7d03c39f3fa86ef2bf6ec46afb848ea6533fb7fcd65"),
    ("libnvonnxparsers-dev_10.16.1.11-1+cuda13.2_amd64.deb", "9904cbbc32d4b0f8eb665ffd2758ee10b74a3a3afb19c8e164b2c4a83a98bcd1"),
]
_NVIDIA_PACKAGES = "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/"

def _version_from_header(text):
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

def _include_dir(ctx, root):
    for suffix in ["include", "include/aarch64-linux-gnu", "include/x86_64-linux-gnu"]:
        candidate = ctx.path(root + "/" + suffix)
        if all([candidate.get_child(name).exists for name in ["NvInfer.h", "NvInferVersion.h", "NvOnnxParser.h"]]):
            return candidate
    return None

def _libraries(ctx, root, major, machine):
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
                soname = [line for line in _readelf(ctx, library, "-d").splitlines() if "(SONAME)" in line]
                coherent = coherent and bool(soname) and expected in soname[0]
                coherent = coherent and (not machine or _elf_machine(ctx, library) == machine)
        if coherent:
            return libraries
    fail("No complete TensorRT {} runtime at {}. Set HSTREAM_PLAYER_TENSORRT_SDK_ROOT to a compatible SDK.".format(major, root))

def _readelf(ctx, path, flag):
    tool = ctx.which("readelf")
    if not tool:
        fail("readelf is required to verify the player TensorRT SDK.")
    result = ctx.execute([tool, flag, path], environment = {"LC_ALL": "C"})
    if result.return_code:
        fail("Cannot inspect TensorRT/DeepStream library {}: {}".format(path, result.stderr))
    return result.stdout

def _elf_machine(ctx, path):
    for line in _readelf(ctx, path, "-h").splitlines():
        if "Machine:" in line:
            return line.split("Machine:")[1].strip()
    fail("Cannot determine ELF architecture for {}".format(path))

def _needed_major(ctx, ds_infer):
    for line in _readelf(ctx, ds_infer, "-d").splitlines():
        if "(NEEDED)" in line and "[libnvinfer.so." in line:
            major = line.split("[libnvinfer.so.")[1].split("]")[0]
            if major.isdigit():
                return major
    fail("Cannot determine the TensorRT runtime required by {}".format(ds_infer))

def _deepstream_infer(ctx, cross):
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

def _runtime_version(ctx, library, cross):
    if cross:
        # Do not execute target code. The actual target performs the full
        # major/minor/patch/build check in QueryRuntimeIdentity at runtime.
        version = library.realpath.basename.split(".so.")[-1].split(".")
        if len(version) == 3 and all([field.isdigit() for field in version]):
            return version
        fail("Cannot identify TensorRT target runtime version: {}".format(library))
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

def _managed_headers(ctx, libraries, ds_machine, version, cross):
    # This fallback is deliberately narrow. Other SDK versions/architectures
    # require their own verified headers; never use host headers for a sysroot.
    host = ctx.execute(["uname", "-m"])
    compatible = not cross and ctx.os.name == "linux" and host.return_code == 0 and host.stdout.strip() == "x86_64"
    compatible = compatible and ds_machine == "Advanced Micro Devices X86-64" and version == ["10", "16", "1", "11"]
    for name in ["nvinfer", "nvonnxparser"]:
        compatible = compatible and libraries[name].realpath.basename == "lib" + name + ".so.10.16.1"
    if not compatible:
        fail("Installed headers do not match DeepStream's TensorRT runtime. Install matching development headers or set HSTREAM_PLAYER_TENSORRT_SDK_ROOT. Automatic headers are available only for native Linux x86_64 TensorRT 10.16.1.")
    dpkg = ctx.which("dpkg-deb")
    if not dpkg:
        fail("dpkg-deb is required to extract managed TensorRT headers; alternatively set HSTREAM_PLAYER_TENSORRT_SDK_ROOT.")
    for filename, digest in _TRT_10_16_HEADERS:
        archive = "downloads/" + filename
        ctx.download(url = _NVIDIA_PACKAGES + filename, output = archive, sha256 = digest)
        result = ctx.execute([dpkg, "--extract", ctx.path(archive), ctx.path("development")])
        if result.return_code:
            fail("Cannot extract TensorRT development headers: {}".format(result.stderr))
    # The parser development package also contains a static archive and linker
    # symlink. Neither participates in this SDK; link only the verified runtime.
    ctx.delete("development/usr/lib")
    return _include_dir(ctx, str(ctx.path("development/usr")))

def _player_tensorrt_sdk_impl(ctx):
    override = ctx.os.environ.get("HSTREAM_PLAYER_TENSORRT_SDK_ROOT")
    cross = ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1"
    root = override or ("/opt/jetson-sysroot/usr" if cross else "/usr")
    ds_infer = _deepstream_infer(ctx, cross)
    include = _include_dir(ctx, root)
    header_version = _version_from_header(ctx.read(include.get_child("NvInferVersion.h"))) if include else [""] * 4
    major = _needed_major(ctx, ds_infer) if ds_infer else header_version[0]
    if not major or not major.isdigit():
        fail("Cannot determine a compatible player TensorRT SDK. Set HSTREAM_PLAYER_TENSORRT_SDK_ROOT.")
    if override and header_version[0] != major:
        fail("Explicit player TensorRT SDK at {} has header major {} but DeepStream requires {}. Select a coherent SDK; explicit overrides never fall back.".format(root, header_version[0] or "missing", major))
    machine = _elf_machine(ctx, ds_infer) if ds_infer else None
    libraries = _libraries(ctx, root, major, machine)
    machine = machine or _elf_machine(ctx, libraries["nvinfer"])
    runtime_version = _runtime_version(ctx, libraries["nvinfer"], cross)
    if header_version[:len(runtime_version)] != runtime_version:
        if override:
            fail("Explicit player TensorRT SDK headers {} disagree with runtime {} at {}. Explicit overrides never fall back.".format(".".join(header_version), ".".join(runtime_version), root))
        include = _managed_headers(ctx, libraries, machine, runtime_version, cross)
    if not include or _version_from_header(ctx.read(include.get_child("NvInferVersion.h")))[:len(runtime_version)] != runtime_version:
        fail("No compatible player TensorRT headers were found.")
    ctx.symlink(include, "include")
    ctx.symlink(libraries["nvinfer"], "lib/libnvinfer.so")
    ctx.symlink(libraries["nvonnxparser"], "lib/libnvonnxparser.so")
    ctx.file("BUILD.bazel", """
cc_import(name = "infer", shared_library = "lib/libnvinfer.so")
cc_import(name = "onnx_parser", shared_library = "lib/libnvonnxparser.so")
cc_library(
    name = "runtime",
    hdrs = glob(["include/**/*.h"]),
    strip_include_prefix = "include",
    include_prefix = "hstream_player_tensorrt",
    deps = [":infer"],
    visibility = ["//visibility:public"],
)
cc_library(
    name = "sdk",
    deps = [":runtime", ":onnx_parser"],
    visibility = ["//visibility:public"],
)
""")

player_tensorrt_sdk_repository = repository_rule(
    implementation = _player_tensorrt_sdk_impl,
    environ = ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT", "HM_BAZEL_PREFER_FIRST_LOCAL_PATH", "DEEPSTREAM_ROOT"],
    local = True,
)
