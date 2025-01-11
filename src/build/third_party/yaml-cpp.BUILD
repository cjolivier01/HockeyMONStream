# Bazel is only available for amd64 and arm64.

# config_setting(
#     name = "aarch64-linux-gnu",
#     constraint_values = ["@platforms//cpu:x86_64"],
# )

# config_setting(
#     name = "x86_64-linux-gnu",
#     constraint_values = ["@platforms//cpu:aarch64"],
# )

# cc_library(
#     name = "yaml-cpp",
#     hdrs = glob([
#         "include/yaml-cpp/**/*.h",
#     ]),
#     includes = [
#         "include/yaml-cpp",
#     ],
#     linkopts =
#         select({
#             ":aarch64-linux-gnu": ["-Llib/aarch64-linux-gnu"],
#             ":x86_64-linux-gnu": ["-Llib/x86_64-linux-gnu"],
#             "//conditions:default": [],
#         }) + [
#             "-l:libyaml-cpp.so",
#         ],
#     visibility = ["//visibility:public"],
# )
