#!/usr/bin/env python3
"""Offline PIL/torchvision oracle fixtures for the native GPU jersey crop tests."""
import argparse
import json
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    import numpy as np
    import PIL
    from PIL import Image
    import torch
    from torchvision import transforms as T
    args.output.mkdir(parents=True,exist_ok=True)
    yy,xx=np.indices((769,1027))
    pixels=np.stack((((xx+yy)%2)*255, (xx*17+yy*41)%256, ((xx//3+yy//5)%2)*255),axis=-1).astype(np.uint8)
    image=Image.fromarray(pixels,'RGB')
    image.save(args.output/'scene.png')
    crops=[[0,0,1027,769],[7,9,5,7],[5,10,128,32],[-37,-21,513,257],
           [990,750,71,53],[1027,769,1,1],[500,100,8192,1],[3,100,1,8192],
           [5,17,128,257],[12,2,513,32],[-1,-1,2,2],[0,0,1,1],
           [8,6,17,27],[7,8,256,64],[7,5,127,31],[7,5,129,33]]
    transform=T.Compose([T.Resize((32,128),T.InterpolationMode.BICUBIC),T.ToTensor(),T.Normalize(.5,.5)])
    for index,(x,y,w,h) in enumerate(crops):
        crop=image.crop((x,y,x+w,y+h))
        resized=crop.resize((128,32),Image.Resampling.BICUBIC)
        np.asarray(resized).tofile(args.output/f'pixels_{index}.rgb')
        transform(crop).numpy().astype(np.float32).tofile(args.output/f'input_{index}.f32')
    (args.output/'fixtures.json').write_text(json.dumps(dict(image='scene.png',crops=crops,pillow=PIL.__version__,
                                                            torch=torch.__version__),indent=2)+'\n')
    print(f'Created {len(crops)} PIL/torchvision reference crops in {args.output}')

if __name__=='__main__':
    main()
