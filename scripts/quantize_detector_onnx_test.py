import argparse
from pathlib import Path
import tempfile
import unittest

try:
  import quantize_detector_onnx as quantizer
except ModuleNotFoundError:
  from scripts import quantize_detector_onnx as quantizer

try:
  import cv2
  import numpy as np
  import onnx
  import onnxruntime as ort
except ImportError:
  onnx = None


class QuantizationTest(unittest.TestCase):
  def test_image_list_rejects_missing_and_empty_inputs(self):
    with tempfile.TemporaryDirectory() as root:
      path = Path(root) / "images.txt"
      path.write_text("\n")
      with self.assertRaisesRegex(ValueError, "empty"):
        quantizer.image_paths(path)
      path.write_text("missing.png\n")
      with self.assertRaisesRegex(ValueError, "does not exist"):
        quantizer.image_paths(path)

  @unittest.skipIf(onnx is None, "offline quantization dependencies are not installed")
  def test_rgb_symmetric_padding_and_scale(self):
    image = np.zeros((2, 4, 3), dtype=np.uint8)
    image[:, :, 2] = 255  # BGR red
    tensor = quantizer.prepare_image(image, 4, 4, 1 / 255)
    self.assertEqual(tensor.shape, (1, 3, 4, 4))
    self.assertEqual(tensor.dtype, np.float32)
    np.testing.assert_array_equal(tensor[0, 0, 1:3], np.ones((2, 4)))
    self.assertFalse(tensor[0, 1:].any())
    self.assertFalse(tensor[0, :, (0, 3)].any())

  @unittest.skipIf(onnx is None, "offline quantization dependencies are not installed")
  def test_native_samples_preserve_pixels_and_require_exact_dimensions(self):
    image = np.arange(5 * 7 * 3, dtype=np.uint8).reshape(5, 7, 3)
    actual = quantizer.prepare_image(image, 5, 7, 1 / 255, preprocessed=True)
    expected = image[:, :, ::-1].astype(np.float32).transpose(2, 0, 1)[None] / 255
    np.testing.assert_allclose(actual, expected, atol=1e-7)
    with self.assertRaises(ValueError):
      quantizer.prepare_image(image, 6, 7, 1 / 255, preprocessed=True)

  @unittest.skipIf(onnx is None, "offline quantization dependencies are not installed")
  def test_qdq_preserves_float_io_and_avoids_int32_bias_dequantization(self):
    with tempfile.TemporaryDirectory() as root:
      root = Path(root)
      tensor = onnx.TensorProto
      weights = np.full((2, 3, 3, 3), 0.02, np.float32)
      graph = onnx.helper.make_graph(
        [onnx.helper.make_node("Conv", ["image", "weights", "bias"], ["output"])], "detector",
        [onnx.helper.make_tensor_value_info("image", tensor.FLOAT, ["batch", 3, 8, 8])],
        [onnx.helper.make_tensor_value_info("output", tensor.FLOAT, ["batch", 2, 6, 6])],
        [onnx.numpy_helper.from_array(weights, "weights"),
        onnx.numpy_helper.from_array(np.array([0.1, -0.1], np.float32), "bias")],
      )
      model = onnx.helper.make_model(graph, opset_imports=[onnx.helper.make_opsetid("", 17)], ir_version=8)
      source, output = root / "source.onnx", root / "int8.onnx"
      onnx.save(model, source)
      image = np.arange(8 * 8 * 3, dtype=np.uint8).reshape(8, 8, 3)
      cv2.imwrite(str(root / "frame.png"), image)
      image_list = root / "images.txt"
      image_list.write_text("frame.png\n")
      quantizer.quantize(argparse.Namespace(onnx=source, output=output, image_list=image_list, scale=1 / 255))
      prepared = onnx.load(output)
      self.assertEqual(prepared.graph.input[0].type.tensor_type.elem_type, tensor.FLOAT)
      self.assertEqual(prepared.graph.output[0].type.tensor_type.elem_type, tensor.FLOAT)
      initializers = {value.name: value for value in prepared.graph.initializer}
      dq_nodes = [node for node in prepared.graph.node if node.op_type == "DequantizeLinear"]
      self.assertTrue(dq_nodes)
      for node in dq_nodes:
        if node.input[0] in initializers:
          self.assertEqual(initializers[node.input[0]].data_type, tensor.INT8)
      inputs = {"image": quantizer.prepare_image(image, 8, 8, 1 / 255)}
      reference = ort.InferenceSession(str(source)).run(None, inputs)[0]
      actual = ort.InferenceSession(str(output)).run(None, inputs)[0]
      np.testing.assert_allclose(actual, reference, atol=0.01)


if __name__ == "__main__":
  unittest.main()
