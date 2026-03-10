def gst_cc_plugin(
        name,
        interfaces = [],
        srcs = [],
        hdrs = [],
        visibility = None,
        deps = [],
        linkopts = [],
        copts = [],
        **kwargs):
    native.cc_binary(
        name = "lib" + name + ".so",
        srcs = srcs + hdrs,
        visibility = visibility,
        deps = deps + [
            "@deepstream//:deepstream_lib",
            "@gstreamer",
            "@yaml-cpp",
            "//toolchains/jetson:cuda_runtime",
            "@local_cuda//:cuda",
        ],
        copts=[
          "-fPIC",
        ] + copts,
        linkopts = linkopts + ["-Wl,-no-undefined"],
        linkshared = True,
        **kwargs
    )
