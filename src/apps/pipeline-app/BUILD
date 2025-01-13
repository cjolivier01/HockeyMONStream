
cc_binary(
  name="pipeline-app",
  srcs=glob(["*.c", "*.cpp", "*.h"]),
  deps=[
    "//src/libs/common:utils",
    "@deepstream//sources/apps/apps-common:deepstream_apps_common",
    "@gstreamer",
    "@yaml-cpp",
    "@local_cuda//:cuda_runtime",
    "@local_cuda//:cuda",
  ],
  copts=[
    "-x", "c++",
    "-std=c++17",
  ],
  linkopts=[
    "-L/usr/local/lib/x86_64-linux-gnu",
    "-ljson-glib-1.0",
    "-lX11",
    "-lgstrtspserver-1.0",
    "-ldl",
    "-lm",
  ],
)

