def gst_cc_plugin(
        name,
        interfaces = [],
        srcs = [],
        hdrs = [],
        visibility = None,
        deps = [],
        linkopts = [],
        **kwargs):
    native.cc_binary(
        name = "lib" + name + ".so",
        srcs = srcs + hdrs,
        visibility = visibility,
        # deps = deps + ["@com_extension_dev//:extension_dev"],
        deps = deps + [
            "@deepstream_lib",
            "@deepstream_apps_common",
            "@gstreamer",
            "@yaml-cpp",
            "@local_cuda//:cuda_runtime",
            "@local_cuda//:cuda",
        ],
        linkopts = linkopts + ["-Wl,-no-undefined"],
        linkshared = True,
        **kwargs
    )
