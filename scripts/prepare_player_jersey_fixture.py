#!/usr/bin/env python3
"""Offline PIL/torchvision/PyTorch oracle for a retained hockey jersey crop."""
import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--checkpoint',type=Path,required=True)
    parser.add_argument('--export-script',type=Path,required=True)
    args=parser.parse_args()
    spec=importlib.util.spec_from_file_location('player_export',args.export_script)
    export=importlib.util.module_from_spec(spec);spec.loader.exec_module(export)
    export.source_paths(args.source_root)
    import numpy as np
    import torch
    import PIL
    from PIL import Image
    from strhub.data.module import SceneTextDataModule
    profile=export.PROFILES['jersey']
    if export.digest(args.checkpoint)!=profile['sha256']:
        raise ValueError('Fixture checkpoint differs from the pinned hockey PARSeq checkpoint')
    model,reference,extra=export.create_model('jersey',args.source_root,args.checkpoint,profile)
    fixture=json.loads((args.fixture/'fixture.json').read_text())
    image=Image.open(args.fixture/fixture['image']).convert('RGB')
    x,y,w,h=fixture['crop_xywh']
    crop=image.crop((x,y,x+w,y+h))
    crop.save(args.fixture/'crop.png')
    tensor=SceneTextDataModule.get_transform((32,128))(crop).unsqueeze(0)
    tensor.numpy().tofile(args.fixture/'input.f32')
    with torch.inference_mode():
        logits=reference(tensor)
        wrapped=model(tensor)
    if not torch.allclose(logits,wrapped,rtol=5e-4,atol=1e-4):
        raise ValueError('Fixed export wrapper differs from upstream max_length=2 forward')
    logits.numpy().astype(np.float32).tofile(args.fixture/'reference.f32')
    probabilities=logits.softmax(-1)
    confidence,indices=probabilities.max(-1)
    charset=extra['charset'];text='';probability=1.;terminated=False
    for token,score in zip(indices[0].tolist(),confidence[0].tolist()):
        probability*=score
        if token==0:
            terminated=True
            break
        text+=charset[token]
    if not terminated or not 1<=len(text)<=2 or not all('0'<=x<='9' for x in text):
        text='';probability=0.
    fixture['reference']=dict(text=text,confidence=probability,tokens=indices[0].tolist(),
                              checkpoint_sha256=export.digest(args.checkpoint),image_sha256=export.digest(args.fixture/fixture['image']),
                              torch_version=torch.__version__,pillow_version=PIL.__version__)
    (args.fixture/'fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
    print(json.dumps(fixture['reference'],indent=2))

if __name__=='__main__':
    main()
