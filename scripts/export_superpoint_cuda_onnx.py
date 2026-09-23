#!/usr/bin/env python3
"""Convert the pinned SuperPoint/LightGlue graph to 2048-keypoint CPU/CUDA assets.

Requires onnx==1.20.1 and numpy. No PyTorch/checkpoint re-export is needed.
Input/output types and LightGlue weights are unchanged; the keypoint capacity
defaults to 2048. LightGlue coordinates use the reference's long-edge scale
after restoring source pixel centers using FLOAT image_sizes[2,4] rows
[source_width, source_height, resized_width, resized_height].
Pass --keypoints=1024 --normalization=legacy-per-axis to reproduce the original
CUDA asset.
Use --cpu-output to also save the float32 CPU graph for the same capacity.
SuperPoint runs once per camera in a serial Loop; only its convolution backbone
and heads use float16. NMS, normalization, descriptor sampling and LightGlue
remain float32. In particular, rounding NMS pooling breaks its equality tests.
"""

import argparse
import copy
import hashlib
from pathlib import Path

import onnx
from onnx import TensorProto as T
from onnx import helper as h
from onnx import numpy_helper

SOURCE_SHA256 = "228994cea8c010146fa2aef933baa3ffaa4bcdc522bc8aa560087fcff8134526"
EXTRACTOR_OUTPUTS = ["keypoints", "/extractor/Transpose_1_output_0"]
KEYPOINT_CONSTANTS = {"/extractor/Constant_78", "/extractor/Constant_99"}


def set_keypoint_capacity(model: onnx.ModelProto, keypoints: int) -> None:
    """Change only the detector TopK and descriptor reshape in the pinned graph."""
    if keypoints not in (1024, 2048):
        raise ValueError("Keypoint capacity must be 1024 or 2048")
    found = set()
    for node in model.graph.node:
        if node.name not in KEYPOINT_CONSTANTS:
            continue
        if node.op_type != "Constant" or len(node.attribute) != 1:
            raise ValueError(f"Unexpected keypoint constant: {node.name}")
        value = node.attribute[0].t
        if (
            value.data_type != T.INT64
            or list(value.dims) != [1]
            or numpy_helper.to_array(value).tolist() != [1024]
        ):
            raise ValueError(f"Unexpected keypoint constant value: {node.name}")
        array = numpy_helper.to_array(value).copy()
        array[0] = keypoints
        value.CopyFrom(numpy_helper.from_array(array, value.name))
        found.add(node.name)
    if found != KEYPOINT_CONSTANTS:
        raise ValueError("Pinned graph is missing keypoint capacity constants")
    output = model.graph.output[0]
    if (
        output.name != "keypoints"
        or output.type.tensor_type.shape.dim[1].dim_value != 1024
    ):
        raise ValueError("Unexpected keypoint output contract")
    output.type.tensor_type.shape.dim[1].dim_value = keypoints
    # Inferred intermediate shapes retain the upstream 1024 capacity. ORT will
    # infer them from the graph; changing every occurrence risks unrelated axes.
    del model.graph.value_info[:]


def save(model: onnx.ModelProto, destination: Path) -> None:
    onnx.checker.check_model(model)
    onnx.save(model, destination)
    print(hashlib.sha256(destination.read_bytes()).hexdigest(), destination)


def set_keypoint_normalization(model: onnx.ModelProto, normalization: str) -> None:
    """Use the same isotropic positional scale as the reference LightGlue model."""
    if normalization == "legacy-per-axis":
        return
    if normalization != "reference":
        raise ValueError("Unknown keypoint normalization")
    nodes = []
    replaced = set()
    for node in model.graph.node:
        if node.name == "/Div":
            if node.op_type != "Div" or list(node.input) != [
                "/Cast_1_output_0",
                "/Cast_2_output_0",
            ]:
                raise ValueError("Unexpected upstream keypoint scale")

            def constant(name, dtype, dimensions, values):
                nodes.append(
                    h.make_node(
                        "Constant",
                        [],
                        [name],
                        value=h.make_tensor(name, dtype, dimensions, values),
                    )
                )

            constant("hstream/source_indices", T.INT64, [2], [0, 1])
            constant("hstream/resized_indices", T.INT64, [2], [2, 3])
            constant("hstream/keypoint_axis", T.INT64, [1], [1])
            constant("hstream/half", T.FLOAT, [], [0.5])
            constant("hstream/two", T.FLOAT, [], [2.0])
            for kind in ("source", "resized"):
                nodes.append(
                    h.make_node(
                        "Gather",
                        ["image_sizes", f"hstream/{kind}_indices"],
                        [f"hstream/{kind}_size_flat"],
                        axis=1,
                    )
                )
                nodes.append(
                    h.make_node(
                        "Unsqueeze",
                        [f"hstream/{kind}_size_flat", "hstream/keypoint_axis"],
                        [f"hstream/{kind}_size"],
                    )
                )
            # Match Extractor.extract(), including its operation order, before
            # LightGlue normalizes each camera against its own original size.
            nodes.extend(
                [
                    h.make_node(
                        "Cast", ["keypoints"], ["hstream/keypoints_float"], to=T.FLOAT
                    ),
                    h.make_node(
                        "Div",
                        ["hstream/resized_size", "hstream/source_size"],
                        ["hstream/resize_scale"],
                    ),
                    h.make_node(
                        "Add",
                        ["hstream/keypoints_float", "hstream/half"],
                        ["hstream/pixel_centers"],
                    ),
                    h.make_node(
                        "Div",
                        ["hstream/pixel_centers", "hstream/resize_scale"],
                        ["hstream/source_centers"],
                    ),
                    h.make_node(
                        "Sub",
                        ["hstream/source_centers", "hstream/half"],
                        ["hstream/source_keypoints"],
                    ),
                    h.make_node(
                        "Div",
                        ["hstream/source_size", "hstream/two"],
                        ["hstream/source_shift"],
                    ),
                    h.make_node(
                        "Sub",
                        ["hstream/source_keypoints", "hstream/source_shift"],
                        ["hstream/centered_keypoints"],
                    ),
                    h.make_node(
                        "ReduceMax",
                        ["hstream/source_size"],
                        ["hstream/long_edge"],
                        axes=[2],
                        keepdims=1,
                    ),
                    h.make_node(
                        "Div",
                        ["hstream/long_edge", "hstream/two"],
                        ["hstream/position_scale"],
                    ),
                ]
            )
            replaced.add(node.name)
        elif node.name == "/Sub":
            if node.op_type != "Sub" or list(node.input) != [
                "/Div_output_0",
                "/Constant_6_output_0",
            ]:
                raise ValueError("Unexpected upstream keypoint shift")
            nodes.append(
                h.make_node(
                    "Div",
                    ["hstream/centered_keypoints", "hstream/position_scale"],
                    list(node.output),
                    name="hstream/normalize_keypoints",
                )
            )
            replaced.add(node.name)
        else:
            nodes.append(node)
    if replaced != {"/Div", "/Sub"}:
        raise ValueError("Pinned graph is missing keypoint normalization nodes")
    del model.graph.node[:]
    model.graph.node.extend(nodes)
    model.graph.input.append(h.make_tensor_value_info("image_sizes", T.FLOAT, [2, 4]))


def convert(
    source: Path,
    destination: Path,
    keypoints: int = 2048,
    cpu_output: Path | None = None,
    normalization: str = "reference",
) -> None:
    if onnx.__version__ != "1.20.1":
        raise ValueError("Use onnx==1.20.1 for a reproducible serialization")
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("Input is not the pinned upstream SuperPoint/LightGlue graph")
    if destination.resolve() == source.resolve() or (
        cpu_output is not None
        and cpu_output.resolve() in (source.resolve(), destination.resolve())
    ):
        raise ValueError(
            "Source, CUDA destination and CPU destination must be distinct"
        )
    model = onnx.load(source)
    set_keypoint_capacity(model, keypoints)
    set_keypoint_normalization(model, normalization)
    if cpu_output is not None:
        h.set_model_props(
            model,
            {
                "hstream.source_sha256": SOURCE_SHA256,
                "hstream.cpu_export": "superpoint-fp32-v1",
                "hstream.superpoint_keypoints": str(keypoints),
                "hstream.keypoint_normalization": normalization,
            },
        )
        save(model, cpu_output)
    producer = {v: i for i, node in enumerate(model.graph.node) for v in node.output}
    needed = set()

    def visit(value: str) -> None:
        if value not in producer or producer[value] in needed:
            return
        index = producer[value]
        needed.add(index)
        for input_name in model.graph.node[index].input:
            visit(input_name)

    for value in EXTRACTOR_OUTPUTS:
        visit(value)
    shared = {
        value
        for index, node in enumerate(model.graph.node)
        if index not in needed
        for value in node.input
        if value in producer
        and producer[value] in needed
        and value not in EXTRACTOR_OUTPUTS
    }
    if any(model.graph.node[producer[v]].op_type != "Constant" for v in shared):
        raise ValueError("Unexpected extractor intermediate consumed by LightGlue")
    local = {
        value: "serial/" + value
        for index in needed
        for value in model.graph.node[index].output
    }
    body = [
        h.make_node(
            "Gather", ["images", "serial_iteration"], ["serial_image_3d"], axis=0
        ),
        h.make_node(
            "Constant",
            [],
            ["serial_axis"],
            value=h.make_tensor("axis", T.INT64, [1], [0]),
        ),
        h.make_node("Unsqueeze", ["serial_image_3d", "serial_axis"], ["serial_image"]),
    ]
    for index in sorted(needed):
        node = copy.deepcopy(model.graph.node[index])
        for i, value in enumerate(node.input):
            node.input[i] = (
                "serial_image" if value == "images" else local.get(value, value)
            )
        for i, value in enumerate(node.output):
            node.output[i] = local[value]
        body.append(node)
    body.append(h.make_node("Identity", ["serial_condition"], ["serial_condition_out"]))
    for index, value in enumerate(EXTRACTOR_OUTPUTS):
        body.append(
            h.make_node(
                "Squeeze", [local[value], "serial_axis"], [f"serial_scan_{index}"]
            )
        )
    graph = h.make_graph(
        body,
        "SequentialSuperPoint",
        [
            h.make_tensor_value_info("serial_iteration", T.INT64, []),
            h.make_tensor_value_info("serial_condition", T.BOOL, []),
        ],
        [
            h.make_tensor_value_info("serial_condition_out", T.BOOL, []),
            h.make_tensor_value_info("serial_scan_0", T.INT64, [keypoints, 2]),
            h.make_tensor_value_info("serial_scan_1", T.FLOAT, [keypoints, 256]),
        ],
    )
    # Convert only the convolution network, including its three pooling layers.
    # NMS also uses MaxPool, but must stay float32 for exact score comparisons.
    half_indices = {
        i
        for i, node in enumerate(graph.node)
        if node.op_type in {"Conv", "Relu"}
        or (node.op_type == "MaxPool" and node.name.startswith("/extractor/pool"))
    }
    half_outputs = {v for i in half_indices for v in graph.node[i].output}
    half_inputs = {v for i in half_indices for v in graph.node[i].input}
    initializers = {value.name: value for value in model.graph.initializer}
    for value in half_inputs & initializers.keys():
        old = initializers[value]
        old.CopyFrom(
            numpy_helper.from_array(numpy_helper.to_array(old).astype("float16"), value)
        )
    boundary = {
        v
        for i, node in enumerate(graph.node)
        if i not in half_indices
        for v in node.input
    } & half_outputs
    cast_inputs = half_inputs - half_outputs - initializers.keys()
    converted = []
    for i, node in enumerate(graph.node):
        if i not in half_indices:
            converted.append(node)
            continue
        for j, value in enumerate(node.input):
            if value in cast_inputs:
                converted.append(
                    h.make_node("Cast", [value], [value + "/fp16"], to=T.FLOAT16)
                )
                node.input[j] = value + "/fp16"
            elif value in half_outputs:
                node.input[j] = value + "/fp16"
        original_outputs = list(node.output)
        for j, value in enumerate(original_outputs):
            node.output[j] = value + "/fp16"
        converted.append(node)
        for value in original_outputs:
            if value in boundary:
                converted.append(
                    h.make_node("Cast", [value + "/fp16"], [value], to=T.FLOAT)
                )
    del graph.node[:]
    graph.node.extend(converted)
    nodes = [n for n in model.graph.node if any(v in shared for v in n.output)]
    nodes.extend(
        [
            h.make_node(
                "Constant",
                [],
                ["serial_count"],
                value=h.make_tensor("count", T.INT64, [], [2]),
            ),
            h.make_node(
                "Constant",
                [],
                ["serial_start"],
                value=h.make_tensor("start", T.BOOL, [], [True]),
            ),
            h.make_node(
                "Loop", ["serial_count", "serial_start"], EXTRACTOR_OUTPUTS, body=graph
            ),
        ]
    )
    nodes.extend(n for i, n in enumerate(model.graph.node) if i not in needed)
    del model.graph.node[:]
    model.graph.node.extend(nodes)
    del model.graph.value_info[:]
    model.graph.input[0].type.tensor_type.shape.dim[0].dim_value = 2
    model.graph.output[0].type.tensor_type.shape.dim[0].dim_value = 2
    metadata = {
        "hstream.source_sha256": SOURCE_SHA256,
        "hstream.cuda_export": "serial-superpoint-fp16-v1",
    }
    if keypoints != 1024:
        metadata["hstream.superpoint_keypoints"] = str(keypoints)
    if normalization != "legacy-per-axis":
        metadata["hstream.keypoint_normalization"] = normalization
    h.set_model_props(model, metadata)
    save(model, destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--keypoints", type=int, choices=(1024, 2048), default=2048)
    parser.add_argument("--cpu-output", type=Path)
    parser.add_argument(
        "--normalization", choices=("reference", "legacy-per-axis"), default="reference"
    )
    args = parser.parse_args()
    convert(
        args.source,
        args.destination,
        args.keypoints,
        args.cpu_output,
        args.normalization,
    )
