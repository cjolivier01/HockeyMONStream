// Generated from configs/player-models/*.json; regenerate as documented there.

constexpr const char kPoseContract[] = R"MODEL({
  "feature": "pose",
  "inputs": [
    {
      "dtype": "float32",
      "name": "image",
      "shape": [
        -1,
        3,
        256,
        192
      ]
    }
  ],
  "model_family": "rtmpose",
  "model_id": "rtmpose-m-coco17-256x192",
  "onnx": {
    "file": "model.onnx",
    "opset": 17,
    "sha256": "5449a830cd12195430c2ac66d93854b47ae8be0cfea0f6ffbe2422d9d28983a0"
  },
  "outputs": [
    {
      "dtype": "float32",
      "name": "simcc_x",
      "shape": [
        -1,
        17,
        384
      ]
    },
    {
      "dtype": "float32",
      "name": "simcc_y",
      "shape": [
        -1,
        17,
        512
      ]
    }
  ],
  "preprocessing": {
    "color": "rgb",
    "flip_test": false,
    "id": "rtmpose-coco17-affine-v1",
    "mean": [
      123.675,
      116.28,
      103.53
    ],
    "padding": 1.25,
    "simcc_split_ratio": 2.0,
    "std": [
      58.395,
      57.12,
      57.375
    ]
  },
  "schema_version": 1,
  "source": {
    "config_sha256": "532dc32cd9c6ba4ea3300fdc0e0a1da622dd9f53510a1998968f7958e08b2a6b",
    "license": "Apache-2.0 (code); retain upstream checkpoint and training-data provenance",
    "sha256": "5e55be2a03f6e5dcd14d088afc4ae5afe94a4f9de93c22e5deb725ad0eee899d",
    "url": "https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/rtmpose-m_simcc-aic-coco_pt-aic-coco_420e-256x192-63eb25f7_20230126.pth"
  },
  "topology_id": "coco17"
}
)MODEL";

constexpr const char kJerseyContract[] = R"MODEL({
  "feature": "jersey",
  "inputs": [
    {
      "dtype": "float32",
      "name": "image",
      "shape": [
        -1,
        3,
        32,
        128
      ]
    }
  ],
  "model_family": "parseq",
  "model_id": "parseq-hockey-cvprw2024",
  "onnx": {
    "file": "model.onnx",
    "opset": 17,
    "sha256": "df48ad7463aa89a5e852f9c4cf2616f0cb9a4e7490ef380b0c6ab67f110228b1"
  },
  "outputs": [
    {
      "dtype": "float32",
      "name": "logits",
      "shape": [
        -1,
        3,
        95
      ]
    }
  ],
  "preprocessing": {
    "align_corners": false,
    "antialias": true,
    "charset": [
      "[E]",
      "0",
      "1",
      "2",
      "3",
      "4",
      "5",
      "6",
      "7",
      "8",
      "9",
      "a",
      "b",
      "c",
      "d",
      "e",
      "f",
      "g",
      "h",
      "i",
      "j",
      "k",
      "l",
      "m",
      "n",
      "o",
      "p",
      "q",
      "r",
      "s",
      "t",
      "u",
      "v",
      "w",
      "x",
      "y",
      "z",
      "A",
      "B",
      "C",
      "D",
      "E",
      "F",
      "G",
      "H",
      "I",
      "J",
      "K",
      "L",
      "M",
      "N",
      "O",
      "P",
      "Q",
      "R",
      "S",
      "T",
      "U",
      "V",
      "W",
      "X",
      "Y",
      "Z",
      "!",
      "\"",
      "#",
      "$",
      "%",
      "&",
      "'",
      "(",
      ")",
      "*",
      "+",
      ",",
      "-",
      ".",
      "/",
      ":",
      ";",
      "<",
      "=",
      ">",
      "?",
      "@",
      "[",
      "\\",
      "]",
      "^",
      "_",
      "`",
      "{",
      "|",
      "}",
      "~"
    ],
    "color": "rgb",
    "cubic_coefficient": -0.5,
    "eos_index": 0,
    "id": "parseq-rgb-bicubic-v1",
    "input_range": [
      -1,
      1
    ],
    "interpolation": "bicubic",
    "max_length": 2,
    "quantize_uint8_after_resize": true,
    "resize_coordinate_mode": "half_pixel"
  },
  "schema_version": 1,
  "source": {
    "config_sha256": "b1c1374a8e672b0929c1eb26ed0eee4da199bacec84602033c018d15357e400c",
    "license": "CC-BY-NC-3.0; model local use remains subject to noncommercial terms",
    "sha256": "5a31899866c092ff26b245898d1fb16ecaabcd091afaadeb6a9c056ca6c846e3",
    "url": "https://drive.google.com/file/d/1FyM31xvSXFRusN0sZH0EWXoHwDfB9WIE/view"
  }
}
)MODEL";

constexpr const char kActionContract[] = R"MODEL({
  "feature": "action",
  "inputs": [
    {
      "dtype": "float32",
      "name": "skeleton",
      "shape": [
        -1,
        2,
        100,
        17,
        3
      ]
    }
  ],
  "labels": [
    "drink water",
    "eat meal/snack",
    "brushing teeth",
    "brushing hair",
    "drop",
    "pickup",
    "throw",
    "sitting down",
    "standing up (from sitting position)",
    "clapping",
    "reading",
    "writing",
    "tear up paper",
    "wear jacket",
    "take off jacket",
    "wear a shoe",
    "take off a shoe",
    "wear on glasses",
    "take off glasses",
    "put on a hat/cap",
    "take off a hat/cap",
    "cheer up",
    "hand waving",
    "kicking something",
    "reach into pocket",
    "hopping (one foot jumping)",
    "jump up",
    "make a phone call/answer phone",
    "playing with phone/tablet",
    "typing on a keyboard",
    "pointing to something with finger",
    "taking a selfie",
    "check time (from watch)",
    "rub two hands together",
    "nod head/bow",
    "shake head",
    "wipe face",
    "salute",
    "put the palms together",
    "cross hands in front (say stop)",
    "sneeze/cough",
    "staggering",
    "falling",
    "touch head (headache)",
    "touch chest (stomachache/heart pain)",
    "touch back (backache)",
    "touch neck (neckache)",
    "nausea or vomiting condition",
    "use a fan (with hand or paper)/feeling warm",
    "punching/slapping other person",
    "kicking other person",
    "pushing other person",
    "pat on back of other person",
    "point finger at the other person",
    "hugging other person",
    "giving something to other person",
    "touch other person's pocket",
    "handshaking",
    "walking towards each other",
    "walking apart from each other"
  ],
  "model_family": "stgcnpp",
  "model_id": "stgcnpp-coco2d-joint-ntu60",
  "onnx": {
    "file": "model.onnx",
    "opset": 17,
    "sha256": "a69d882572452c6c046d998bd587c39cf614b45844e50d32ec24803ca6a3e9bd"
  },
  "outputs": [
    {
      "dtype": "float32",
      "name": "logits",
      "shape": [
        -1,
        60
      ]
    }
  ],
  "preprocessing": {
    "coordinate_normalization": "full_canvas_minus1_plus1",
    "id": "stgcnpp-coco17-causal-v1",
    "max_gap_ns": 150000000,
    "sample_period_ns": 100000000,
    "samples": 100,
    "second_person": "zeros"
  },
  "schema_version": 1,
  "source": {
    "config_sha256": "31bd05179ebe52ccb9ecb194f2e9e8a804b4765d2172a124b646bdc169fd1c82",
    "license": "Apache-2.0 (code); retain upstream checkpoint and NTU60 training-data provenance",
    "sha256": "86e1e77a7b27dd53c6ccc1600a4cc3c321ec8e04d17f25a41fbbb7896026945a",
    "url": "https://download.openmmlab.com/mmaction/v1.0/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d_20221228-86e1e77a.pth"
  },
  "topology_id": "coco17"
}
)MODEL";

constexpr const char kReidContract[] = R"MODEL({
  "feature": "reid",
  "inputs": [
    {
      "dtype": "float32",
      "name": "input",
      "shape": [
        -1,
        3,
        256,
        128
      ]
    }
  ],
  "model_family": "reidentificationnet",
  "model_id": "reidentificationnet-deployable-v1.2",
  "onnx": {
    "file": "model.onnx",
    "opset": 14,
    "sha256": "0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b"
  },
  "outputs": [
    {
      "dtype": "float32",
      "name": "fc_pred",
      "shape": [
        -1,
        256
      ]
    }
  ],
  "schema_version": 1,
  "source": {
    "config_sha256": "fc8a3fd542a04d10995f4d2ffe86e91289a1331e34c4f919bcb8261951cf4bfd",
    "license": "NVIDIA AI Enterprise EULA; see the NVIDIA NGC model card and applicable model terms",
    "sha256": "0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b",
    "url": "https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx"
  },
  "tracker": {
    "ReID": {
      "addFeatureNormalization": 1,
      "batchSize": 32,
      "colorFormat": 0,
      "inferDims": [
        3,
        256,
        128
      ],
      "inputOrder": 0,
      "keepAspc": 0,
      "minVisibility4GalleryUpdate": 0.6,
      "modelEngineFile": "model.engine",
      "netScaleFactor": 0.01735207357279195,
      "networkMode": 1,
      "offsets": [
        123.675,
        116.28,
        103.53
      ],
      "outputReidTensor": 0,
      "reidFeatureSize": 256,
      "reidHistorySize": 32,
      "reidType": 2,
      "workspaceSize": 256
    },
    "TrajectoryManagement": {
      "enableReAssoc": 1,
      "reidExtractionInterval": 8
    }
  }
}
)MODEL";
