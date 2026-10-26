#!/usr/bin/env python3
"""Media rasters must ignore desktop versus headless font hint settings."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="highlight-raster-policy-") as temporary:
        root = Path(temporary)
        outputs = []
        for hint in ("hintnone", "hintfull"):
            config = root / f"{hint}.conf"
            config.write_text(
                '<?xml version="1.0"?>\n'
                '<!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n'
                "<fontconfig><include>/etc/fonts/fonts.conf</include>"
                '<match target="font"><edit name="hintstyle" mode="assign">'
                f"<const>{hint}</const></edit></match></fontconfig>\n"
            )
            output = root / hint
            environment = dict(
                os.environ,
                QT_QPA_PLATFORM="offscreen",
                FONTCONFIG_FILE=str(config),
                CUDA_VISIBLE_DEVICES="",
                HIP_VISIBLE_DEVICES="",
                ROCR_VISIBLE_DEVICES="",
            )
            subprocess.run(
                [str(binary), "--raster-dir", str(output)],
                env=environment,
                check=True,
                timeout=30,
            )
            outputs.append(output)
        for name in ("title.rgba", "matchup.rgba", "text-cue.rgba", "arrow-cue.rgba"):
            assert (outputs[0] / name).read_bytes() == (
                outputs[1] / name
            ).read_bytes(), f"System font hinting changed {name}"
        print("Title, matchup and cue rasters ignore system font hinting")


if __name__ == "__main__":
    main()
