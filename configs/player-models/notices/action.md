STGCN++ COCO2D NTU60 (generic activities)

OpenMMLab MMAction2 and PYSKL / STGCN++ contributors.
PYSKL: Towards Good Practices for Skeleton Action Recognition (2022).
MMAction2: https://github.com/open-mmlab/mmaction2
PYSKL: https://github.com/kennymckormick/pyskl
Paper: https://arxiv.org/abs/2205.09443

The upstream project is Apache-2.0. Keep the supplied license and source attribution. Training provenance is NTU RGB+D 60; this notice does not relicense that dataset. This single-window deployment is not published ten-clip evaluation and does not make the generic labels hockey-event predictions.

Conversion:
Converted the exact upstream COCO2D joint STGCN++ checkpoint to ONNX opset 17 with dynamic batch. The wrapper executes the original backbone and classification head. The deployment profile uses one 100-sample causal window, a zero-padded second person, full-canvas normalization, and the original 60 NTU labels.

Original checkpoint:
https://download.openmmlab.com/mmaction/v1.0/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d_20221228-86e1e77a.pth
SHA256: 86e1e77a7b27dd53c6ccc1600a4cc3c321ec8e04d17f25a41fbbb7896026945a
Configuration SHA256: 31bd05179ebe52ccb9ecb194f2e9e8a804b4765d2172a124b646bdc169fd1c82

Portable ONNX:
stgcnpp-coco2d-ntu60-a69d882572452c6c.onnx
SHA256: a69d882572452c6c046d998bd587c39cf614b45844e50d32ec24803ca6a3e9bd
