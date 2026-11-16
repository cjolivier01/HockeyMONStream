load(":cuda_dependencies.bzl", "cuda_dependencies")

cc_test(
    name = "cuda_dependencies_test",
    size = "small",
    srcs = ["CudaDependenciesTest.cpp"],
    deps = cuda_dependencies(
        "cuda_dependencies_test",
        [
            "//src/libs/gpu:runtime",
            "@hm-cupano//src/cuda:cuda_types",
            "@jetson-utils//cuda:cuda_lib_headers",
        ],
        testonly = True,
    ),
)

py_test(
    name = "tensorrt_sdk_repository_test",
    size = "small",
    srcs = ["player_tensorrt_sdk_repository_test.py"],
    data = [
        "player_tensorrt_sdk_repository.bzl",
        "tensorrt_sdk_common.bzl",
        "tensorrt_sdk_repository.bzl",
    ],
    main = "player_tensorrt_sdk_repository_test.py",
)
