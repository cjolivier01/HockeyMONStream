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
            "@deepstream_lib",
            #"@deepstream_apps_common",
            "@deepstream_sources//:deepstream_includes",
            "@deepstream_sources//apps/apps-common:deepstream_apps_common",
            "@gstreamer",
            "@yaml-cpp",
            "@local_cuda//:cuda_runtime",
            "@local_cuda//:cuda",
        ],
        copts=[
          "-fPIC",
        ] + copts,
        linkopts = linkopts + ["-Wl,-no-undefined"],
        linkshared = True,
        **kwargs
    )
