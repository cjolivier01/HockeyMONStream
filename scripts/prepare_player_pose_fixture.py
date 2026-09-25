#!/usr/bin/env python3
"""Offline MMPose/OpenCV/PyTorch oracle for one retained RGB scene and player ROI.

Run in the player-model export environment. The fixture directory must contain
scene.png and fixture.json with roi_xywh and metadata_size. This script never
runs in playback and its pixel/distribution readbacks are validation-only.
"""
import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--export-script', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('player_export', args.export_script)
    export = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(export)
    export.source_paths(args.source_root)
    import cv2
    import numpy as np
    import torch
    from mmpose.datasets.transforms import TopdownAffine
    from mmpose.structures.bbox import bbox_xyxy2cs
    from mmpose.codecs import SimCCLabel
    profile = export.PROFILES['pose']
    if export.digest(args.checkpoint) != profile['sha256']:
        raise ValueError('Fixture checkpoint differs from the pinned pose export checkpoint')
    model, _, _ = export.create_model('pose', args.source_root, args.checkpoint, profile)
    fixture = json.loads((args.fixture/'fixture.json').read_text())
    rgb = cv2.cvtColor(cv2.imread(str(args.fixture/fixture['image'])), cv2.COLOR_BGR2RGB)
    box = np.array(fixture['roi_xywh'], dtype=np.float32)
    factor = np.array([rgb.shape[1], rgb.shape[0]], dtype=np.float32) / fixture['metadata_size']
    xyxy = np.concatenate((box[:2]*factor, (box[:2]+box[2:])*factor)).astype(np.float32)[None]
    center, scale = bbox_xyxy2cs(xyxy, padding=1.25)
    transformed = TopdownAffine(input_size=(192,256), use_udp=False).transform(
        dict(img=rgb, bbox_center=center, bbox_scale=scale))
    patch = transformed['img']
    mean = np.array([123.675,116.28,103.53], dtype=np.float32)
    deviation = np.array([58.395,57.12,57.375], dtype=np.float32)
    tensor = np.ascontiguousarray(((patch.astype(np.float32)-mean)/deviation).transpose(2,0,1)[None])
    tensor.tofile(args.fixture/'input.f32')
    cv2.imwrite(str(args.fixture/'mmpose-crop.png'),cv2.cvtColor(patch,cv2.COLOR_RGB2BGR))
    with torch.inference_mode():
        outputs = export.output_arrays(model(torch.from_numpy(tensor)))
    for index, output in enumerate(outputs):
        output.astype(np.float32).tofile(args.fixture/f'reference_{index}.f32')
    coordinates, scores = SimCCLabel(input_size=(192,256), simcc_split_ratio=2.0, use_dark=False).decode(*outputs)
    metadata = (coordinates / np.array([192,256]) * transformed['input_scale'] +
                transformed['input_center'] - transformed['input_scale'] / 2) / factor
    pose = np.concatenate((metadata, scores[...,None]),axis=-1).astype(np.float32)
    pose.tofile(args.fixture/'reference_pose.f32')
    fixture['reference'] = dict(checkpoint_sha256=export.digest(args.checkpoint),
                              config_sha256=export.digest(args.source_root/profile['config']),
                              image_sha256=export.digest(args.fixture/fixture['image']),
                              torch_version=torch.__version__,opencv_version=cv2.__version__,
                              input_shape=list(tensor.shape),output_shapes=[list(x.shape) for x in outputs],
                              max_confidence=float(scores.max()),min_confidence=float(scores.min()))
    (args.fixture/'fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
    print(json.dumps(fixture['reference'],indent=2))


if __name__ == '__main__':
    main()
