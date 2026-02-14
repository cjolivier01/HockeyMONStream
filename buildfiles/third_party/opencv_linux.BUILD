# Bazel is only available for amd64 and arm64.
# load("//:buildfiles/third_party/opencv.bzl", "get_opencv_version")

config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

OPENCV_PREFIX = "/home/colivier/miniforge3/envs/ubuntu"
OPENCV_VERSION = "opencv5"

cc_library(
    name = "opencv",
    hdrs = glob([
        OPENCV_VERSION + "/opencv2/*.h*",
        OPENCV_VERSION + "/opencv2/**/*.h*",
    ]) + select({
        ":aarch64-linux-gnu": [
            # "aarch64-linux-gnu/opencv5/opencv2/cvconfig.h"
        ] + glob([
            # OPENCV_VERSION + "/opencv2/*.h*",
            # OPENCV_VERSION + "/opencv2/**/*.h*",
        ]),
        ":x86_64-linux-gnu": [
            # "x86_64-linux-gnu/opencv5/opencv2/cvconfig.h",
        ] + glob([
            # OPENCV_VERSION + "/opencv2/*.h*",
            # OPENCV_VERSION + "/opencv2/**/*.h*",
        ]),
        "//conditions:default": [],
    }),
    includes = [
        OPENCV_VERSION,
        "aarch64-linux-gnu/" + OPENCV_VERSION,
        "aarch64-linux-gnu/" + OPENCV_VERSION + "/opencv2",
    ] + select({
        ":aarch64-linux-gnu": [
            # OPENCV_VERSION,
            # "aarch64-linux-gnu/" + OPENCV_VERSION,
            # "aarch64-linux-gnu/" + OPENCV_VERSION + "/opencv2",
        ],
        ":x86_64-linux-gnu": [
            # OPENCV_VERSION,
            # "x86_64-linux-gnu/" + OPENCV_VERSION,
            # "x86_64-linux-gnu/" + OPENCV_VERSION + "/opencv2",
        ],
        "//conditions:default": [],
    }),
    linkopts = [
        "-Wl,-rpath," + OPENCV_PREFIX + "/lib",
        OPENCV_PREFIX + "/lib/libopencv_core.so",
        OPENCV_PREFIX + "/lib/libopencv_highgui.so",
        OPENCV_PREFIX + "/lib/libopencv_imgcodecs.so",
        OPENCV_PREFIX + "/lib/libopencv_imgproc.so",
        OPENCV_PREFIX + "/lib/libopencv_video.so",
        OPENCV_PREFIX + "/lib/libopencv_videoio.so",
        OPENCV_PREFIX + "/lib/libopencv_cudawarping.so",
        # Add as neeeded
        # OPENCV_PREFIX + "/lib/libopencv_stitching.so", OPENCV_PREFIX + "/lib/libopencv_alphamat.so", ...
    ],
    visibility = ["//visibility:public"],
)
