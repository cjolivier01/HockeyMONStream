# Player analytics model research — 2026-09-24

This report records model research and offline export feasibility verified on 2026-09-24. The selected checkpoint bytes were downloaded and checked, and all three selected models passed PyTorch/ONNX/native TensorRT conversion checks on RTX5090. These checks used synthetic nonzero inputs: they do not establish deployed pipeline performance or hockey accuracy. Sources were fetched directly from the linked official repositories, model cards and papers.

## Recommended concrete first implementation

- **Pose:** RTMPose-M, COCO17, 256×192 crops, FP16 TensorRT, reusing hstream's existing tracked person boxes. RTMPose-S is the credible lower-cost Jetson alternative. Preserve HARPET18 RTMPose-M as a separate hockey-specific pose/jersey profile; it cannot substitute for COCO17 action inputs.
- **Jersey:** a native bounded TensorRT PARSeq adapter, initially using an explicitly supplied hockey-finetuned Koshkina checkpoint with its original architecture/tokenizer/hyperparameters. Local preparation only, record CC BY-NC 3.0 provenance and do not redistribute these weights by default. Generic Apache-2.0 PARSeq is a weaker but available comparison baseline. Future preferred compact replacement: hockey-finetuned SVTRv2-T CTC after dataset/evaluation work; generic OCR benchmark superiority does not establish jersey superiority.
- **Action:** export the concrete MMAction2 STGCN++ COCO2D joint NTU60 model as a native TensorRT classifier. It is a functioning generic skeleton action model, **not a hockey event detector**. Clearly expose NTU60 labels and capability limitations. Keep PoseC3D solely as legacy parity baseline, not performance default. Hockey-specific shot/pass/check/save labels require additional training and evaluation; RGB/context or stick/puck cues are likely required for distinctions body pose cannot support.

RTMPose has official TensorRT support; STGCN++ and PARSeq use dedicated export wrappers whose x86 conversion checks are recorded below. They do not have claimed official MMDeploy skeleton/OCR deployment support. Each target still requires its own engine build and representative-data validation. Prepared ONNX and manifest must exist before enabling an option; never silently construct random weights or silently substitute a CPU runtime.

## Local evidence and source artifacts

`/home/colivier/src/hm/hmlib/config/baseline.yaml:448` currently lists:

1. HARPET RTMPose-M config `hmlib/config/models/body_2d_keypoint/rtmpose/harpet/rtmpose-m_8xb64-crowdpose_harpe-256x192.py`, checkpoint:
   https://github.com/cjolivier01/HockeyMON/releases/download/v0.0.1/rtmpose-m_8xb64-crowdpose_harpe-256x192_best_PCK_epoch_1178.pth
   HTTP GET headers verified 200 and 57,101,163 bytes, body intentionally not downloaded.
2. COCO RTMPose-L 384×288, checkpoint:
   https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/rtmpose-l_simcc-aic-coco_pt-aic-coco_420e-384x288-97d6cb0f_20230228.pth
3. PoseC3D SlowOnly-R50 NTU60 2D keypoints:
   https://download.openmmlab.com/mmaction/v1.0/skeleton/posec3d/slowonly_r50_8xb16-u48-240e_ntu60-xsub-keypoint/slowonly_r50_8xb16-u48-240e_ntu60-xsub-keypoint_20220815-38db104b.pth

Recommended COCO-M replacement was downloaded from the official model zoo: 54,536,690 bytes; checksum and conversion results are recorded below:
https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/rtmpose-m_simcc-aic-coco_pt-aic-coco_420e-256x192-63eb25f7_20230126.pth
Config:
https://github.com/open-mmlab/mmpose/blob/main/projects/rtmpose/rtmpose/body_2d_keypoint/rtmpose-m_8xb256-420e_coco-256x192.py

Recommended STGCN++ checkpoint was downloaded: 5,859,545 bytes; checksum and conversion results are recorded below:
https://download.openmmlab.com/mmaction/v1.0/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d_20221228-86e1e77a.pth
Exact config already exists locally at `openmm/mmaction2/configs/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d.py`.

Koshkina hockey PARSeq and legibility weights are linked by the author. The PARSeq checkpoint was downloaded and verified (381,630,245 bytes); the optional legibility checkpoint was not downloaded in this pass:
https://github.com/mkoshkina/jersey-number-pipeline
https://drive.google.com/file/d/1FyM31xvSXFRusN0sZH0EWXoHwDfB9WIE/view
https://drive.google.com/file/d/1RfxINtZ_wCNVF8iZsiMYuFOP7KMgqgDp/view

The initial checkout survey found no RTMPose, PARSeq, STGCN++, or PoseC3D checkpoints in `/mnt/data/pretrained` (hm's `pretrained` symlink) or `~/.cache/torch/hub/checkpoints`. The research pass subsequently downloaded the selected COCO-M, STGCN++ and hockey PARSeq artifacts into separate scratch storage; this does not change those existing checkout paths. Configured PARSeq path `pretrained/parseq/jersey/hockey/epoch=3-step=95-val_accuracy=98.7903-val_NED=99.3952.ckpt` does not exist here. Do not present that filename's validation number as accuracy measured for this integration.

Existing `JerseyNumberFromPosePlugin` uses MMOCR FCENet text detection plus ABINet recognition. `KoshkinaJerseyNumberPlugin._run_parseq_on_crop` copies each crop to CPU/PIL then back to GPU and decodes per crop; it must not be ported literally. Its initialization can fall back to randomly initialized PARSeq after loading failure, which must not be retained. `ActionFromPosePlugin` uses CPU keypoint arrays and per-track MMAction inference; its current checkpoint is NTU60, not a hockey-trained action model.

Preparation environment: default `python` is `/home/colivier/miniforge3/envs/ubuntu/bin/python`, Python 3.14, with discoverable torch/mmpose/mmengine/mmcv/TensorRT but no onnx, onnxruntime or pytorch_lightning. `python312` env exists but has none of these packages. Local source trees exist for MMPose/MMAction2/PARSeq. A separate scratch virtual environment was subsequently created with pinned ONNX/export dependencies, preserving the base environment. The base Python TensorRT11.2 differs from DeepStream's actual TensorRT10.16 and was not used to build engines. Inference deployment remains native and Python-free.

## Pose alternatives and evidence

| Candidate | Actual evidence/artifact | Decision |
|---|---|---|
| RTMPose-M (2023) | Official COCO table: 75.8 AP, 13.59M parameters, 1.93 GFLOPs, 2.29 ms TensorRT FP16 on GTX1660Ti for 256×192. PTH and export configuration available; MMPose/MMDeploy Apache-2.0, TRT support explicitly listed. | Recommended practical top-down baseline. Existing player detector is already required; only crop/pose cost is incremental. |
| RTMPose-S (2023) | 72.2 AP, 5.47M params, 0.68 GFLOPs, 1.39 ms under same reported setup. | Low-power candidate if hockey evaluation permits. |
| HARPET RTMPose-M | Existing repo checkpoint, 18 joints including stick endpoints. Local config uses 256×192. No independent test result verified. | Preserve as explicit hockey profile; benchmark against COCO17. |
| RTMO (CVPR2024; released Dec 2023) | Official RTMO-L body7: 74.8 AP, reported 141 FPS in paper; model-zoo ORT latency 19.1 ms on V100 is a **different execution setup**, not contradictory interchangeable TRT timing. Actual ONNX SDK zip downloads and MMDeploy TRT recipe exist. | One-stage alternative, but reruns person localization and needs object-ID association. Whole panorama resize loses tiny-player detail; tiling adds cost. Do not replace known tracking pipeline before measured benefit. |
| YOLO26-pose (Jan 2026) | Current official pose page: N 57.2 AP/1.8 ms, S 63.0 AP/2.7 ms, M 68.8 AP/5.0 ms, X 71.6 AP/12.2 ms; table says T4 TensorRT10. Public PT weights and ONNX/TensorRT export; AGPL-3.0 or enterprise. | Real current alternative; same full-frame/tiny-person and re-association concerns. Licensing differs from OpenMMLab. Do not apply detection AP figures to pose. |
| ViTPose / ViTPose++ | Released official checkpoints, generally stronger accuracy backbones and cost; paper/model-zoo evidence, no local hockey or target speed test. | Offline reference/teacher or optional high-quality evaluation, not assume better total-cost default. |
| Sapiens2 (ICLR2026 repo) | Public safetensors, 308 whole-body joints. Smallest listed 0.1B model: 114M parameters, 0.342 TFLOPs at 1024×768; up to 5B. Custom Sapiens2 License. | Current frontier quality family but grossly mismatched to bounded per-player hockey enrichment cost; possible offline labeling/teacher only. |
| NVIDIA BodyPoseNet | NGC model card gives 18 OpenPose-style joints and legacy TRT7/8 INT8 calibration files. Old `deepstream_pose_estimation` sample explicitly no longer maintained and targets DS5/TRT7. | Evidence/reference, not a free modern drop-in or reason to port old binaries. |
| NVIDIA BodyPose3DNet | Current TAO repo downloads actual ONNX. HRNet 34 joints, five inputs including image, intrinsics inverse, inverse transform, limb lengths; NVIDIA Open Model License. | Native NVIDIA-supported alternative if actual 3D needed, but added geometry contract and compute unnecessary for initial 2D pose. |

Sources:
- https://github.com/open-mmlab/mmpose/tree/main/projects/rtmpose
- https://github.com/open-mmlab/mmdeploy/blob/main/docs/en/04-supported-codebases/mmpose.md
- https://github.com/open-mmlab/mmpose/tree/main/projects/rtmo
- https://arxiv.org/abs/2312.07526
- https://docs.ultralytics.com/models/yolo26/
- https://docs.ultralytics.com/tasks/pose/
- https://github.com/ViTAE-Transformer/ViTPose
- https://github.com/facebookresearch/sapiens2 (model cards and custom LICENSE.md)
- https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/bodyposenet
- https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/bodypose3dnet
- https://github.com/NVIDIA-AI-IOT/deepstream_pose_estimation
- https://github.com/NVIDIA-AI-IOT/deepstream_tao_apps/blob/master/download_models.sh

Published numbers above are source claims on other hardware and datasets, often isolated model latency. None establishes 5090/Jetson throughput, hockey pose quality, concurrent-program cost, or hstream's end-to-end latency. COCO AP is not directly comparable to HARPET PCK.

### Pose preparation/runtime contract

Export backbone → optional neck → RTMCC head, outputs raw `simcc_x[B,K,384]` and `simcc_y[B,K,512]` for input `[B,3,256,192]`. Export K=17 for COCO, K=18 for HARPET. Decode argmax and score exactly as pinned SimCC implementation; coordinate divide by split ratio 2.0 and inverse affine transform into stitched canvas. Prefer GPU decode and copy compact K×(x,y,score) results only. Raw score handling is part of the manifest; do not invent sigmoid/softmax and then reuse thresholds from unnormalized reference scores.

GPU preprocessing must match MMPose: bbox center/scale, default padding 1.25, expand to 192:256 aspect, affine crop, border/pixel-center rules, bilinear sampling, RGB order, mean `[123.675,116.28,103.53]`, std `[58.395,57.12,57.375]` in 0..255 units. Source RGBA must become RGB directly; `bgr_to_rgb=True` in Python config does not mean reverse an already RGB buffer. Configs enable flip-test for reference evaluation; native low-cost profile should explicitly disable flip augmentation and measure its quality difference. A raw-head export does not automatically include flip-test.

HARPET18 local metainfo `openmm/mmpose/configs/_base_/datasets/harpe18.py` has anonymous joint names j0..j17 and stick joints 16,17; don't infer a full COCO mapping. `topology_id`, names/edges/torso indices and provenance are mandatory. Reject incompatible action bundles before pipeline allocation.

## Jersey alternatives and evidence

1. **Koshkina/Elder CVPRW2024:** paper reports **91.4% hockey image-level** jersey accuracy after hockey PARSeq fine-tuning versus 85.4% generic PARSeq; SoccerNet 87.45% test/79.31% challenge tracklet accuracy. The authors supply hockey-finetuned STR and ResNet34 legibility weights. This is the strongest directly hockey-relevant downloaded-source evidence found, and it matches an existing hm option. Author repo/work is CC BY-NC 3.0; PARSeq upstream code Apache-2.0 does not relicense derived weights/dataset. Local use depends on applicable terms; omission of redistribution alone does not eliminate noncommercial conditions.
2. **Grad CVPRW2025 uncertainty-aware jersey model:** actual public ViT-S and ViT-B SoccerNet checkpoints, digit-compositional classifiers, uncertainty scoring, CC-BY-SA-4.0 repo. Released ViT-B SoccerNet result 86.37% test/83.52% challenge. Highest 85.62% challenge model (200M proprietary training dataset) has **no download link**. Better challenge result than 2024 does not prove hockey performance. Worth comparison for joint legibility/recognition and efficient fixed-output export, not a known deployable hockey upgrade.
3. **SVTRv2 ICCV2025:** official repo public model links (T/S/B), T ~5.13M, source table 5.0 latency with test conditions requiring inspection; no 5090/Jetson promise. CTC removes autoregressive decode and is attractive for digits. Official OpenOCR ONNX export path exists, Apache-2.0. Requires hockey fine-tuning or at least a held-out hockey evaluation before replacement. Generic text word accuracy and generic OCR latency are not jersey accuracy.
4. **NVIDIA LPRNet:** actual DeepStream secondary OCR path and TensorRT integration exist. License-plate recognizer weights target plate characters/layouts, not jerseys. Reuse ideas for batched ROI classification/custom parser, not claim LPR weights recognize hockey reliably.

Sources:
- https://openaccess.thecvf.com/content/CVPR2024W/CVsports/papers/Koshkina_A_General_Framework_for_Jersey_Number_Recognition_in_Sports_Video_CVPRW_2024_paper.pdf
- https://github.com/mkoshkina/jersey-number-pipeline
- https://github.com/baudm/parseq and /blob/main/LICENSE
- https://github.com/lukaszgrad/uncertainty-jnr
- https://openaccess.thecvf.com/content/CVPR2025W/CVSPORTS/papers/Grad_Single-Stage_Uncertainty-Aware_Jersey_Number_Recognition_in_Soccer_CVPRW_2025_paper.pdf
- https://github.com/Topdu/OpenOCR/tree/main/configs/rec/svtrv2
- https://arxiv.org/abs/2411.15858
- https://github.com/Topdu/OpenOCR/blob/main/docs/openocr.md#exporting-to-onnx-engine
- https://github.com/NVIDIA-AI-IOT/deepstream_lpr_app

### Jersey export/runtime contract

Input `[B,3,32,128]` RGB is the normal PARSeq profile, but assert exact `img_size` from checkpoint hyperparameters. Reference PARSeq `SceneTextDataModule.get_transform` uses **bicubic resize**, then tensor /255 and normalize mean/std 0.5 → [-1,1]. A bilinear GPU resize is an intentional approximation requiring quality measurement, not exact parity. Implement bounded bicubic CUDA resize if exact reference required. Torso ROI derived from fresh pose shoulders/hips, bbox fallback when pose disabled/unavailable; quality/min-size/occlusion and legibility gating keep bad crops from increasing false certainty.

The downloaded hockey checkpoint has 95 output classes: EOS, digits, letters and punctuation. Keep its original training vocabulary and token indices, and compute probabilities over all 95 classes. Restrict accepted decoded labels to digit strings of supported length; slicing to 11 logits before softmax would wrongly redistribute non-digit probability. Preserve leading zeros as strings. For original PARSeq, wrapper `forward(images,max_length=2)` creates exactly 3 prediction positions (2 characters+EOS), suppresses data-dependent early EOS loop termination (`testing=False`) and preserves original AR + refinement settings. This yields a bounded export graph without silently switching to NAR or dropping refinement. The verified output is `[B,3,95]`; compact probabilities/token indices can be decoded on host, with no crop/image readback. Need ONNX/TensorRT parity including blank/no-number, one/two digits, leading zero, partial occlusion and stop token, and strict state-dict loading. If fixed length conflicts with checkpoint's label setup, preparation fails.

ResNet34 legibility `[B,3,128,128]` is available as an optional separately budgeted gate, not assumed zero cost. Crop count selection, temporal aggregation and abstention often matter more than switching generic OCR architecture. Require confidence margin/unknown state and independent evidence across sampled times; stable numbers reduce re-inference cadence; tracker ID switches/epochs invalidate votes.

## Action models, labels and exact input

**STGCN++** (PYSKL/ACMMM2022) is a sensible performance baseline, with code/config/checkpoint available. Official MMAction2 table lists 1.39M parameters and 1.95 GFLOPs for COCO2D joint model; 89.29% NTU60 xsub is **10-clip** testing. No claim that one causal sliding clip reaches that accuracy. Apache-2.0 MMAction2 source.

Its configured native backbone input is `[B,M=2,T=100,V=17,C=3]`, C=(normalized x, normalized y, keypoint score). A thin wrapper `cls_head(backbone(x))` returns `[B,60]` logits. Full RecognizerGCN API expects extra `num_clips` axis `[B,1,2,100,17,3]`; skip API and pin single-clip semantics. The second person slot is **all zeros**, matching `FormatGCNInput(num_person=2, mode='zero')`, not a duplicate unless model profile explicitly says loop. GPU or compact CPU skeleton assembly is permitted; it never requires image readback. `PreNormalize2D`: x=(x-W/2)/(W/2), y=(y-H/2)/(H/2), using recorded source/canvas coordinate dimensions, not silently torso-relative recentering. Causal time sampling of a sliding window differs from training's full-action uniform sampling; record sample cadence, 100 positions, window duration, gaps, and freshness, and validate before treating predictions as useful.

No official MMDeploy skeleton deployment entry was found: its MMAction support table lists TSN, SlowFast, TSM, X3D. A dedicated PyTorch→ONNX export for STGCN++ passed the x86 native TensorRT parser/build/parity checks below, including its standard Einsum operations without custom plugins. This is an offline conversion result, not an integrated hockey action-quality result. Pin opset appropriate to both runtime SDKs, freeze T/V/M, dynamic or discrete bounded B only. Decompose unsupported einsum/matmul patterns if needed without numerically changing the model; require dual-platform engine builds and reference comparison.

NTU60 labels include falling, staggering, cheer up, hand waving, pushing another person, but most are daily activities (drink/eat/read/write/put on a jacket etc). Never rename a generic score “shot/pass/check” by label mapping. Whole-player independent inference loses interaction context, so even pushing has limited meaning. Actual source label map:
https://github.com/open-mmlab/mmaction2/blob/main/tools/data/skeleton/label_map_ntu60.txt

**NVIDIA PoseClassificationNet** is an actual alternative already exported as `st-gcn_3dbp_nvidia.onnx`, but needs NVIDIA **34-joint 3D** `(N,3,300,34,1)` skeletons, and outputs only sitting_down/getting_up/sitting/standing/walking/jumping. It cannot consume RTMPose COCO17 2D by reshaping. Model card and downloader:
https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/poseclassificationnet
https://github.com/NVIDIA-AI-IOT/deepstream_tao_apps/blob/master/download_models.sh

**RGB alternatives:** MMAction2 X3D/TSM have official TensorRT deployment routes and available Kinetics checkpoints, unlike large frontier VideoMAEv2/UniFormerV2/VideoMamba models that add substantial clip cost. Kinetics400 labels include `hockey stop`, `ice skating`, `playing ice hockey`, but not the full desired within-hockey shot/pass/check/save ontology. These broad sports labels cannot provide useful hockey event recognition out of the box. They are research/fine-tuning alternatives after an event dataset exists, not an excuse to buffer per-player RGB clips in v1.

Sources:
- https://github.com/open-mmlab/mmaction2/tree/main/configs/skeleton/stgcnpp
- https://github.com/open-mmlab/mmdeploy/blob/main/docs/en/04-supported-codebases/mmaction2.md
- https://github.com/open-mmlab/mmaction2/tree/main/configs/skeleton/posec3d
- https://github.com/open-mmlab/mmaction2/blob/main/tools/data/kinetics/label_map_k400.txt
- https://github.com/OpenGVLab/VideoMAEv2
- https://github.com/OpenGVLab/UniFormerV2
- https://github.com/OpenGVLab/VideoMamba

## What installed DeepStream actually provides

Local `/opt/nvidia/deepstream/deepstream/version` is **9.1.0, build date 2026-06-25**. It contains `deepstream-3d-action-recognition`, full custom CUDA sequence preprocessing source, `libnvds_custom_sequence_preprocess.so`, and bodypose postprocessor source. Its README explicitly tells users to download TAO models; **sample code/binaries do not establish installed model weights**. Demo ONNX names: `resnet18_3d_rgb_hmdb5_32.onnx`, `resnet18_2d_rgb_hmdb5_32.onnx`. Labels: push/fall_floor/walk/run/ride_bike. Example input 3D `[4,3,32,224,224]`; 2D temporal stacking `[4,96,224,224]`.

Reference pipeline `nvstreammux → nvdspreprocess → nvinfer` supports custom GPU spatial/temporal batching. Sample `subsample` and `stride` prove useful scheduling mechanisms, not track identity/lifecycle correctness for moving ROI clips. One FP16 RGB tensor with B=4, C=3, T=32, H=W=224 alone costs ~36.75 MiB; per-track slots/multiple outputs grow quickly, while a 100×17×3 float32 skeleton history is ~20 KiB per person. This motivates skeleton default.

Sources:
https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_3D_Action.html
https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvdspreprocess.html
Installed sample README/config/source paths under `sources/apps/sample_apps/deepstream-3d-action-recognition/`.

## Runtime and performance contract

- Build engines **on each actual target/runtime**. Never move serialized 5090/DS9.1 TensorRT engines to JP6/DS7 Jetson. Portable artifact is pinned ONNX+manifest; local cache key includes graph digest, TensorRT/CUDA/runtime build, GPU compute capability/device identity, precision, shape profile and plugin ABI. Verify SDK runtime paired with builder rather than relying on the first TensorRT Python package on PATH.
- Default FP16, use FP32 where required for score/normalization stability. INT8 is a separately calibrated and evaluated derivative, not default just because detector supports it. Old TAO INT8 calibration files do not establish TRT-current or hockey validity.
- Bound every feature by per-media-time cadence, tracks/ROIs, batch, pending work, result age, history, and GPU memory. Suggested starting experiment (not validated defaults): pose 5–10 Hz, at most8 crops per inference batch; jersey 1–2 Hz only large readable uncertain tracks; action 1 Hz when enough history. Pose cadence/window duration must agree with action profile; randomly skipping to meet a budget must produce missing/unknown or resampled/gap-marked clips, not pretend uniform original-time data.
- All-disabled graph omits element/branch, download, model session, worker thread, GPU context/pool, history, and metadata hooks. Runtime flags inside an always-running worker do not meet literal disabled-zero-cost.
- Pose consumer dependencies are explicit: jersey bbox mode works without pose; action's COCO skeleton profile needs matching pose enabled, so configuration either rejects or clearly computes an effective dependency, rather than secretly loading pose. Overlay off should not imply inference off when action/jersey still consumes pose.
- Crop/normalize on GPU from actual stitched surface and preserve exact frame/source/geometry identity. Keep queue bounds; under load skip analytics jobs, not video buffers; never leak/drop source or archive frames. GPU decode/readback only compact numbers. Cache no `NvDsObjectMeta*` beyond buffer lifetime.
- Benchmark all-disabled equivalence then pose-only/jersey-only/action(+pose)/all. Report model and complete pipeline P50/P95 time, processed video FPS, sampled task Hz, starvation/result age, output-frame completeness, VRAM high-water, CPU usage, and Nsight proof of no video D2H. Compare 5090 and `stubby` with actual workload, not internet latency arithmetic. Quality evaluation needs held-out hockey clips by number size, pose occlusion and jersey readability; generic action output additionally needs honest unknown handling/domain-limit label.


## Verified artifacts and conversion results

The following actual files were acquired from the URLs above. SHA256 identifies bytes,
not a license grant or an accuracy result.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| RTMPose-M COCO17 checkpoint | 54,536,690 | `5e55be2a03f6e5dcd14d088afc4ae5afe94a4f9de93c22e5deb725ad0eee899d` |
| STGCN++ COCO2D NTU60 checkpoint | 5,859,545 | `86e1e77a7b27dd53c6ccc1600a4cc3c321ec8e04d17f25a41fbbb7896026945a` |
| Hockey PARSeq checkpoint | 381,630,245 | `5a31899866c092ff26b245898d1fb16ecaabcd091afaadeb6a9c056ca6c846e3` |
| NVIDIA ReIdentificationNet deployable_v1.2 ONNX | 96,398,132 | `0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b` |

Strict weight loading passed for all three pose/jersey/action models. The PARSeq file
contains the original 95-class head, 25-character training maximum, autoregressive
decoding and one refinement iteration; the bounded wrapper preserves those operations
and explicitly limits inference to two characters plus EOS. Its axis-free upstream
`squeeze()` must become `squeeze(1)` on the known one-token axis for TensorRT dynamic
batch parsing. The wrapper was compared against the original PyTorch forward path.
RTMPose's legacy NumPy metadata was loaded using a narrow safe-type allowlist; the
known SHA-pinned PARSeq optimizer/scheduler pickle was converted only offline.

The separate exporter environment used PyTorch2.11.0a0 from the explicit base
installation plus ONNX1.23/ONNXRuntime1.30. Native builds used coherent TensorRT10.16.1
headers and `.so.10` libraries, not the default Python11.2 or system11.3 development
SDK. The resulting native runtime reported TRT10.16.1 build11, CUDA runtime13040,
RTX5090 compute capability12.0. Engine identity records the loaded CUDA runtime, which
can differ from the compile-time header minor version.

The staged preparation command exported opset17 models and tested dynamic batches
1,2,8 against the original PyTorch model, ONNX Runtime and native enqueueV3 engines.
All three passed with recorded tolerances on deterministic nonzero synthetic inputs.
The following initial batch2 probes show the numerical scale; the preparation reports
retain every batch's results:

| Model | PyTorch→ONNX maximum absolute error | PyTorch→TRT FP16 maximum absolute error | Decision observation |
|---|---:|---:|---|
| RTMPose-M | <9.4e-7 | 0.00383 | SimCC argmax differed by at most one bin in this probe |
| STGCN++ | 3.1e-6 | 0.00577 | Both top1 predictions matched |
| PARSeq hockey | 1.24e-5 | 0.03199 | All token top1 predictions matched |

RTMPose's builder reported two large constants clipped to FP16 range. Numerical checks
on synthetic crops passed, but actual player-crop confidence/coordinate parity and
held-out hockey accuracy remain required. None of these model-only probes measures
video readback, tracker association, overlay correctness, application throughput or
Jetson pose/jersey/action integration.

## Concrete ReID reference

The official NVIDIA downloader provides an unencrypted ONNX with no credentials:

https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx

Sources:
- https://github.com/NVIDIA-AI-IOT/deepstream_tao_apps/blob/master/download_models.sh
- https://github.com/NVIDIA-AI-IOT/deepstream_tao_apps/blob/master/configs/nvinfer/reidentificationnet_tao/sgie_reidentificationnet_tao_config.txt
- https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/reidentificationnet
- https://api.ngc.nvidia.com/v2/models/nvidia/tao/reidentificationnet

Inspected ONNX opset14 input is `input[B,3,256,128]`, output `fc_pred[B,256]`.
The output requires L2 normalization. Its artifact-specific published preprocessing is
RGB/NCHW, offsets `[123.675,116.28,103.53]`, scale `0.01735207357279195`, and stretch
resize (`maintain-aspect-ratio=0`, corresponding to tracker `keepAspc:0`). The installed
NvDCF template targets older ETLT weights and uses `keepAspc:1`; copying that value
would change this ONNX recipe. Tracker `networkMode:1` means FP16, whereas nvinfer
`network-mode:2` means FP16; the enums are different.

The native x86 FP16 engine built and executed with profile minimum1/optimum2/maximum8.
Against ONNX Runtime, the initial batch2 probe had maximum absolute embedding error
0.00970 and normalized embedding cosine at least0.999999. A separate engine built
successfully on Jetson TensorRT10.3 with the same profile. These conversion results
do not establish hockey reassociation accuracy; actual NvDCF loading and tracking
validation belongs to the implementation's platform test record.

The model card (updated2024-11-27) says ready for commercial use and links its licensing
section to the [NVIDIA AI Enterprise EULA](https://www.nvidia.com/en-us/data-center/products/nvidia-ai-enterprise/eula/).
Do not label the weights Apache-2.0 based on the sample code's license.
