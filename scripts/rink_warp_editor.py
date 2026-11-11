#!/usr/bin/env python3
"""Open the GPU still-frame warp editor without starting video playback."""

import argparse
import base64
import importlib.util
import json
import os
import webbrowser
from pathlib import Path

import yaml


def image_payload(path):
    data = path.read_bytes()
    if len(data) > 256 * 1024 * 1024:
        raise ValueError("Image exceeds the 256 MiB input limit")
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(
            "The launcher accepts PNG stills; other formats can be opened inside the editor"
        )
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--mask", type=Path)
    parser.add_argument("--guides", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--no-open", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    config_root = (
        Path(os.environ["HM_CONFIG_ROOT"])
        if "HM_CONFIG_ROOT" in os.environ
        else root / "configs"
    )
    baseline = yaml.safe_load((config_root / "baseline.yaml").read_text())
    payload = {
        "defaults": baseline["hstream_ui"]["rink_warp"],
        "title": args.image.name,
        "image": image_payload(args.image),
    }
    if args.mask:
        payload["mask"] = image_payload(args.mask)
    if args.guides:
        payload["guides"] = json.loads(args.guides.read_text())
    module_spec = importlib.util.spec_from_file_location(
        "rink_warp_bundle", root / "src/apps/rink-warp-editor/bundle.py"
    )
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    html = module.template(root / "src/apps/rink-warp-editor").replace(
        "@@WARP_BOOTSTRAP@@", json.dumps(payload).replace("<", "\\u003c")
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as handle:
        handle.write(html)
    print(args.output.resolve())
    if not args.no_open:
        webbrowser.open(args.output.resolve().as_uri())


if __name__ == "__main__":
    main()
