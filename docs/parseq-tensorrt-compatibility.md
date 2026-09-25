# PARSeq export compatibility with TensorRT10.3

PARSeq's offline exporter must select timm's explicit encoder attention branch
while tracing the model. TensorRT10.3 on Orin can parse the ONNX graph emitted by
its fused PyTorch `scaled_dot_product_attention` branch yet produce incorrect
encoder values, even in FP32 with TF32 disabled. The export uses the equivalent
`q * scale -> QK matmul -> softmax -> V matmul` branch with the same trained weights.
Playback still runs a normal prepared TensorRT engine and requires no Python or
custom TensorRT plugin.

The switch is scoped to the export wrapper's encoder call and restored in a
`finally` block. The independent upstream PARSeq reference retains its original
attention branch. Existing PyTorch/wrapper/ONNX checks, TensorRT numeric tolerances,
and high-margin decision checks remain unchanged. Do not work around a preparation
failure by widening tolerances or publishing an unchecked engine.

The autoregressive singleton prediction axis must also be selected with
`prediction[:, 0, :]`; `squeeze(1)` can export rank-changing ONNX `If` branches
that TensorRT10.3 rejects. Encoder attention compatibility and this indexing rule
solve separate issues. Keep both while preserving the original autoregressive plus
one-refinement behavior.

Validation used the pinned hockey checkpoint and batches1,2,8, with three synthetic
inputs and one retained real jersey27 crop. PyTorch and ONNX remain within
`atol=1e-4, rtol=5e-4`. Target-local preparation checks the actual SDK/GPU and uses
its existing FP32 (`1e-4,5e-4`) or FP16 (`0.08,0.03`) gates before atomically
publishing a new immutable bundle. These checks establish operator/conversion
parity for the fixtures, not hockey recognition accuracy.
