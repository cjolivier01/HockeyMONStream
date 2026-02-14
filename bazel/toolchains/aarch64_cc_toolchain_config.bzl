"""A minimal aarch64 GCC cross-toolchain config."""

load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "artifact_name_pattern", "tool_path")

def _impl(ctx):
    tool_paths = [
        tool_path(name = "ar", path = "/usr/bin/aarch64-linux-gnu-ar"),
        tool_path(name = "as", path = "/usr/bin/aarch64-linux-gnu-as"),
        tool_path(name = "cpp", path = "/usr/bin/aarch64-linux-gnu-cpp"),
        tool_path(name = "dwp", path = "/usr/bin/aarch64-linux-gnu-dwp"),
        tool_path(name = "gcc", path = "/usr/bin/aarch64-linux-gnu-gcc"),
        tool_path(name = "g++", path = "/usr/bin/aarch64-linux-gnu-g++"),
        tool_path(name = "gcov", path = "/usr/bin/aarch64-linux-gnu-gcov"),
        tool_path(name = "ld", path = "/usr/bin/aarch64-linux-gnu-ld"),
        tool_path(name = "nm", path = "/usr/bin/aarch64-linux-gnu-nm"),
        tool_path(name = "objcopy", path = "/usr/bin/aarch64-linux-gnu-objcopy"),
        tool_path(name = "objdump", path = "/usr/bin/aarch64-linux-gnu-objdump"),
        tool_path(name = "strip", path = "/usr/bin/aarch64-linux-gnu-strip"),
    ]

    return [
        cc_common.create_cc_toolchain_config_info(
            ctx = ctx,
            toolchain_identifier = "aarch64-linux-gnu-gcc",
            host_system_name = "local",
            target_system_name = "aarch64-unknown-linux-gnu",
            target_cpu = "aarch64",
            target_libc = "glibc",
            compiler = "gcc",
            abi_version = "gcc",
            abi_libc_version = "glibc",
            tool_paths = tool_paths,
            cxx_builtin_include_directories = [
                "/usr/lib/gcc-cross/aarch64-linux-gnu/13/include",
                "/usr/aarch64-linux-gnu/include",
                "/usr/include/aarch64-linux-gnu",
                "/usr/include",
            ],
            artifact_name_patterns = [
                artifact_name_pattern(category_name = "executable", prefix = "", extension = ""),
                artifact_name_pattern(category_name = "static_library", prefix = "lib", extension = ".a"),
                artifact_name_pattern(category_name = "alwayslink_static_library", prefix = "lib", extension = ".lo"),
                artifact_name_pattern(category_name = "dynamic_library", prefix = "lib", extension = ".so"),
                artifact_name_pattern(category_name = "interface_library", prefix = "lib", extension = ".ifso"),
                artifact_name_pattern(category_name = "object_file", prefix = "", extension = ".o"),
                artifact_name_pattern(category_name = "pic_object_file", prefix = "", extension = ".pic.o"),
            ],
        ),
    ]

aarch64_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
