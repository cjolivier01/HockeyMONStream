"""Match signaling's libsoup ABI to the target WebRTC/libnice dependency chain."""

def _library(ctx, directories, name):
    for directory in directories:
        candidate = ctx.path(directory + "/" + name)
        if candidate.exists:
            return candidate
    return None

def _webrtc_soup(ctx, directories):
    # libnice brings in libsoup through UPnP on current distributions. Inspect
    # DT_NEEDED rather than executing target libraries (also works for Jetson).
    pending = ["libnice.so.10"]
    seen = {}
    majors = {}
    for _ in range(4):
        following = []
        for name in pending:
            if name in seen:
                continue
            seen[name] = True
            library = _library(ctx, directories, name)
            if library == None:
                continue
            result = ctx.execute(["readelf", "-d", str(library)])
            if result.return_code:
                fail("Cannot inspect WebRTC dependency {}: {}".format(library, result.stderr))
            for line in result.stdout.splitlines():
                if "(NEEDED)" not in line or "[" not in line:
                    continue
                dependency = line.split("[", 1)[1].split("]", 1)[0]
                if dependency.startswith("libsoup-3.0.so"):
                    majors["3"] = True
                elif dependency.startswith("libsoup-2.4.so"):
                    majors["2"] = True
                elif dependency.startswith("libgupnp") or dependency.startswith("libgssdp"):
                    following.append(dependency)
        pending = following
    if len(majors) > 1:
        fail("The target libnice/UPnP stack mixes libsoup 2 and 3; install a coherent WebRTC runtime.")
    return list(majors.keys())[0] if majors else None

def _libsoup_impl(ctx):
    cross = ctx.os.environ.get("HM_BAZEL_PREFER_FIRST_LOCAL_PATH") == "1"
    root = "/opt/jetson-sysroot/usr" if cross else "/usr"
    arm = cross or ctx.os.arch in ["aarch64", "arm64"]
    triplet = "aarch64-linux-gnu" if arm else "x86_64-linux-gnu"
    directories = [root + "/lib/" + triplet, root + "/lib", root + "/lib64"]
    major = _webrtc_soup(ctx, directories)
    if major == None:
        # Non-WebRTC installations may lack libnice. Prefer the modern SDK when
        # present; native startup already reports a missing ICE plugin clearly.
        major = "3" if ctx.path(root + "/include/libsoup-3.0/libsoup/soup.h").exists else "2"
    version = "3.0" if major == "3" else "2.4"
    soname = "libsoup-" + version + (".so.0" if major == "3" else ".so.1")
    include = ctx.os.environ.get("HSTREAM_LIBSOUP_INCLUDE_DIR", root + "/include/libsoup-" + version)
    header = ctx.path(include + "/libsoup/soup-version.h")
    if not header.exists:
        fail(("WebRTC requires libsoup {}, but matching headers are missing at {}. " +
              "Install {} in the target system or set HSTREAM_LIBSOUP_INCLUDE_DIR to matching extracted headers.").format(
            major,
            include,
            "libsoup-3.0-dev" if major == "3" else "libsoup2.4-dev",
        ))
    declared = None
    for line in ctx.read(header).splitlines():
        fields = [field for field in line.replace("\t", " ").split(" ") if field]
        if len(fields) >= 3 and fields[:2] == ["#define", "SOUP_MAJOR_VERSION"]:
            declared = fields[2].replace("(", "").replace(")", "")
    if declared != major:
        fail("libsoup headers at {} are major {}, but the target WebRTC runtime requires {}.".format(include, declared, major))
    if _library(ctx, directories, soname) == None:
        fail("Missing target libsoup runtime {} under {}.".format(soname, root))
    ctx.symlink(ctx.path(include), "include")
    # The pinned jetson-utils WebRTC helper also consumes json.hpp through this
    # longstanding dependency; keep that include independent of the soup SDK.
    ctx.symlink(ctx.path(root + "/include/nlohmann"), "nlohmann")
    ctx.file("version.bzl", "SOUP_SONAME = {}\n".format(repr(soname)))
    ctx.symlink(ctx.path(ctx.attr.build_file), "BUILD.bazel")

libsoup_repository = repository_rule(
    implementation = _libsoup_impl,
    attrs = {
        "build_file": attr.label(default = "//buildfiles:third_party/libsoup.BUILD", allow_single_file = True),
    },
    environ = ["HM_BAZEL_PREFER_FIRST_LOCAL_PATH", "HSTREAM_LIBSOUP_INCLUDE_DIR"],
    local = True,
)
