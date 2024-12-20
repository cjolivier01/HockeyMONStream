
cc_binary(
  name="pipeline-app",
  srcs=glob(["*.c", "*.cpp", "*.h"]),
  deps=[
    "@deepstream_apps_common",
    "@deepstream_lib",
    "@gstreamer",
    "@yaml-cpp",
    "@local_cuda//:cuda_runtime",
    "@local_cuda//:cuda",
  ],
  linkopts=[
    "-L/usr/local/lib/x86_64-linux-gnu",
    "-ljson-glib-1.0",
    "-lX11",
    "-lgstrtspserver-1.0",
    "-ldl",
    "-lm",
    # "-lnvdsgst_meta",
    # "-lnvds_meta",
    # "-lnvdsgst_helper",
    # "-lnvdsgst_customhelper",
    # "-lnvdsgst_smartrecord",
    #  -lnvds_utils -lnvds_msgbroker -lm -lyaml-cpp \
    # -lcuda 
  ],
)

