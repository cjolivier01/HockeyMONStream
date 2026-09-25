NVIDIA ReIdentificationNet deployable_v1.2

NVIDIA Corporation. ResNet50 person re-identification trained on Market-1501
and AI City Challenge data; see the model card for training provenance.
Model card: https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/reidentificationnet
Model terms: https://www.nvidia.com/en-us/data-center/products/nvidia-ai-enterprise/eula/

The ONNX is downloaded unchanged directly from NVIDIA NGC. It is not mirrored
in the HockeyMONStream release. NVIDIA's model terms govern its use; the MIT
permission notice for the preprocessing example does not license the weights.

Official ONNX:
https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx
SHA256: 0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b

Preprocessing source:
https://raw.githubusercontent.com/NVIDIA-AI-IOT/deepstream_tao_apps/b5363fa47b5539e5c768dcf61065426edd1478a9/configs/nvinfer/reidentificationnet_tao/sgie_reidentificationnet_tao_config.txt
SHA256: fc8a3fd542a04d10995f4d2ffe86e91289a1331e34c4f919bcb8261951cf4bfd
The copyright and permission notice from that example is retained in
licenses/reid-preprocessing-notice.txt.

Deployment profile:
RGB NCHW float32 input [batch,3,256,128], direct resize without preserving aspect
ratio, offsets [123.675,116.28,103.53], and scale 0.01735207357279195.
The unnormalized fc_pred output has 256 float32 components; native NvDCF adds
feature normalization. The tracker profile uses ReID type 2, batch 32, FP16
(networkMode=1 in the tracker API), and extraction interval 8. The NVIDIA
nvinfer sample has a different precision enum and batch size; the profile here
records the native tracker deployment choices explicitly.

The native cache supplies tracker.ReID.modelEngineFile after local TensorRT
preparation. Only the tracker map becomes the runtime overlay, so no ONNX or
engine-build settings are passed to tracker playback.
