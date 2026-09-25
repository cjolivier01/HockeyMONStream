RTMPose-M COCO17

OpenMMLab MMPose / RTMPose contributors.
MMPose copyright 2018-2020 Open-MMLab.
RTMPose: Real-Time Multi-Person Pose Estimation based on MMPose (2023).
Project: https://github.com/open-mmlab/mmpose
Paper: https://arxiv.org/abs/2303.07399

The upstream project is Apache-2.0. Keep the supplied license and source attribution. Training provenance includes AI Challenger and COCO; this notice does not relicense those datasets.

Conversion:
Converted the exact upstream RTMPose-M checkpoint to ONNX opset 17 with dynamic batch and raw COCO17 SimCC outputs. No flip-test augmentation. The fixed profile uses 192x256 RGB affine input, padding 1.25, and SimCC split ratio 2.

Original checkpoint:
https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/rtmpose-m_simcc-aic-coco_pt-aic-coco_420e-256x192-63eb25f7_20230126.pth
SHA256: 5e55be2a03f6e5dcd14d088afc4ae5afe94a4f9de93c22e5deb725ad0eee899d
Configuration SHA256: 532dc32cd9c6ba4ea3300fdc0e0a1da622dd9f53510a1998968f7958e08b2a6b

Portable ONNX:
rtmpose-m-coco17-5449a830cd121954.onnx
SHA256: 5449a830cd12195430c2ac66d93854b47ae8be0cfea0f6ffbe2422d9d28983a0
