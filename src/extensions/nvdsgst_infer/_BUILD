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

exports_files(["LICENSE"])

graph_cc_extension(
    name = "nvdsgst_infer",
    srcs = [
        "nvdsgst_infer_ext.cpp",
    ],
    hdrs = [
        "nvinfer.hpp",
        "nvinfer_interfaces.hpp",
    ],
    deps = [
        "@NvDsBaseExt",
        "@NvDsInterfaceExt",
        "@glib",
        "@gstreamer",
    ],
)

register_extension(
    name = "register_nvdsgst_infer_ext",
    badges = [""],
    extension = "nvdsgst_infer",
    labels = [
        "nvidia",
        "gpu",
        "deepstream",
    ],
    ngc_dependencies = {
        "NvDsBaseExt": "1.6.0",
    },
    priority = "1",
    git_repository = "",
    compute_dependencies = ["deepstream"],
    url = "www.example.com",
    uuid = "1ca0f521-371e-3c65-80fd-0b6a7cac4082",
    version = "0.0.1",
    visibility = ["//visibility:public"],
)
