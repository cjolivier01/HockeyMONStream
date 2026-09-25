DeepStream tracker default: NVIDIA ReIdentificationNet deployable_v1.0

NVIDIA Corporation and affiliates. The model and its use remain subject to the
terms linked by NVIDIA's NGC model card (NVIDIA AI Enterprise EULA):
https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/reidentificationnet
https://www.nvidia.com/en-us/data-center/products/nvidia-ai-enterprise/eula/

The default is the SDK-supplied NvDCF accuracy profile's resnet50_market1501.etlt,
not the newer deployable_v1.2 ONNX alternative. The ETLT bytes are unmodified.

Source:
https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.0/files?redirect=true&path=resnet50_market1501.etlt
SHA256: 0e5b7f702ce7e3734e45f27b819866f15f6b083048481d1afae2089136e20201
Bytes: 96377716

HStream preserves SDK preprocessing, including keepAspc=1, but limits preparation
batch/workspace/gallery history to 32 / 256 MiB / 32. The installed tracker builds
a local TensorRT engine in the user cache. No model conversion or new training is
performed, and this integration does not establish hockey-specific ReID accuracy.
