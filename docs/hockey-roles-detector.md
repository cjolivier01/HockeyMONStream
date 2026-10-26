# Epoch-10 hockey roles detector

Select **Hockey roles — epoch 10 (YOLOv8-m, 1408x544)** in the detector menu.
The existing default stays selected unless changed. This checkpoint was initialized
from COCO and trained with CrowdHuman and gray-background hockey images. Its classes
are `0: person`, `1: goalie`, `2: official`.

The checkpoint, dynamic-batch ONNX, labels, and provenance are published at
https://github.com/cjolivier01/HockeyMONStream/releases/tag/pretrained-assets-v1 .
The four `config_infer_yolov8m_hockey_roles` precision variants pin the same ONNX and
label SHA-256 values. FP32/FP16 run directly; BF16/INT8 use the existing preparation
workflow. The ONNX takes RGB/255 at 1408×544 and returns decoded xyxy, confidence,
and class index; batch sizes 1 and 2 were compared against the epoch-10 checkpoint.

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

An epoch checkpoint's validation score does not establish accuracy on every rink.
Review the role labels on the actual game before relying on role-based framing.
