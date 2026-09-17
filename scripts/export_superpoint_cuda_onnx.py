#!/usr/bin/env python3
"""Convert the pinned SuperPoint/LightGlue graph for native-resolution CUDA.

Requires onnx==1.20.1 and numpy. No PyTorch/checkpoint re-export is needed.
The public float32 input/output contract and LightGlue weights are unchanged.
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


def convert(source: Path, destination: Path) -> None:
    if onnx.__version__ != "1.20.1":
        raise ValueError("Use onnx==1.20.1 for a reproducible serialization")
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("Input is not the pinned upstream SuperPoint/LightGlue graph")
    model = onnx.load(source)
    producer = {v: i for i, node in enumerate(model.graph.node) for v in node.output}
    needed = set()

    def visit(value: str) -> None:
        if value not in producer or producer[value] in needed:
            return
        index = producer[value]
        needed.add(index)
        for value in model.graph.node[index].input:
            visit(value)

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
            h.make_tensor_value_info("serial_scan_0", T.INT64, [1024, 2]),
            h.make_tensor_value_info("serial_scan_1", T.FLOAT, [1024, 256]),
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
    h.set_model_props(
        model,
        {
            "hstream.source_sha256": SOURCE_SHA256,
            "hstream.cuda_export": "serial-superpoint-fp16-v1",
        },
    )
    onnx.checker.check_model(model)
    onnx.save(model, destination)
    print(hashlib.sha256(destination.read_bytes()).hexdigest(), destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    convert(args.source, args.destination)
