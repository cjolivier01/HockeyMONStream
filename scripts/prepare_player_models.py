#!/usr/bin/env python3
"""Explicit offline export and target-local TensorRT preparation for player models.

Installed playback never imports this module. Export requires a compatible HockeyMON
source checkout and PyTorch environment; build requires only NumPy and the native
player-model-builder linked to the same TensorRT SDK as DeepStream.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

PROFILES = {
    "pose": {
        "model_id": "rtmpose-m-coco17-256x192",
        "model_family": "rtmpose",
        "url": "https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/rtmpose-m_simcc-aic-coco_pt-aic-coco_420e-256x192-63eb25f7_20230126.pth",
        "sha256": "5e55be2a03f6e5dcd14d088afc4ae5afe94a4f9de93c22e5deb725ad0eee899d",
        "license": "Apache-2.0 (code); retain upstream checkpoint and training-data provenance",
        "config": "openmm/mmpose/projects/rtmpose/rtmpose/body_2d_keypoint/rtmpose-m_8xb256-420e_coco-256x192.py",
        "input": ("image", [3, 256, 192]),
        "outputs": [("simcc_x", [17, 384]), ("simcc_y", [17, 512])],
        "preprocessing": {
            "id": "rtmpose-coco17-affine-v1", "color": "rgb",
            "mean": [123.675, 116.28, 103.53], "std": [58.395, 57.12, 57.375],
            "padding": 1.25, "simcc_split_ratio": 2.0, "flip_test": False,
        },
    },
    "jersey": {
        "model_id": "parseq-hockey-cvprw2024",
        "model_family": "parseq",
        "url": "https://drive.google.com/file/d/1FyM31xvSXFRusN0sZH0EWXoHwDfB9WIE/view",
        "sha256": "5a31899866c092ff26b245898d1fb16ecaabcd091afaadeb6a9c056ca6c846e3",
        "license": "CC-BY-NC-3.0; model local use remains subject to noncommercial terms",
        "config": "xmodels/str/parseq/strhub/models/parseq/system.py",
        "input": ("image", [3, 32, 128]),
        "outputs": [("logits", [3, 95])],
        "preprocessing": {
            "id": "parseq-rgb-bicubic-v1", "color": "rgb", "input_range": [-1, 1],
            "interpolation": "bicubic", "align_corners": False, "antialias": True,
            "cubic_coefficient": -0.5, "resize_coordinate_mode": "half_pixel",
            "quantize_uint8_after_resize": True, "max_length": 2, "eos_index": 0,
        },
    },
    "action": {
        "model_id": "stgcnpp-coco2d-joint-ntu60",
        "model_family": "stgcnpp",
        "url": "https://download.openmmlab.com/mmaction/v1.0/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d_20221228-86e1e77a.pth",
        "sha256": "86e1e77a7b27dd53c6ccc1600a4cc3c321ec8e04d17f25a41fbbb7896026945a",
        "license": "Apache-2.0 (code); retain upstream checkpoint and NTU60 training-data provenance",
        "config": "openmm/mmaction2/configs/skeleton/stgcnpp/stgcnpp_8xb16-joint-u100-80e_ntu60-xsub-keypoint-2d.py",
        "input": ("skeleton", [2, 100, 17, 3]),
        "outputs": [("logits", [60])],
        "preprocessing": {
            "id": "stgcnpp-coco17-causal-v1", "coordinate_normalization": "full_canvas_minus1_plus1",
            "sample_period_ns": 100000000, "max_gap_ns": 150000000,
            "samples": 100, "second_person": "zeros",
        },
    },
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else _old_digest(stream)


def _old_digest(stream) -> str:
    value = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        value.update(block)
    return value.hexdigest()


def json_write(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def relative_file(root: Path, name: str) -> Path:
    child = root / name
    if not name or Path(name).is_absolute() or child.is_symlink() or not child.is_file():
        raise ValueError(f"Invalid bundle file: {name!r}")
    if child.resolve().parent != root.resolve():
        raise ValueError(f"Bundle file escapes its directory: {name!r}")
    return child


@contextlib.contextmanager
def publication(destination: Path):
    destination = destination.expanduser().absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise ValueError(f"Bundle already exists: {destination}; choose a new immutable destination")
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}.", dir=destination.parent))
    try:
        yield staging
        for file in staging.rglob("*"):
            if file.is_file():
                with file.open("rb") as stream:
                    os.fsync(stream.fileno())
        # The output is a new immutable generation. Never merge into an existing bundle.
        if destination.exists():
            raise ValueError(f"Bundle appeared during preparation: {destination}")
        libc = ctypes.CDLL(None, use_errno=True)
        rename = libc.renameat2
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(staging), -100, os.fsencode(destination), 1) != 0:
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code), str(destination))
        directory = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def checkpoint(args, profile: dict, staging: Path) -> Path:
    if args.checkpoint:
        path = Path(args.checkpoint).expanduser().resolve(strict=True)
    else:
        if args.feature == "jersey":
            raise ValueError("Jersey export requires the explicitly supplied local hockey checkpoint; see its model license")
        path = staging / "checkpoint.pth"
        with urllib.request.urlopen(profile["url"], timeout=60) as source, path.open("wb") as output:
            shutil.copyfileobj(source, output, length=1024 * 1024)
    if digest(path) != profile["sha256"]:
        raise ValueError("Checkpoint SHA256 does not match this supported model profile")
    return path


def source_paths(root: Path) -> None:
    # Use one explicit source checkout, avoiding an unrelated installed MMPose/MMAction.
    paths = [root, *[root / "openmm" / x for x in ("mmengine", "mmcv", "mmdetection", "mmpose", "mmaction2")],
             root / "xmodels/str/parseq"]
    for path in reversed(paths):
        if path.is_dir():
            sys.path.insert(0, str(path))


def load_weights(path: Path, feature: str):
    import numpy as np
    import torch
    if feature == "jersey":
        # This exact SHA-pinned author checkpoint embeds legacy optimizer/scheduler
        # pickle classes. It is converted offline only, never loaded by playback.
        if digest(path) != PROFILES["jersey"]["sha256"]:
            raise ValueError("Legacy pickle conversion is restricted to the pinned author checkpoint")
        return torch.load(path, map_location="cpu", weights_only=False)
    allowed = [(np.core.multiarray._reconstruct, "numpy.core.multiarray._reconstruct"), np.ndarray, np.dtype]
    for dtype in (np.float64, np.float32, np.uint8, np.int32, np.int64):
        allowed.append(type(np.dtype(dtype)))
    with torch.serialization.safe_globals(allowed):
        return torch.load(path, map_location="cpu", weights_only=True)


@contextlib.contextmanager
def explicit_encoder_attention(encoder):
    """Export equivalent q*scale/matmul attention, preserving the upstream oracle.

    TensorRT10.3 misoptimizes the ONNX split-q/k scaling emitted for timm's
    scaled_dot_product_attention branch, including with FP16 and TF32 disabled.
    Its explicit attention branch has the same weights/math and passes the
    unchanged PyTorch/ONNX/TensorRT gates. Scope the switch to the wrapper call:
    the original model reference must retain its independently executed branch.
    This is a serialized offline export operation, never a playback model.
    """
    modes = [(block.attn, block.attn.fused_attn) for block in encoder.blocks
             if hasattr(block.attn, "fused_attn")]
    try:
        for attention, _ in modes:
            attention.fused_attn = False
        yield
    finally:
        for attention, mode in modes:
            attention.fused_attn = mode


def create_model(feature: str, root: Path, path: Path, profile: dict):
    import torch
    state = load_weights(path, feature)
    extra = {}
    if feature == "jersey":
        from strhub.models.parseq.system import PARSeq
        parameters = state["hyper_parameters"]
        expected = {"img_size": [32, 128], "decode_ar": True, "refine_iters": 1, "max_label_length": 25}
        for name, value in expected.items():
            if parameters.get(name) != value:
                raise ValueError(f"Unexpected PARSeq {name}")
        model = PARSeq(**parameters).eval()
        model.load_state_dict(state["state_dict"], strict=True)
        vocabulary = list(model.tokenizer._itos[:-2])
        if len(vocabulary) != 95 or vocabulary[:11] != ["[E]", *list("0123456789")]:
            raise ValueError("Unexpected PARSeq tokenizer")
        extra["charset"] = vocabulary
        extra["training_hyperparameters"] = parameters

        class ShortPARSeq(torch.nn.Module):
            """Original AR+one-refinement inference with a fixed two-character limit.

            Select the known singleton token axis explicitly. Even squeeze(1)
            exports a rank-changing ONNX If when that axis is not inferred;
            TensorRT 10.3 rejects those conditional output ranks.
            """
            def __init__(self, module):
                super().__init__()
                self.model = module

            def forward(self, images):
                module = self.model
                with explicit_encoder_attention(module.encoder):
                    memory = module.encode(images)
                batch = images.shape[0]
                steps = 3
                queries = module.pos_queries[:, :steps].expand(batch, -1, -1)
                mask = torch.triu(torch.full((steps, steps), float("-inf"), device=images.device), 1)
                target = torch.full((batch, steps), module.pad_id, dtype=torch.long, device=images.device)
                target[:, 0] = module.bos_id
                logits = []
                for index in range(steps):
                    end = index + 1
                    decoded = module.decode(target[:, :end], memory, mask[:end, :end],
                                            tgt_query=queries[:, index:end], tgt_query_mask=mask[index:end, :end])
                    prediction = module.head(decoded)
                    logits.append(prediction)
                    if end < steps:
                        target[:, end] = prediction[:, 0, :].argmax(-1)
                logits = torch.cat(logits, dim=1)
                query_mask = mask.clone()
                query_mask[torch.triu(torch.ones(steps, steps, dtype=torch.bool, device=images.device), 2)] = 0
                bos = torch.full((batch, 1), module.bos_id, dtype=torch.long, device=images.device)
                target = torch.cat([bos, logits[:, :-1].argmax(-1)], dim=1)
                padding = (target == module.eos_id).int().cumsum(-1) > 0
                decoded = module.decode(target, memory, mask, padding, tgt_query=queries,
                                        tgt_query_mask=query_mask[:, :target.shape[1]])
                return module.head(decoded)

        wrapped = ShortPARSeq(model).eval()
        return wrapped, lambda x: model(x, max_length=2), extra

    from mmengine import Config
    config = Config.fromfile(str(root / profile["config"]))
    if feature == "pose":
        from mmpose.registry import MODELS
        from mmpose.utils import register_all_modules
    else:
        from mmaction.registry import MODELS
        from mmaction.utils import register_all_modules
    register_all_modules(init_default_scope=True)
    model = MODELS.build(config.model).eval()
    model.load_state_dict(state["state_dict"], strict=True)

    class Network(torch.nn.Module):
        def __init__(self, module):
            super().__init__()
            self.backbone = module.backbone
            self.head = module.head if feature == "pose" else module.cls_head
        def forward(self, inputs):
            return self.head(self.backbone(inputs))

    if feature == "action":
        labels_file = root / "openmm/mmaction2/tools/data/skeleton/label_map_ntu60.txt"
        labels = labels_file.read_text(encoding="utf-8").splitlines()
        if len(labels) != 60 or labels[42] != "falling":
            raise ValueError("Unexpected NTU60 label map")
        extra["labels"] = labels
    wrapped = Network(model).eval()
    return wrapped, wrapped, extra


def output_arrays(value):
    if not isinstance(value, (tuple, list)):
        value = [value]
    return [x.detach().cpu().numpy() for x in value]


def export_bundle(args) -> None:
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    torch.set_num_threads(min(8, os.cpu_count() or 1))
    profile = PROFILES[args.feature]
    root = Path(args.hm_root).expanduser().resolve(strict=True)
    source_paths(root)
    validation = Path(args.validation_inputs).expanduser().resolve(strict=True)
    with np.load(validation, allow_pickle=False) as data:
        inputs = np.asarray(data["inputs"], dtype=np.float32)
    suffix = profile["input"][1]
    if list(inputs.shape[1:]) != suffix or not 1 <= inputs.shape[0] <= 256 or not np.isfinite(inputs).all():
        raise ValueError(f"Validation NPZ must contain finite inputs[N,{suffix}], 1<=N<=256")
    if args.feature == "action" and np.any(inputs[:, 1] != 0):
        raise ValueError("This per-player action profile requires zero second-person slots")
    with publication(Path(args.output)) as directory:
        ckpt = checkpoint(args, profile, directory)
        model, reference, extra = create_model(args.feature, root, ckpt, profile)
        input_name = profile["input"][0]
        output_names = [name for name, _ in profile["outputs"]]
        onnx_path = directory / "model.onnx"
        exemplar = torch.from_numpy(np.stack([inputs[i % len(inputs)] for i in range(min(2, args.max_batch))]))
        with torch.inference_mode():
            torch.onnx.export(model, exemplar, str(onnx_path), input_names=[input_name], output_names=output_names,
                              opset_version=17, dynamo=False, external_data=False,
                              dynamic_axes={name: {0: "batch"} for name in [input_name, *output_names]})
        graph = onnx.load(onnx_path)
        onnx.checker.check_model(graph)
        session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        if [x.name for x in session.get_inputs()] != [input_name] or [x.name for x in session.get_outputs()] != output_names:
            raise ValueError("ONNX tensor contract changed during export")
        cases = []
        for batch in sorted({1, min(2, args.max_batch), args.max_batch}):
            value = np.ascontiguousarray(np.stack([inputs[i % len(inputs)] for i in range(batch)]))
            tensor = torch.from_numpy(value)
            with torch.inference_mode():
                expected = output_arrays(reference(tensor))
                wrapped = output_arrays(model(tensor))
            actual = session.run(None, {input_name: value})
            case = {"batch": batch, "input": f"input_b{batch}.f32", "outputs": []}
            value.tofile(directory / case["input"])
            case["input_sha256"] = digest(directory / case["input"])
            for index, (name, dimensions) in enumerate(profile["outputs"]):
                shape = [batch, *dimensions]
                if list(expected[index].shape) != shape or list(actual[index].shape) != shape:
                    raise ValueError(f"Output shape mismatch: {name}")
                for candidate in (wrapped[index], actual[index]):
                    if not np.isfinite(candidate).all() or not np.allclose(candidate, expected[index], atol=1e-4, rtol=5e-4):
                        raise ValueError(f"PyTorch/export parity failed for {name}, batch {batch}")
                filename = f"reference_b{batch}_{index}.f32"
                expected[index].astype(np.float32).tofile(directory / filename)
                case["outputs"].append({"name": name, "file": filename, "shape": shape,
                                         "sha256": digest(directory / filename),
                                         "onnx_max_abs_error": float(np.abs(actual[index] - expected[index]).max())})
            cases.append(case)
        preprocessing = dict(profile["preprocessing"])
        if "charset" in extra:
            preprocessing["charset"] = extra["charset"]
        manifest = {
            "schema_version": 1, "feature": args.feature, "model_family": profile["model_family"],
            "model_id": profile["model_id"], "max_batch": args.max_batch,
            "source": {"url": profile["url"], "sha256": profile["sha256"], "license": profile["license"],
                       "config_sha256": digest(root / profile["config"]),
                       "exporter_sha256": digest(Path(__file__)),
                       "torch_version": str(torch.__version__)},
            "onnx": {"file": "model.onnx", "sha256": digest(onnx_path), "opset": 17},
            "inputs": [{"name": input_name, "dtype": "float32", "shape": [-1, *suffix]}],
            "outputs": [{"name": name, "dtype": "float32", "shape": [-1, *dims]} for name, dims in profile["outputs"]],
            "preprocessing": preprocessing,
            "validation": {"inputs_sha256": digest(validation), "cases": cases,
                           "description": args.validation_description,
                           "onnx_atol": 1e-4, "onnx_rtol": 5e-4},
        }
        if args.feature in ("pose", "action"):
            manifest["topology_id"] = "coco17"
        if "labels" in extra:
            manifest["labels"] = extra["labels"]
        if "training_hyperparameters" in extra:
            manifest["source"]["training_hyperparameters"] = extra["training_hyperparameters"]
        if ckpt.parent == directory:
            ckpt.unlink()
        json_write(directory / "export.json", manifest)
    print(f"Exported {args.feature}: {Path(args.output).absolute()}")


def native_json(command: list[str]) -> dict:
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.stderr:
        sys.stderr.write(result.stderr)
    if result.returncode:
        # A failed native process may put parser/runtime details on either stream.
        # Preserve them before check_returncode raises the concise command error.
        if result.stdout:
            sys.stderr.write(result.stdout)
        result.check_returncode()
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        if result.stdout:
            sys.stderr.write(result.stdout)
        raise ValueError("Native model builder did not return its JSON contract") from error


def verify_deepstream(builder: Path, library: Path) -> dict:
    info = native_json([str(builder), "--runtime-info"])
    dependencies = subprocess.run(["readelf", "-d", str(library)], check=True, capture_output=True, text=True).stdout
    match = re.search(r"NEEDED.*\[libnvinfer\.so\.(\d+)\]", dependencies)
    if not match or str(info["tensorrt_version"]).split(".")[0] != match[1]:
        raise ValueError("Native builder TensorRT major does not match the actual DeepStream inference library")
    return info


def build_bundle(args) -> None:
    import numpy as np
    source = Path(args.export_dir).expanduser().resolve(strict=True)
    manifest = json.loads(relative_file(source, "export.json").read_text(encoding="utf-8"))
    feature = manifest.get("feature")
    if manifest.get("schema_version") != 1 or feature not in PROFILES:
        raise ValueError("Unsupported export manifest")
    onnx_path = relative_file(source, manifest["onnx"]["file"])
    if digest(onnx_path) != manifest["onnx"]["sha256"]:
        raise ValueError("Exported ONNX checksum mismatch")
    builder = Path(args.builder).expanduser().resolve(strict=True)
    identity = verify_deepstream(builder, Path(args.deepstream_library).expanduser().resolve(strict=True))
    max_batch = manifest["max_batch"]
    if not isinstance(max_batch, int) or not 1 <= max_batch <= 8:
        raise ValueError("Invalid export batch profile")
    with publication(Path(args.output)) as directory:
        shutil.copyfile(onnx_path, directory / "model.onnx")
        engine_path = directory / "model.engine"
        native_json([str(builder), "--onnx", str(directory / "model.onnx"), "--engine", str(engine_path),
                     "--precision", args.precision, "--max-batch", str(max_batch)])
        reports = []
        cases = manifest["validation"]["cases"]
        if {x["batch"] for x in cases} != {1, min(2, max_batch), max_batch}:
            raise ValueError("Export validation must cover minimum, optimum and maximum batch")
        for case in cases:
            input_path = relative_file(source, case["input"])
            if digest(input_path) != case["input_sha256"]:
                raise ValueError("Validation input checksum mismatch")
            with tempfile.TemporaryDirectory(prefix="player-engine-validation-") as temporary:
                output_directory = Path(temporary)
                result = native_json([str(builder), "--engine", str(engine_path), "--input", str(input_path),
                                      "--batch", str(case["batch"]), "--outputs", str(output_directory)])
                if result["runtime"] != identity:
                    raise ValueError("GPU/runtime identity changed during model preparation")
                by_name = {x["name"]: x for x in result["outputs"]}
                if set(by_name) != {x["name"] for x in case["outputs"]}:
                    raise ValueError("Engine output names differ from ONNX")
                for reference in case["outputs"]:
                    path = relative_file(source, reference["file"])
                    if digest(path) != reference["sha256"]:
                        raise ValueError("Validation reference checksum mismatch")
                    output = by_name[reference["name"]]
                    if output["shape"] != reference["shape"] or output["dtype"] != "float32":
                        raise ValueError("Engine output shape/dtype mismatch")
                    expected = np.fromfile(path, dtype=np.float32).reshape(reference["shape"])
                    actual = np.fromfile(relative_file(output_directory, output["file"]), dtype=np.float32).reshape(reference["shape"])
                    atol, rtol = ((0.08, 0.03) if args.precision == "fp16" else (1e-4, 5e-4))
                    if not np.isfinite(actual).all() or not np.allclose(actual, expected, atol=atol, rtol=rtol):
                        raise ValueError(f"TensorRT numeric parity failed for {reference['name']} batch {case['batch']}")
                    flat_expected = expected.reshape(-1, expected.shape[-1])
                    flat_actual = actual.reshape(flat_expected.shape)
                    ordered = np.sort(flat_expected, axis=-1)
                    # Near ties can legitimately change under FP16. A high-margin
                    # decision must survive conversion; report all changes below.
                    stable = ordered[:, -1] - ordered[:, -2] > 2 * atol
                    equal = flat_expected.argmax(-1) == flat_actual.argmax(-1)
                    if np.any(stable & ~equal):
                        raise ValueError("TensorRT changes a high-margin reference decision")
                    reports.append({"batch": case["batch"], "name": reference["name"],
                                    "max_abs_error": float(np.abs(actual - expected).max()),
                                    "argmax_agreement": float(equal.mean()), "atol": atol, "rtol": rtol})
        manifest["engine"] = {"file": "model.engine", "sha256": digest(engine_path), "max_batch": max_batch,
                              "precision": args.precision, **identity}
        manifest["onnx"]["file"] = "model.onnx"
        manifest["validation"]["engine_results"] = reports
        # Export references are provenance of the completed checks, not runtime dependencies.
        manifest["validation"]["export_manifest_sha256"] = digest(source / "export.json")
        preparation = {"validation": manifest.pop("validation"), "max_batch": manifest.pop("max_batch"),
                       "source_details": {key: manifest["source"].pop(key) for key in
                                          ("exporter_sha256", "torch_version", "training_hyperparameters")
                                          if key in manifest["source"]}}
        json_write(directory / "manifest.json", manifest)
        preparation["manifest_sha256"] = digest(directory / "manifest.json")
        json_write(directory / "preparation.json", preparation)
    print(f"Prepared {feature}: {Path(args.output).absolute() / 'manifest.json'}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    export = commands.add_parser("export", help="Export pinned weights and validate PyTorch/ONNX on supplied tensors")
    export.add_argument("--feature", choices=PROFILES, required=True)
    export.add_argument("--hm-root", required=True)
    export.add_argument("--checkpoint", help="Local pinned checkpoint (required for jersey)")
    export.add_argument("--validation-inputs", required=True, help="NPZ containing normalized float32 inputs[N,...]")
    export.add_argument("--validation-description", required=True, help="Source/meaning of the validation tensors")
    export.add_argument("--max-batch", type=int, default=8)
    export.add_argument("--output", required=True, help="New immutable export directory")
    build = commands.add_parser("build", help="Build on the playback GPU and validate native engine against export references")
    build.add_argument("--export-dir", required=True)
    build.add_argument("--builder", required=True)
    build.add_argument("--precision", choices=("fp16", "fp32"), default="fp16")
    build.add_argument("--deepstream-library", default="/opt/nvidia/deepstream/deepstream/lib/libnvds_infer.so")
    build.add_argument("--output", required=True, help="New immutable prepared bundle directory")
    args = parser.parse_args()
    if args.command == "export":
        if not 1 <= args.max_batch <= 8:
            parser.error("--max-batch must be between 1 and 8")
        export_bundle(args)
    else:
        build_bundle(args)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"prepare_player_models: {error}")
