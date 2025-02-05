_workspace_name = "kstream"

workspace(name = _workspace_name)

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

local_repository(
    name = "com_extension_dev",
    path = "external/deepstream/graph-composer/extension-dev",
)

local_repository(
    name = "deepstream",
    path = "external/deepstream",
)

local_repository(
  name = "jetson-utils",
  path = "external/jetson-utils",
)

local_repository(
    name = "gst_plugin_dev",
    path = ".",
)

local_repository(
    name = "rules_compdb",
    path = "/home/colivier/src/hstream/bazel",
)

local_repository(
    name = "hm",
    path = "external/hm",
)

load(
    "@com_extension_dev//build:graph_extension.bzl",
    "graph_nvidia_extension",
    "load_extension_dev_workspace",
)

load_extension_dev_workspace()

git_repository(
    name = "rules_cuda",
    # v0.2.3 breaks some lubcupti for our version of bazel
    commit = "3f2429254ec956220557e79ea9d5f5e8871c2907",
    remote = "https://github.com/bazel-contrib/rules_cuda",
)

load("@rules_cuda//cuda:repositories.bzl", "register_detected_cuda_toolchains", "rules_cuda_dependencies")

rules_cuda_dependencies()

register_detected_cuda_toolchains()

graph_nvidia_extension(
    name = "NvDsInterfaceExt",
    version = "1.6.0",
)

graph_nvidia_extension(
    name = "NvDsBaseExt",
    version = "1.6.0",
)

graph_nvidia_extension(
    name = "StandardExtension",
    version = "2.6.0",
)

new_local_repository(
    name = "misc",
    build_file = "//buildfiles:third_party/misc.BUILD",
    path = "/usr/include",
)

new_local_repository(
    name = "glibconfig_x86",
    build_file = "//buildfiles:third_party/glibconfig.BUILD",
    path = "/usr/lib/x86_64-linux-gnu/glib-2.0/include",
)

new_local_repository(
    name = "glibconfig_aarch64",
    build_file = "//buildfiles:third_party/glibconfig.BUILD",
    path = "/usr/lib/aarch64-linux-gnu/glib-2.0/include",
)

new_git_repository(
    name = "yaml-cpp",
    build_file = "@//buildfiles:third_party/yaml-cpp.BUILD",
    tag="0.8.0",
    remote = "https://github.com/jbeder/yaml-cpp.git",
)

# git_repository(
#     name = "rules_python",
#     remote = "https://github.com/bazelbuild/rules_python.git",
#     tag = "0.1.0",  # Use the tag corresponding to version 0.1.0
# )

http_archive(
    name = "rules_python",
    sha256 = "9c6e26911a79fbf510a8f06d8eedb40f412023cf7fa6d1461def27116bff022c",
    strip_prefix = "rules_python-1.1.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/1.1.0/rules_python-1.1.0.tar.gz",
)

load("@rules_python//python:repositories.bzl", "py_repositories")

py_repositories()

git_repository(
    name = "magic_enum",
    remote = "https://github.com/Neargye/magic_enum.git",
    strip_prefix = "",  # No need for strip_prefix since Git directly clones the repository
    tag = "v0.9.3",  # Use the tag corresponding to version 0.9.3
)

new_local_repository(
    name = "glib",
    build_file = "//buildfiles:third_party/glib_nobuild.BUILD",
    # path = "/usr/local",
    path = "/usr",
)

new_local_repository(
    name = "opencv_linux",
    build_file = "@//buildfiles:third_party/opencv_linux.BUILD",
    path = "/usr/include",
    # path = "/usr/local/include",
)

new_local_repository(
    name = "json_glib",
    build_file = "@//buildfiles:third_party/json_glib.BUILD",
    path = "/usr",
)

new_local_repository(
    name = "gstreamer",
    build_file = "@//buildfiles:third_party/gstreamer_nobuild.BUILD",
    #path = "/usr/local",
    path = "/usr",
)

new_local_repository(
    name = "libsoup",
    build_file = "@//buildfiles:third_party/libsoup.BUILD",
    path = "/usr",
)

