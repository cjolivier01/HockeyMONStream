load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cuda//cuda:defs.bzl", "cuda_library")

cc_binary(
    name = "nvdsinfer_custom_impl_Yolo",
    srcs = glob([
        "*.cpp",
        "layers/*.cpp",
    ]),
    copts = [
        "-Wall",
        "-std=c++11",
        "-fPIC",
        "-Wno-error=deprecated-declarations",
        "-DWITH_CUDA",
    ],
    linkopts = [
        "-shared",
        "-Wl,-z,defs",
        "-Wno-error=deprecated-declarations",
        "-Wl,--start-group",
        "-lnvinfer_plugin",
        "-lnvinfer",
        "-lnvonnxparser",
        "-L/usr/local/cuda-12.6/lib64",
        "-lcudart",
        "-lcublas",
        "-lstdc++fs",
        "-Wl,--end-group",
    ],
    deps=[
        ":nvdsinfer_custom_impl_Yolo_cuda_lib",
        "@deepstream_lib",
        # "@deepstream_sources//apps/apps-common:deepstream_apps_common",
        "@deepstream_sources//:deepstream_includes",
        #"@deepstream_apps_common",
        "@NvDsBaseExt",
        "@NvDsInterfaceExt",
        "@StandardExtension",
        "@glib",
        "@gstreamer",
        "@opencv_linux//:opencv",
        "@local_cuda//:cuda_runtime",
    ],
    linkshared = True,
    visibility = ["//visibility:public"],
)

cuda_library(
    name = "nvdsinfer_custom_impl_Yolo_cuda_lib",
    srcs=glob([
        "*.cu",
    ]),
    copts=[
        "-std=c++11",
        "-Xcompiler=-fPIC",
        "-Xcompiler=-g",
        "-DWITH_CUDA",
        "-g",
    ],
    deps = [
        "@deepstream_sources//:deepstream_includes",
        ":nvdsinfer_custom_impl_Yolo_headers",
        "@deepstream_lib",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "nvdsinfer_custom_impl_Yolo_headers",
    hdrs = glob([
        "*.h",
        "layers/*.h",
    ]),
    visibility = ["//visibility:public"],
)
