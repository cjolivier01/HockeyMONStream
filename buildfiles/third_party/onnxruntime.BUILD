load("@kstream//bazel:onnxruntime_import.bzl", "onnxruntime_import")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

onnxruntime_import(
    name = "native_runtime",
    library = "lib/libonnxruntime.so.1",
    providers = ["lib/libonnxruntime_providers_cuda.so", "lib/libonnxruntime_providers_shared.so"],
)

cc_library(
    name = "onnxruntime",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    deps = [":native_runtime"],
)

filegroup(
    name = "cuda_provider_files",
    srcs = ["lib/libonnxruntime_providers_cuda.so", "lib/libonnxruntime_providers_shared.so"],
)

filegroup(
    name = "runtime_files",
    srcs = [
        "LICENSE",
        "ThirdPartyNotices.txt",
        ":cuda_provider_files",
    ] + glob(["lib/libonnxruntime.so.1.*"]),
)
