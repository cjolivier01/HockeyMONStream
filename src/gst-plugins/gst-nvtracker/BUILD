################################################################################
# Copyright 2021-2024, NVIDIA CORPORATION
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

load("@com_extension_dev//build:graph_extension.bzl", "graph_cc_extension")
load("@com_extension_dev//build:registry.bzl", "register_extension")
load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")

exports_files(["LICENSE"])

cc_binary(
    name = "nvdsgst_tracker",
    srcs = [
      "convbufmanager.cpp", "gstnvtracker.cpp", "miscdatabufmanager.cpp", "nvtracker_proc.cpp",
      "convbufmanager.h", "gstnvtracker.h", "invtracker_proc.h", "logging.h", "miscdatabufmanager.h", "nvtracker_proc.h"
    ],
    copts=[
      "-fPIC",
    ],
    linkopts=[
      "-shared",
    ],
    deps = [
        "//src/gst-plugins/gst-nvdspreprocess:nvdsgst_preprocess_lib",
        # "@deepstream_lib",
        "@deepstream//sources/apps/apps-common:deepstream_apps_common",
        #"@deepstream_apps_common",
        # "@NvDsBaseExt",
        # "@NvDsInterfaceExt",
        # "@StandardExtension",
        "@glib",
        "@gstreamer",
        "@local_cuda//:cuda_runtime",
    ],
    visibility = ["//visibility:public"],
    linkshared = True,
)
