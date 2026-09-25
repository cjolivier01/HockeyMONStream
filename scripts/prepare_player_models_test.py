#!/usr/bin/env python3
"""CPU preparation coverage/failure checks; actual model parity uses export/build."""
import importlib.util
import json
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("prepare_player_models", Path(__file__).with_name("prepare_player_models.py"))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class EncoderAttentionTest(unittest.TestCase):
    def test_export_switch_does_not_change_upstream_reference(self):
        attention = [SimpleNamespace(fused_attn=True), SimpleNamespace(fused_attn=False), SimpleNamespace()]
        encoder = SimpleNamespace(blocks=[SimpleNamespace(attn=mode) for mode in attention])
        with prepare.explicit_encoder_attention(encoder):
            self.assertFalse(attention[0].fused_attn)
            self.assertFalse(attention[1].fused_attn)
            self.assertFalse(hasattr(attention[2], "fused_attn"))
        self.assertTrue(attention[0].fused_attn)
        self.assertFalse(attention[1].fused_attn)
        self.assertFalse(hasattr(attention[2], "fused_attn"))

    def test_export_failure_restores_attention_mode(self):
        attention = SimpleNamespace(fused_attn=True)
        encoder = SimpleNamespace(blocks=[SimpleNamespace(attn=attention)])
        with self.assertRaisesRegex(RuntimeError, "export failed"):
            with prepare.explicit_encoder_attention(encoder):
                raise RuntimeError("export failed")
        self.assertTrue(attention.fused_attn)


class PublicationTest(unittest.TestCase):
    def test_native_failure_retains_both_diagnostic_streams(self):
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics), self.assertRaises(subprocess.CalledProcessError) as failure:
            prepare.native_json([
                sys.executable, "-c",
                "import sys; print('parser diagnostic'); print('runtime diagnostic', file=sys.stderr); sys.exit(7)",
            ])
        self.assertEqual(failure.exception.returncode, 7)
        self.assertIn("parser diagnostic", diagnostics.getvalue())
        self.assertIn("runtime diagnostic", diagnostics.getvalue())

    def test_invalid_native_json_retains_stdout(self):
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics), self.assertRaisesRegex(ValueError, "JSON contract"):
            prepare.native_json([sys.executable, "-c", "print('unexpected native output')"])
        self.assertIn("unexpected native output", diagnostics.getvalue())

    def test_failure_never_publishes_partial_bundle(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / "bundle"
            with self.assertRaisesRegex(RuntimeError, "conversion failed"):
                with prepare.publication(output) as staging:
                    (staging / "model.engine").write_bytes(b"partial")
                    raise RuntimeError("conversion failed")
            self.assertFalse(output.exists())
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_racing_destination_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / "bundle"
            with self.assertRaises(ValueError):
                with prepare.publication(output) as staging:
                    (staging / "model.engine").write_bytes(b"new")
                    output.mkdir()
                    (output / "owned").write_text("existing")
            self.assertEqual((output / "owned").read_text(), "existing")
            self.assertEqual(list(output.iterdir()), [output / "owned"])

    def test_existing_generation_is_immutable(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / "bundle"
            with prepare.publication(output) as staging:
                (staging / "model.engine").write_bytes(b"complete")
            with self.assertRaises(ValueError):
                with prepare.publication(output):
                    self.fail("entered an existing generation")
            self.assertEqual((output / "model.engine").read_bytes(), b"complete")

    def test_rejects_symlinks_absolute_paths_and_parent_escape(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "bundle"
            directory.mkdir()
            outside = Path(root) / "external"
            outside.write_bytes(b"secret")
            (directory / "link").symlink_to(outside)
            for name in ("link", "../external", str(outside), "missing"):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    prepare.relative_file(directory, name)
            (directory / "model.engine").write_bytes(b"engine")
            self.assertEqual(prepare.relative_file(directory, "model.engine"), directory / "model.engine")


class ValidationCoverageTest(unittest.TestCase):
    @staticmethod
    def manifest(input_count=19, maximum_batch=8):
        return {"input_count": input_count, "cases": [
            {"batch": batch, "input": f"input_{suffix}.f32", "input_indices": indices,
             "outputs": [{"file": f"reference_{suffix}_0.f32"}]}
            for batch, suffix, indices in prepare.validation_batches(input_count, maximum_batch)
        ]}

    def test_every_supported_count_and_batch_covers_all_rows(self):
        for maximum in range(1, 9):
            for count in range(1, 257):
                with self.subTest(maximum=maximum, count=count):
                    batches = prepare.validation_batches(count, maximum)
                    self.assertEqual({i for _, _, indices in batches for i in indices}, set(range(count)))
                    self.assertTrue({1, min(2, maximum), maximum}.issubset({b for b, _, _ in batches}))
                    self.assertEqual(len({name for _, name, _ in batches}), len(batches))
                    self.assertTrue(all(1 <= b <= maximum and len(indices) == b for b, _, indices in batches))
                    prepare.validation_cases(self.manifest(count, maximum), maximum)

    def test_boundary_names_and_partial_tail_are_preserved(self):
        batches = prepare.validation_batches(19, 8)
        self.assertEqual([name for _, name, _ in batches], ["b1", "b2", "b8", "b8_from8", "b3_from16"])
        self.assertEqual(batches[-1], (3, "b3_from16", [16, 17, 18]))
        self.assertEqual(prepare.validation_batches(1, 8)[-1][2], [0] * 8)

    def test_missing_late_rows_are_rejected(self):
        validation = self.manifest()
        validation["cases"].pop()
        with self.assertRaisesRegex(ValueError, "omits supplied input rows"):
            prepare.validation_cases(validation, 8)

    def test_missing_boundary_case_and_invalid_extra_batch_are_rejected(self):
        validation = self.manifest()
        validation["cases"].pop(1)
        with self.assertRaisesRegex(ValueError, "minimum, optimum and maximum"):
            prepare.validation_cases(validation, 8)
        validation = self.manifest()
        validation["cases"][-1]["batch"] = 9
        with self.assertRaisesRegex(ValueError, "outside the export profile"):
            prepare.validation_cases(validation, 8)

    def test_duplicate_case_files_and_invalid_indices_are_rejected(self):
        validation = self.manifest()
        validation["cases"][-1]["input"] = validation["cases"][0]["input"]
        with self.assertRaisesRegex(ValueError, "unique input/reference filenames"):
            prepare.validation_cases(validation, 8)
        validation = self.manifest()
        validation["cases"][-1]["input_indices"][-1] = 19
        with self.assertRaisesRegex(ValueError, "Invalid validation input indices"):
            prepare.validation_cases(validation, 8)

    def test_legacy_boundary_exports_remain_usable(self):
        validation = self.manifest(3, 8)
        validation.pop("input_count")
        for case in validation["cases"]:
            case.pop("input_indices")
        self.assertEqual(len(prepare.validation_cases(validation, 8)), 3)


class NativeCoverageReplayTest(unittest.TestCase):
    def setUp(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("native preparation CPU tests require NumPy")
        self.np = np

    def test_export_executes_all_rows_and_preserves_model_shape(self):
        np = self.np
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = np.arange(38, dtype=np.float32).reshape(19, 2)
            np.savez(root / "inputs.npz", inputs=inputs)
            (root / "config.py").write_text("CPU export fixture")
            checkpoint = root / "weights"
            checkpoint.write_bytes(b"CPU weights stand-in")
            profile = {"input": ("image", [2]), "outputs": [("logits", [2])], "preprocessing": {},
                       "model_id": "CPU fixture", "model_family": "fixture", "url": "fixture",
                       "sha256": prepare.digest(checkpoint), "license": "fixture", "config": "config.py"}
            calls = {"reference": [], "wrapper": [], "onnx": []}

            class Tensor:
                def __init__(self, array):
                    self.array = array
                def detach(self):
                    return self
                def cpu(self):
                    return self
                def numpy(self):
                    return self.array

            def model(name):
                def run(value):
                    calls[name].append(value.array[:, 0].astype(int).tolist())
                    return value
                return run

            class Session:
                def get_inputs(self):
                    return [SimpleNamespace(name="image")]
                def get_outputs(self):
                    return [SimpleNamespace(name="logits")]
                def run(self, _, bindings):
                    value = bindings["image"]
                    calls["onnx"].append(value[:, 0].astype(int).tolist())
                    return [value]

            torch = SimpleNamespace(set_num_threads=lambda _: None, from_numpy=Tensor,
                                    inference_mode=contextlib.nullcontext, __version__="CPU fixture",
                                    onnx=SimpleNamespace(export=lambda _model, _input, path, **kwargs:
                                                         Path(path).write_bytes(b"CPU ONNX stand-in")))
            onnx = SimpleNamespace(load=lambda _: None, checker=SimpleNamespace(check_model=lambda _: None))
            ort = SimpleNamespace(InferenceSession=lambda *args, **kwargs: Session())
            args = SimpleNamespace(feature="pose", hm_root=root, validation_inputs=root / "inputs.npz",
                                   output=root / "export", max_batch=8, validation_description="19 CPU examples")
            with mock.patch.dict(sys.modules, torch=torch, onnx=onnx, onnxruntime=ort), \
                    mock.patch.dict(prepare.PROFILES, pose=profile), \
                    mock.patch.object(prepare, "source_paths"), \
                    mock.patch.object(prepare, "checkpoint", return_value=checkpoint), \
                    mock.patch.object(prepare, "create_model", return_value=(model("wrapper"), model("reference"), {})), \
                    contextlib.redirect_stdout(io.StringIO()):
                prepare.export_bundle(args)
            for route in calls.values():
                self.assertEqual([len(batch) for batch in route], [1, 2, 8, 8, 3])
                self.assertEqual({value for batch in route for value in batch}, set(range(0, 38, 2)))
            manifest = json.loads((args.output / "export.json").read_text())
            self.assertEqual(manifest["inputs"][0]["shape"], [-1, 2])
            self.assertEqual(manifest["validation"]["input_count"], 19)
            for case in prepare.validation_cases(manifest["validation"], 8):
                actual = np.fromfile(args.output / case["input"], dtype=np.float32).reshape(case["batch"], 2)
                np.testing.assert_array_equal(actual, inputs[case["input_indices"]])

    def build(self, root, fail_after_first_batch=False):
        np = self.np
        source = root / "export"
        source.mkdir()
        (source / "model.onnx").write_bytes(b"CPU-test ONNX stand-in")
        validation = ValidationCoverageTest.manifest()
        for case in validation["cases"]:
            values = np.asarray([[float(i), float(i) + 0.5] for i in case["input_indices"]], dtype=np.float32)
            values.tofile(source / case["input"])
            case["input_sha256"] = prepare.digest(source / case["input"])
            output = case["outputs"][0]
            values.tofile(source / output["file"])
            output.update(name="logits", shape=list(values.shape), sha256=prepare.digest(source / output["file"]))
        manifest = {"schema_version": 1, "feature": "pose", "source": {}, "max_batch": 8,
                    "onnx": {"file": "model.onnx", "sha256": prepare.digest(source / "model.onnx")},
                    "validation": validation}
        prepare.json_write(source / "export.json", manifest)
        builder = root / "builder"
        builder.touch()
        library = root / "deepstream.so"
        library.touch()
        identity = {"test_identity": "CPU-only"}
        calls = []

        def native(command):
            if "--onnx" in command:
                Path(command[command.index("--engine") + 1]).write_bytes(b"CPU-test engine stand-in")
                return {}
            batch = int(command[command.index("--batch") + 1])
            path = Path(command[command.index("--input") + 1])
            values = np.fromfile(path, dtype=np.float32).reshape(batch, 2)
            calls.append((batch, values[:, 0].astype(int).tolist()))
            if fail_after_first_batch and np.any(values[:, 0] >= 8):
                values = values + 1
            directory = Path(command[command.index("--outputs") + 1])
            values.tofile(directory / "actual.f32")
            return {"runtime": identity, "outputs": [
                {"name": "logits", "file": "actual.f32", "shape": [batch, 2], "dtype": "float32"}
            ]}

        args = SimpleNamespace(export_dir=source, builder=builder, deepstream_library=library,
                               precision="fp16", output=root / "prepared")
        with mock.patch.object(prepare, "verify_deepstream", return_value=identity), \
                mock.patch.object(prepare, "native_json", side_effect=native), contextlib.redirect_stdout(io.StringIO()):
            if fail_after_first_batch:
                with self.assertRaisesRegex(ValueError, "TensorRT numeric parity failed"):
                    prepare.build_bundle(args)
            else:
                prepare.build_bundle(args)
        return args.output, calls

    def test_native_preparation_replays_every_case_including_partial_tail(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, calls = self.build(Path(temporary))
            self.assertEqual([batch for batch, _ in calls], [1, 2, 8, 8, 3])
            self.assertEqual({i for _, indices in calls for i in indices}, set(range(19)))
            report = json.loads((destination / "preparation.json").read_text())
            results = report["validation"]["engine_results"]
            self.assertEqual(len(results), 5)
            self.assertEqual(len({x["input"] for x in results}), 5)

    def test_error_only_after_first_maximum_batch_prevents_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination, calls = self.build(root, fail_after_first_batch=True)
            self.assertEqual([batch for batch, _ in calls], [1, 2, 8, 8])
            self.assertEqual(calls[-1][1], list(range(8, 16)))
            self.assertFalse(destination.exists())
            self.assertFalse(any(path.name.startswith(".prepared.") for path in root.iterdir()))


if __name__ == "__main__":
    unittest.main()
