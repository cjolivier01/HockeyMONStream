#!/usr/bin/env python3
"""Non-GPU preparation failure-path checks; model parity is exercised by export/build."""
import importlib.util
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
