INCLUDE_PREFIX = "deepstream"

config_setting(
    name = "jetson",
    constraint_values = ["@platforms//cpu:aarch64"],
    values = {"define": "target_platform=jetson"},
)

config_setting(
    name = "arm64-sbsa",
    values = {"define": "target_platform=arm64"},
)

config_setting(
    name = "aarch64-linux-gnu",
    constraint_values = ["@platforms//cpu:aarch64"],
)

config_setting(
    name = "x86_64-linux-gnu",
    constraint_values = ["@platforms//cpu:x86_64"],
)

cc_library(
    name = "deepstream_lib",
    hdrs = glob([
    ]) + [
    ],
    linkopts = [
        "-L./lib",
    ] + select({
        ":jetson": ["-L/opt/jetson-sysroot/opt/nvidia/deepstream/deepstream/lib"],
        ":arm64-sbsa": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        ":aarch64-linux-gnu": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        ":x86_64-linux-gnu": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        "//conditions:default": [],
    }) + [
        "-l:libnvdsgst_meta.so",
        "-l:libnvbufsurface.so",
        "-l:libnvdsgst_inferbase.so",
        "-l:libnvdsgst_helper.so",
        "-l:libnvds_meta.so",
        "-l:libnvbufsurftransform.so",
        "-l:libnvdsgst_customhelper.so",
        "-l:libnvds_nvtxhelper.so",
        "-l:libnvdsgst_smartrecord.so",
        "-l:libnvds_utils.so",
        "-l:libnvds_logger.so",
        "-l:libnvds_msgbroker.so",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":deepstream_includes",
    ],
)

cc_library(
    name = "deepstream_includes",
    srcs = [],
    hdrs = glob([
        "sources/includes/**/*.h*",
        "sources/includes/nvdsinferserver/*.h",
    ]),
    include_prefix = "deepstream/sources/includes",
    includes = [
        "sources/includes",
        "sources/includes/nvdsinferserver",
    ],
    visibility = ["//visibility:public"],
    deps = [
    ],
)

cc_library(
    name = "nvdsinfer",
    hdrs = glob([
        "sources/libs/nvdsinfer/*.h",
    ]) + [
        # "nvdsinfer_context_impl.h",
    ],
    copts = [
    ],
    includes = [
        "sources/libs/nvdsinfer",
    ],
    linkopts = [
        "-lnvinfer_plugin",
        "-lnvinfer",
        "-lnvonnxparser",
    ],
    visibility = ["//visibility:public"],
    deps = [
        # "@deepstream//sources:deepstream_includes",
        ":deepstream_includes",
        "@gst_plugin_dev//toolchains/jetson:cuda_runtime",
    ],
)

# DeepStream's new nvstreammux uses a wall-clock partial-batch timeout. That is
# appropriate for live analytics but can silently split an exact stereo pair
# after a file/chapter transition. Build a private, statically registered mux
# from the SDK source with only that recovery policy disabled. The generated
# copies keep the SDK installation and Jetson sysroot immutable.
genrule(
    name = "hstream_lossless_nvstreammux_backend_sources",
    srcs = [
        "sources/libs/nvstreammux/MuxConfigParser.cpp",
        "sources/libs/nvstreammux/nvstreammux.cpp",
        "sources/libs/nvstreammux/nvstreammux_batch.cpp",
        "sources/libs/nvstreammux/nvstreammux_pads.cpp",
        "@gst_plugin_dev//src/apps/apps-common:nvstreammux_lossless.patch",
    ],
    outs = [
        "hstream_lossless_nvstreammux_backend/MuxConfigParser.cpp",
        "hstream_lossless_nvstreammux_backend/nvstreammux.cpp",
        "hstream_lossless_nvstreammux_backend/nvstreammux_batch.cpp",
        "hstream_lossless_nvstreammux_backend/nvstreammux_pads.cpp",
    ],
    cmd = " && ".join([
        "cp $(location sources/libs/nvstreammux/MuxConfigParser.cpp) $(RULEDIR)/hstream_lossless_nvstreammux_backend/MuxConfigParser.cpp",
        "cp $(location sources/libs/nvstreammux/nvstreammux.cpp) $(RULEDIR)/hstream_lossless_nvstreammux_backend/nvstreammux.cpp",
        "cp $(location sources/libs/nvstreammux/nvstreammux_batch.cpp) $(RULEDIR)/hstream_lossless_nvstreammux_backend/nvstreammux_batch.cpp",
        "cp $(location sources/libs/nvstreammux/nvstreammux_pads.cpp) $(RULEDIR)/hstream_lossless_nvstreammux_backend/nvstreammux_pads.cpp",
        "chmod u+w $(RULEDIR)/hstream_lossless_nvstreammux_backend/*.cpp",
        "patch -p1 -d $(RULEDIR)/hstream_lossless_nvstreammux_backend -i $$(pwd)/$(location @gst_plugin_dev//src/apps/apps-common:nvstreammux_lossless.patch)",
    ]),
)

cc_library(
    name = "hstream_lossless_nvstreammux_backend",
    srcs = [
        ":hstream_lossless_nvstreammux_backend_sources",
    ],
    hdrs = glob([
        "sources/libs/nvstreammux/include/*.h",
    ]),
    copts = [
        "-fPIC",
        "-std=c++17",
        "-Wno-deprecated-declarations",
    ],
    includes = [
        "sources/libs/nvstreammux",
        "sources/libs/nvstreammux/include",
    ],
    linkopts = ["-luuid"],
    linkstatic = True,
    deps = [
        ":deepstream_includes",
        "@glib",
        "@json_glib",
        "@yaml-cpp",
    ],
)

cc_library(
    name = "hstream_lossless_nvstreammux_impl",
    srcs = [
        "@gst_plugin_dev//src/apps/apps-common:HStreamLosslessMux.cpp",
        "sources/gst-plugins/gst-nvmultistream2/GstNvStreamMuxCtx.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvbufaudio.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvstreammux.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvstreammux_audio.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvstreammux_ntp.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvstreammux_pads.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvstreampad.cpp",
        "sources/gst-plugins/gst-nvmultistream2/gstnvtimesynch.cpp",
    ],
    hdrs = glob([
        "sources/gst-plugins/gst-nvmultistream2/*.h",
    ]),
    copts = [
        "-fPIC",
        "-std=c++17",
        "-Wno-deprecated-declarations",
        "-Wno-error=deprecated-declarations",
        "-DGstNvStreamMux=GstHStreamLosslessMux",
        "-DGstNvStreamMuxClass=GstHStreamLosslessMuxClass",
        "-D_GstNvStreamMux=_GstHStreamLosslessMux",
        "-D_GstNvStreamMuxClass=_GstHStreamLosslessMuxClass",
        "-Dgst_nvstreammux_2_get_type=gst_hstream_lossless_mux_get_type",
        "-DGstNvStreamPad=GstHStreamLosslessPad",
        "-DGstNvStreamPadClass=GstHStreamLosslessPadClass",
        "-D_GstNvStreamPad=_GstHStreamLosslessPad",
        "-D_GstNvStreamPadClass=_GstHStreamLosslessPadClass",
        "-Dgst_nvstream_pad_get_type=gst_hstream_lossless_pad_get_type",
    ],
    includes = [
        "sources/gst-plugins/gst-nvmultistream2",
        "sources/libs/nvstreammux",
        "sources/libs/nvstreammux/include",
    ],
    linkopts = select({
        ":jetson": ["-L/opt/jetson-sysroot/opt/nvidia/deepstream/deepstream/lib"],
        ":arm64-sbsa": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        ":aarch64-linux-gnu": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        ":x86_64-linux-gnu": ["-L/opt/nvidia/deepstream/deepstream/lib"],
        "//conditions:default": [],
    }) + [
        "-ldl",
        "-lpthread",
        "-lm",
        "-lnvdsgst_helper",
        "-lnvdsgst_meta",
        "-lnvds_meta",
        "-lnvbufsurface",
        "-lnvbufsurftransform",
        "-lgstnvdsseimeta",
        "-lnvds_nvtxhelper",
        "-lnvdsbufferpool",
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
    deps = [
        ":deepstream_includes",
        ":deepstream_lib",
        ":hstream_lossless_nvstreammux_backend",
        "@local_cuda//:cuda_headers",
        "@gst_plugin_dev//toolchains/jetson:cuda_runtime",
        "@gstreamer",
        "@yaml-cpp",
    ],
)
