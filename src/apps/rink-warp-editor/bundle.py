#!/usr/bin/env python3
"""Build the identical self-contained editor for Qt and browser launchers."""

import argparse
from pathlib import Path

ASSETS = (
    "editor.html",
    "editor.css",
    "warp-icons.css",
    "warp-geometry.js",
    "warp-gpu.js",
    "editor.js",
)


def template(root):
    read = lambda name: (root / name).read_text()
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        "<title>Rink warp</title><style>"
        + read("editor.css")
        + "\n"
        + read("warp-icons.css")
        + "</style></head><body>"
        + read("editor.html")
        + '<script type="application/json" id="warp-bootstrap">@@WARP_BOOTSTRAP@@</script>'
        + "".join(
            "<script>\n" + read(name) + "\n</script>"
            for name in ASSETS
            if name.endswith(".js")
        )
        + "</body></html>\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--header", action="store_true")
    args = parser.parse_args()
    result = template(args.root)
    if args.header:
        if ')RINK_WARP_ASSET"' in result:
            raise ValueError("Unexpected raw-string delimiter in editor asset")
        result = (
            '#pragma once\ninline constexpr char kRinkWarpEditorHtml[] = R"RINK_WARP_ASSET('
            + result
            + ')RINK_WARP_ASSET";\n'
        )
    args.output.write_text(result)


if __name__ == "__main__":
    main()
