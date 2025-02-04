
cc_binary(
  name="pipeline-app",
  srcs=glob(["*.c", "*.cpp", "*.h"]),
  deps=[
    "//src/libs/common:utils",
    "@deepstream//sources/apps/apps-common:deepstream_apps_common",
    "@deepstream//sources/apps/sample_apps/deepstream-audio:deepstream_audio",
    "@gstreamer",
    "@yaml-cpp",
    "@json_glib",
    "@local_cuda//:cuda_runtime",
    "@local_cuda//:cuda",
  ],
  copts=[
    "-x", "c++",
    "-std=c++17",
  ],
  linkopts=[
    "-lX11",
    "-ldl",
    "-lm",
  ],
)

