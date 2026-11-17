# Hockey roles detector

Select **Hockey roles — mixed iter 487500 (YOLOv8-m, 1984x736)** in the detector menu.
The default detector is the newer single-class CrowdHuman epoch-90 checkpoint
at the same resolution.
The role model is optional and stays unselected unless changed. It now uses the
original-background checkpoint at iteration 487500, trained with CrowdHuman,
hockey images with restored backgrounds, spectator person labels, and camera
degradation augmentation. Its classes are `0: person`, `1: goalie`, `2: official`.
CrowdHuman mAP is 0.527, compared with 0.530 for the previous gray-background roles
model and 0.534 for the person-only epoch-90 model. Original-background hockey mAP
is 0.623 and official AP is 0.585. The old gray-background hockey score used
different validation imagery/annotations and is not directly comparable.

An offline check of 12 fixed-time video frames from Littleton Hawks and CAHA
Golden Bears found 13 visually confirmed referee detections with this checkpoint,
versus 2 with the gray model, at the same 0.25 threshold. Both still mislabeled
some clear referees as person. This small sample used rendered program frames,
not native detector inputs; it does not establish recall or tracking stability.

The checkpoint, dynamic-batch ONNX, labels, and provenance are published at
https://github.com/cjolivier01/HockeyMONStream/releases/tag/pretrained-assets-v1 .
The four `config_infer_yolov8m_hockey_roles` precision variants pin the same ONNX and
label SHA-256 values. FP32/FP16 run directly; BF16/INT8 use the existing preparation
workflow. The ONNX takes RGB/255 at 1984×736 and returns decoded xyxy, confidence,
and class index; batch sizes 1 and 2 are compared against the iter-487500 checkpoint.
The replacement assets have `hockey_roles_original` and their content hashes in
their filenames. Updated applications download these automatically when the
existing roles option is selected. Old assets remain available for older versions;
new engine and INT8 calibration paths prevent reuse of the gray model's caches.
BF16/INT8 must be prepared again for the new weights. Explicit custom model/engine
overrides retain their selected files. HockeyMON's `hockey-roles` profile pins the
matching PyTorch checkpoint.

Under Program tracking, **Ignore referees for play tracking** and **Ignore goalies
for play tracking** independently exclude those roles from camera subjects. Both
start off. The canonical settings are `rink.tracking.cam_ignore_referees` and
`rink.tracking.cam_ignore_goalies`; native camera YAML uses `ignore-referees` and
`ignore-goalies`. Detections, object tracking, and telemetry remain intact. Tracked
boxes show GOALIE or REF, with no person/player word. A single-class detector cannot
recognize these roles, so its tracks are unaffected by the switches.

NVIDIA uses the detector's semantic labels; AMD uses the model's declared
`person-roles` class indices. Arbitrary nonzero COCO classes are not treated as
hockey roles. NVIDIA and HockeyMON accept live role-filter changes through their
existing frame-owner controls; AMD retains its existing limitation on live camera
tuning and consumes the settings at launch. No video readback is added.

A checkpoint's validation score does not establish accuracy on every rink.
Review the role labels on the actual game before relying on role-based framing.
